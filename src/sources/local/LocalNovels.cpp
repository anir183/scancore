#include "Ebooks.h"
#include "Local.h"
#include "LocalSources.h"

#include "core/Html.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QUrl>
#include <QXmlStreamReader>

namespace sc::local {

namespace {

// An EPUB's package: what it says about itself and its reading order.
struct Epub {
    QString title;
    QString author;
    QString description;
    QString coverEntry;
    // Entry paths inside the archive, in reading order, with their titles
    // from the table of contents where it names them.
    QStringList spine;
    QHash<QString, QString> titles;
};

QString joinPath(const QString &dir, const QString &href) {
    const auto clean = QUrl::fromPercentEncoding(href.section(QLatin1Char('#'), 0, 0).toUtf8());
    return QDir::cleanPath(dir.isEmpty() ? clean : dir + QLatin1Char('/') + clean);
}

// Titles from an EPUB 3 contents page (<nav epub:type="toc">).
void readNav(const QString &archive, const QString &navPath, QHash<QString, QString> &titles) {
    const auto data = zipEntry(archive, navPath);
    if (data.isEmpty())
        return;
    HtmlDocument doc(data);
    const auto dir = QFileInfo(navPath).path() == QLatin1String(".") ? QString() : QFileInfo(navPath).path();
    for (const auto &link : doc.select(QStringLiteral("nav a[href]"))) {
        const auto entry = joinPath(dir, link.attr("href"));
        if (!titles.contains(entry) && !link.textTrim().isEmpty())
            titles.insert(entry, link.textTrim());
    }
}

// Titles from an EPUB 2 table of contents (toc.ncx).
void readNcx(const QString &archive, const QString &ncxPath, QHash<QString, QString> &titles) {
    QXmlStreamReader xml(zipEntry(archive, ncxPath));
    const auto dir = QFileInfo(ncxPath).path() == QLatin1String(".") ? QString() : QFileInfo(ncxPath).path();
    QString label;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("text"))
            label = xml.readElementText().simplified();
        else if (xml.name() == QLatin1String("content")) {
            const auto entry = joinPath(dir, xml.attributes().value(QStringLiteral("src")).toString());
            if (!titles.contains(entry) && !label.isEmpty())
                titles.insert(entry, label);
        }
    }
}

Epub readEpub(const QString &archive) {
    if (!QFileInfo::exists(archive))
        throw Error(QStringLiteral("%1 is no longer on this device.").arg(QDir::toNativeSeparators(archive)), 404);
    QString opfPath;
    {
        QXmlStreamReader xml(zipEntry(archive, QStringLiteral("META-INF/container.xml")));
        while (!xml.atEnd() && opfPath.isEmpty())
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("rootfile"))
                opfPath = xml.attributes().value(QStringLiteral("full-path")).toString();
    }
    if (opfPath.isEmpty())
        throw Error(QStringLiteral("%1 is not an EPUB book.").arg(QFileInfo(archive).fileName()));
    const auto dir = QFileInfo(opfPath).path() == QLatin1String(".") ? QString() : QFileInfo(opfPath).path();

    Epub book;
    struct Item {
        QString href;
        QString type;
        QString properties;
    };
    QHash<QString, Item> manifest;
    QStringList spineIds;
    QString coverId, ncxId;
    QXmlStreamReader xml(zipEntry(archive, opfPath));
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto name = xml.name();
        const auto a = xml.attributes();
        if (name == QLatin1String("title") && book.title.isEmpty())
            book.title = xml.readElementText().simplified();
        else if (name == QLatin1String("creator") && book.author.isEmpty())
            book.author = xml.readElementText().simplified();
        else if (name == QLatin1String("description") && book.description.isEmpty())
            book.description = HtmlDocument(xml.readElementText()).root().textTrim();
        else if (name == QLatin1String("meta") && a.value(QStringLiteral("name")) == QLatin1String("cover"))
            coverId = a.value(QStringLiteral("content")).toString();
        else if (name == QLatin1String("item"))
            manifest.insert(a.value(QStringLiteral("id")).toString(),
                            {joinPath(dir, a.value(QStringLiteral("href")).toString()), a.value(QStringLiteral("media-type")).toString(),
                             a.value(QStringLiteral("properties")).toString()});
        else if (name == QLatin1String("spine"))
            ncxId = a.value(QStringLiteral("toc")).toString();
        else if (name == QLatin1String("itemref"))
            spineIds.append(a.value(QStringLiteral("idref")).toString());
    }
    for (auto it = manifest.begin(); it != manifest.end(); ++it) {
        if (it->properties.contains(QLatin1String("cover-image")) || (book.coverEntry.isEmpty() && it.key() == coverId))
            book.coverEntry = it->href;
        if (it->properties.contains(QLatin1String("nav")))
            readNav(archive, it->href, book.titles);
    }
    if (manifest.contains(ncxId))
        readNcx(archive, manifest.value(ncxId).href, book.titles);
    for (const auto &id : spineIds) {
        const auto item = manifest.value(id);
        if (!item.href.isEmpty() && (item.type.contains(QLatin1String("html")) || item.type.isEmpty()))
            book.spine.append(item.href);
    }
    if (book.title.isEmpty())
        book.title = titleFrom(archive);
    return book;
}

