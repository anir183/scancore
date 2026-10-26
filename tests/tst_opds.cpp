// OPDS catalogues as the reader's own servers: feeds (Atom and JSON), the
// catalogue's sections, search, series and their books, pages streamed with
// OPDS-PSE or read from the fetched file, and EPUB books for the novels.
// The catalogue is tests/fixtures/opds on opds.test (Http's test handler).

#include "core/Database.h"
#include "core/Http.h"
#include "sources/servers/Opds.h"
#include "sources/servers/Servers.h"

#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

using namespace sc;
using namespace sc::servers;

namespace {

QByteArray fixture(const QString &name) {
    QFile f(QStringLiteral(SCANCORE_SOURCE_DIR "/tests/fixtures/") + name);
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing fixture %s", qPrintable(name));
    return f.readAll();
}

} // namespace

class Opds : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QMutex m_mutex;
    QStringList m_asked;
    QList<QByteArray> m_ranges;
    std::unique_ptr<comics::Source> m_comics;
    std::unique_ptr<novels::Source> m_novels;

    QStringList asked() {
        QMutexLocker lock(&m_mutex);
        return m_asked;
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        // Files fetched by an earlier run would be read from the cache.
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/opds")).removeRecursively();
        QVERIFY(m_dir.isValid());
        Database::open(m_dir.filePath(QStringLiteral("library.db")));
        Http::setTestHandler([this](const Request &request) -> std::optional<Response> {
            if (request.url.host() != QLatin1String("opds.test"))
                return std::nullopt;
            const auto path = request.url.path();
            QByteArray range;
            for (const auto &[k, v] : request.headers)
                if (k.compare("Range", Qt::CaseInsensitive) == 0)
                    range = v;
            {
                QMutexLocker lock(&m_mutex);
                m_asked.append(request.url.toString());
                if (!range.isEmpty())
                    m_ranges.append(range);
            }
            Response r;
            r.status = 200;
            r.finalUrl = request.url;
            if (path.startsWith(QLatin1String("/opds/pse/"))) {
                r.body = fixture(QStringLiteral("smoke/smoke-comic.cbz")).left(64);
                r.contentType = "image/jpeg";
            } else if (path == QLatin1String("/opds/search")) {
                r.body = fixture(QStringLiteral("opds/search.xml"));
            } else if (path == QLatin1String("/books/smoke.cbz")) {
                // Ranges, as Komga and Calibre-web answer them.
                const auto whole = fixture(QStringLiteral("smoke/smoke-comic.cbz"));
                if (range.startsWith("bytes=")) {
                    const auto parts = range.mid(6).split('-');
                    const qint64 from = parts.value(0).toLongLong();
                    const qint64 to = std::min<qint64>(parts.value(1).toLongLong(), whole.size() - 1);
                    if (from >= whole.size()) {
                        r.status = 416;
                    } else {
                        r.status = 206;
                        r.body = whole.mid(from, to - from + 1);
                        r.headers.append({"Content-Range", QByteArray("bytes ") + QByteArray::number(from) + '-' + QByteArray::number(to) + '/' +
                                                               QByteArray::number(whole.size())});
                    }
                } else {
                    r.body = whole;
                }
            } else if (path == QLatin1String("/books/novel.epub")) {
                // No ranges: the whole file at once.
                r.body = fixture(QStringLiteral("smoke/smoke-novel.epub"));
            } else if (path.startsWith(QLatin1String("/opds/")) || path.startsWith(QLatin1String("/opds2/"))) {
                r.body = fixture(QStringLiteral("opds/") + path.section(QLatin1Char('/'), -1));
            } else {
                r.status = 404;
            }
            return r;
        });
        Server server;
        server.id = QStringLiteral("t1");
        server.kind = QStringLiteral("opds");
        server.label = QStringLiteral("Test catalogue");
        server.url = QStringLiteral("http://opds.test/opds/root.xml");
        save({server});
        m_comics = createSource(server);
        m_novels = createNovelSource(server);
        QVERIFY(m_comics && m_novels);
    }
    void cleanupTestCase() {
        Http::setTestHandler({});
        m_comics.reset();
        m_novels.reset();
    }

    void parsesAtom() {
        const auto feed = opds::parse(fixture(QStringLiteral("opds/series.xml")), QUrl(QStringLiteral("http://opds.test/opds/series.xml")));
        QCOMPARE(feed.title, QStringLiteral("Smoke Series"));
        QCOMPARE(feed.entries.size(), 2);
        const auto &v1 = feed.entries.first();
        QVERIFY(v1.isBook());
        QVERIFY(v1.stream());
        QCOMPARE(v1.stream()->pseCount, 3);
        // Templates keep their braces; links are absolute.
        QCOMPARE(v1.stream()->href, QStringLiteral("http://opds.test/opds/pse/1?page={pageNumber}&width={maxWidth}"));
        QCOMPARE(v1.cover, QStringLiteral("http://opds.test/covers/vol-1.png"));
        QVERIFY(v1.acquisition(QStringLiteral("comic")));
        QVERIFY(!v1.acquisition(QStringLiteral("novel")));
        const auto root = opds::parse(fixture(QStringLiteral("opds/root.xml")), QUrl(QStringLiteral("http://opds.test/opds/root.xml")));
        QCOMPARE(root.searchDescription, QStringLiteral("http://opds.test/opds/search-desc.xml"));
        QCOMPARE(root.entries.first().navigation(), QStringLiteral("http://opds.test/opds/libraries.xml"));
        QCOMPARE(root.entries.at(1).navigation(), QStringLiteral("http://opds.test/opds/series-list.xml"));
    }

    void parsesJson() {
        const auto feed = opds::parse(fixture(QStringLiteral("opds/root.json")), QUrl(QStringLiteral("http://opds.test/opds2/root.json")));
        QCOMPARE(feed.title, QStringLiteral("JSON catalogue"));
        QCOMPARE(feed.searchTemplate, QStringLiteral("http://opds.test/opds2/search{?query}"));
        QCOMPARE(feed.entries.size(), 2);
        QCOMPARE(feed.entries.first().navigation(), QStringLiteral("http://opds.test/opds2/new.json"));
        const auto &book = feed.entries.at(1);
        QCOMPARE(book.title, QStringLiteral("JSON Book"));
        QCOMPARE(book.author, QStringLiteral("First, Second"));
        QCOMPARE(book.cover, QStringLiteral("http://opds.test/covers/json.png"));
        QCOMPARE(book.genres, QStringList{QStringLiteral("Fantasy")});
        QVERIFY(book.acquisition(QStringLiteral("novel")));
    }

    void searchTemplates() {
        QCOMPARE(opds::searchUrl(QStringLiteral("http://a.test/s?q={searchTerms}&p={startPage?}"), QStringLiteral("one piece")),
                 QStringLiteral("http://a.test/s?q=one%20piece&p="));
        QCOMPARE(opds::searchUrl(QStringLiteral("http://a.test/search{?query}"), QStringLiteral("x&y")), QStringLiteral("http://a.test/search?query=x%26y"));
        QCOMPARE(opds::searchUrl(QStringLiteral("http://a.test/search?lang=en{?query}"), QStringLiteral("z")), QStringLiteral("http://a.test/search?lang=en&query=z"));
        QCOMPARE(opds::kindOf(QStringLiteral("application/x-cbr"), {}), QStringLiteral("comic"));
        QCOMPARE(opds::kindOf({}, QStringLiteral("http://a.test/b/book.kepub")), QStringLiteral("novel"));
        QCOMPARE(opds::kindOf(QStringLiteral("application/x-mobipocket-ebook"), {}), QString());
    }

    // The catalogue's newest feed, a page at a time: folders and comic books,
    // never the EPUB (that one is a novel).
    void browsesAndPages() {
        const auto first = m_comics->browse(QStringLiteral("popular"), 1);
        QCOMPARE(first.items.size(), 1);
        QCOMPARE(first.items.first().title, QStringLiteral("Smoke Series"));
        QCOMPARE(first.items.first().type, QStringLiteral("Folder"));
        QCOMPARE(first.items.first().cover, QStringLiteral("http://opds.test/covers/series-1.png"));
        QVERIFY(first.hasNextPage);
        const auto second = m_comics->browse(QStringLiteral("popular"), 2);
        QCOMPARE(second.items.size(), 1);
        QCOMPARE(second.items.first().title, QStringLiteral("Second Page Comic"));
        QVERIFY(second.items.first().url.startsWith(QLatin1String("opds-book:")));
        QVERIFY(!second.hasNextPage);
        QVERIFY(m_comics->browse(QStringLiteral("popular"), 3).items.isEmpty());
    }

    void sections() {
        const auto filters = m_comics->filters();
        QCOMPARE(filters.size(), 1);
        QStringList labels;
        for (const auto &v : filters.first().toObject().value(QStringLiteral("values")).toArray())
            labels.append(v.toObject().value(QStringLiteral("label")).toString());
        QVERIFY2(labels.contains(QStringLiteral("Libraries")), qPrintable(labels.join(QLatin1Char('|'))));
        QVERIFY(labels.contains(QStringLiteral("Libraries › Manga")));
        QVERIFY(labels.contains(QStringLiteral("Latest series")));
        // Picking one lists its feed.
        auto picked = filters;
        auto f = picked.first().toObject();
        f.insert(QStringLiteral("state"), int(labels.indexOf(QStringLiteral("Libraries"))));
        picked[0] = f;
        const auto listing = m_comics->browse(QStringLiteral("search"), 1, QString(), picked);
        QCOMPARE(listing.items.size(), 2);
        QCOMPARE(listing.items.first().title, QStringLiteral("Manga"));
    }

    void searches() {
        const auto found = m_comics->search(QStringLiteral("smoke"));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().url, QStringLiteral("http://opds.test/opds/series.xml"));
        QVERIFY(asked().contains(QStringLiteral("http://opds.test/opds/search?q=smoke")));
    }

    void chaptersAndPages() {
        const auto details = m_comics->details(QStringLiteral("http://opds.test/opds/series.xml"));
        QVERIFY(details);
        QCOMPARE(details->title, QStringLiteral("Smoke Series"));
        QCOMPARE(details->cover, QStringLiteral("http://opds.test/covers/vol-1.png"));
        const auto chapters = m_comics->chapters(QStringLiteral("http://opds.test/opds/series.xml"));
        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters.first().title, QStringLiteral("Vol. 1"));
        QCOMPARE(chapters.first().number, QStringLiteral("1"));
        QCOMPARE(chapters.first().released, QStringLiteral("2026-10-01"));
        // Streamed pages: no file fetched.
        const auto streamed = m_comics->panels(chapters.first().url);
        QCOMPARE(streamed.size(), 3);
        QCOMPARE(streamed.at(2).url, QStringLiteral("http://opds.test/opds/pse/1?page=2&width=2000"));
        // A file: fetched by ranges, then its pages read from it.
        const auto pages = m_comics->panels(chapters.at(1).url);
        QCOMPARE(pages.size(), 3);
        {
            QMutexLocker lock(&m_mutex);
            QVERIFY(!m_ranges.isEmpty());
            QCOMPARE(m_ranges.first(), QByteArray("bytes=0-8388607"));
        }
        // Once: the next time comes from the cache.
        const int before = int(asked().size());
        QCOMPARE(m_comics->panels(chapters.at(1).url).size(), 3);
        QCOMPARE(int(asked().size()), before);
    }

    void folderOfFolders() {
        try {
            m_comics->chapters(QStringLiteral("http://opds.test/opds/libraries.xml"));
            QFAIL("a folder of series has no chapters");
        } catch (const Error &e) {
            QVERIFY(e.message().contains(QLatin1String("Section")));
        }
    }

    // EPUB books on the novels' side: its chapters come from the file.
    void novelBooks() {
        const auto listing = m_novels->browse(QStringLiteral("popular"), 1, QString(), {});
        QCOMPARE(listing.items.size(), 2);
        const auto &book = listing.items.at(1);
        QCOMPARE(book.title, QStringLiteral("A Novel Book"));
        QCOMPARE(book.author, QStringLiteral("Novel Author"));
        QList<novels::ChapterRef> refs;
        const auto opened = m_novels->book(book.url, refs);
        QCOMPARE(opened.title, QStringLiteral("A Novel Book"));
        QCOMPARE(refs.size(), 1);
        QString title;
        const auto blocks = m_novels->chapter(refs.first().url, title);
        QVERIFY(blocks.size() >= 12);
        // A folder with no EPUB in it says so.
        QList<novels::ChapterRef> none;
        QVERIFY_THROWS_EXCEPTION(Error, m_novels->book(QStringLiteral("http://opds.test/opds/series.xml"), none));
    }

    void refusedLogin() {
        Http::setTestHandler([](const Request &request) -> std::optional<Response> {
            if (request.url.host() != QLatin1String("locked.test"))
                return std::nullopt;
            Response r;
            r.status = 401;
            return r;
        });
        Server locked;
        locked.id = QStringLiteral("t2");
        locked.kind = QStringLiteral("opds");
        locked.label = QStringLiteral("Locked");
        locked.url = QStringLiteral("http://locked.test/opds");
        auto source = createSource(locked);
        try {
            source->browse(QStringLiteral("popular"), 1);
            QFAIL("a refused login fails");
        } catch (const Error &e) {
            QVERIFY(e.message().contains(QLatin1String("refused the login")));
        }
    }
};

QTEST_GUILESS_MAIN(Opds)
#include "tst_opds.moc"