QString coverUrl(const QString &archive, const Epub &book) {
    if (book.coverEntry.isEmpty())
        return {};
    const auto file = cacheFile(archive, QStringLiteral("cover.") + QFileInfo(book.coverEntry).suffix());
    if (!QFileInfo::exists(file)) {
        const auto data = zipEntry(archive, book.coverEntry);
        QSaveFile out(file);
        if (data.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
            return {};
    }
    return QUrl::fromLocalFile(file).toString();
}

// FB2 and MOBI books are read whole (a few MB at most), so the last ones
// opened stay in memory while their chapters are read one by one.
struct CachedEbook {
    QString path;
    QDateTime modified;
    Ebook book;
};
QMutex g_ebooksLock;
QList<CachedEbook> g_ebooks;

Ebook ebookAt(const QString &path) {
    if (!QFileInfo::exists(path))
        throw Error(QStringLiteral("%1 is no longer on this device.").arg(QDir::toNativeSeparators(path)), 404);
    const auto modified = QFileInfo(path).lastModified();
    {
        QMutexLocker lock(&g_ebooksLock);
        for (int i = 0; i < g_ebooks.size(); ++i)
            if (g_ebooks[i].path == path && g_ebooks[i].modified == modified) {
                g_ebooks.move(i, 0);
                return g_ebooks.first().book;
            }
    }
    auto book = readEbook(path);
    if (book.title.isEmpty())
        book.title = titleFrom(path);
    QMutexLocker lock(&g_ebooksLock);
    g_ebooks.prepend({path, modified, book});
    while (g_ebooks.size() > 3)
        g_ebooks.removeLast();
    return book;
}

QString ebookCover(const QString &path, const Ebook &book) {
    if (book.cover.isEmpty())
        return {};
    const auto file = cacheFile(path, QStringLiteral("cover.") + book.coverSuffix);
    if (!QFileInfo::exists(file)) {
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(book.cover) != book.cover.size() || !out.commit())
            return {};
    }
    return QUrl::fromLocalFile(file).toString();
}

// The book files the novels list: EPUB, FB2 and MOBI.
const QStringList &bookFilters() {
    static const QStringList filters = [] {
        QStringList out;
        for (const auto ext : {"epub", "fb2", "fb2.zip", "mobi", "azw", "azw3", "prc"})
            out << QStringLiteral("*.") + QLatin1String(ext) << QStringLiteral("*.") + QString::fromLatin1(ext).toUpper();
        return out;
    }();
    return filters;
}

class LocalNovelSource : public novels::Source {
public:
    using novels::Source::Source;

    bool owns(const QUrl &url) const override { return url.isLocalFile(); }

    // Books in the chosen folders and one folder below.
    QList<novels::Book> search(const QString &query) override {
        const auto words = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QStringList files;
        for (const auto &root : folders()) {
            QDirIterator it(root, bookFilters(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const auto path = it.next();
                if (QDir(root).relativeFilePath(path).count(QLatin1Char('/')) <= 1)
                    files.append(path);
            }
        }
        naturalSort(files);
        QList<novels::Book> out;
        for (const auto &path : files) {
            const auto title = titleFrom(path);
            const auto lower = title.toLower();
            if (!std::all_of(words.begin(), words.end(), [&](const QString &w) { return lower.contains(w); }))
                continue;
            novels::Book book;
            book.source = key();
            book.url = QUrl::fromLocalFile(path).toString();
            book.title = title;
            try {
                if (isEbook(path)) {
                    const auto ebook = readEbook(path, true);
                    if (!ebook.title.isEmpty())
                        book.title = ebook.title;
                    book.author = ebook.author;
                    book.cover = ebookCover(path, ebook);
                    out.append(book);
                    continue;
                }
                const auto epub = readEpub(path);
                book.title = epub.title;
                book.author = epub.author;
                book.cover = coverUrl(path, epub);
                book.chapterCount = int(epub.spine.size());
            } catch (const Error &) {
            }
            out.append(book);
        }
        return out;
    }

    novels::Book book(const QString &url, QList<novels::ChapterRef> &chapters) override {
        const auto path = QUrl(url).toLocalFile();
        if (isEbook(path)) {
            const auto ebook = ebookAt(path);
            for (int i = 0; i < ebook.chapters.size(); ++i) {
                novels::ChapterRef ref;
                ref.url = QUrl::fromLocalFile(path).toString() + QStringLiteral("#ch=%1").arg(i);
                ref.title = ebook.chapters[i].title;
                ref.number = QString::number(i + 1);
                chapters.append(ref);
            }
            novels::Book book;
            book.source = key();
            book.url = url;
            book.title = ebook.title;
            book.author = ebook.author;
            book.description = ebook.description;
            book.cover = ebookCover(path, ebook);
            book.chapterCount = int(chapters.size());
            return book;
        }
        const auto epub = readEpub(path);
        int untitled = 0;
        for (const auto &entry : epub.spine) {
            novels::ChapterRef ref;
            ref.url = QUrl::fromLocalFile(path).toString() + QLatin1Char('#') + QString::fromLatin1(QUrl::toPercentEncoding(entry));
            ref.title = epub.titles.value(entry);
            if (ref.title.isEmpty())
                ref.title = QStringLiteral("Part %1").arg(++untitled);
            ref.number = QString::number(chapters.size() + 1);
            chapters.append(ref);
        }
        novels::Book book;
        book.source = key();
        book.url = url;
        book.title = epub.title;
        book.author = epub.author;
        book.description = epub.description;
        book.cover = coverUrl(path, epub);
        book.chapterCount = int(chapters.size());
        return book;
    }

    QList<novels::Block> chapter(const QString &url, QString &title) override {
        const QUrl parsed(url);
        const auto path = parsed.toLocalFile();
        const auto entry = QUrl::fromPercentEncoding(parsed.fragment(QUrl::FullyEncoded).toUtf8());
        QByteArray data;
        if (isEbook(path)) {
            const auto ebook = ebookAt(path);
            const int index = entry.startsWith(QLatin1String("ch=")) ? entry.mid(3).toInt() : -1;
            if (index < 0 || index >= ebook.chapters.size())
                throw Error(QStringLiteral("This part of %1 could not be read.").arg(QFileInfo(path).fileName()));
            data = ebook.chapters[index].html;
        } else {
            data = zipEntry(path, entry);
        }
        if (data.isEmpty())
            throw Error(QStringLiteral("This part of %1 could not be read.").arg(QFileInfo(path).fileName()));
        HtmlDocument doc(data);
        // The first heading is the chapter's title, shown above the text.
        auto heading = doc.first(QStringLiteral("h1, h2, h3"));
        if (heading) {
            title = heading.textTrim();
            heading.remove();
        }
        return novels::textBlocks(doc.body());
    }
};

} // namespace

std::unique_ptr<novels::Source> createNovelSource(const novels::SourceConfig &config) {
    return std::make_unique<LocalNovelSource>(config);
}

} // namespace sc::local
