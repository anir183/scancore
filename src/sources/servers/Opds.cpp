#include "Opds.h"

#include "Servers.h"
#include "core/Database.h"
#include "sources/local/LocalSources.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace sc::servers::opds {

namespace {

const QString kAcquisition = QStringLiteral("http://opds-spec.org/acquisition");
const QString kPseStream = QStringLiteral("http://vaemendis.net/opds-pse/stream");
const QString kPseNs = QStringLiteral("http://vaemendis.net/opds-pse/ns");
// What a chapter's address carries after "#" when its pages stream (PSE).
const QString kPseMark = QStringLiteral("scancore-pse=");

QString resolve(const QUrl &base, const QString &href) {
    if (href.isEmpty())
        return {};
    // Templates keep their braces (QUrl would escape them).
    if (href.contains(QLatin1Char('{'))) {
        if (href.contains(QLatin1String("://")))
            return href;
        const auto root = base.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        if (href.startsWith(QLatin1Char('/')))
            return root + href;
        auto dir = base.adjusted(QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
        return dir + href;
    }
    return base.resolved(QUrl(href)).toString();
}

bool isNavigationType(const QString &type) {
    return type.contains(QLatin1String("profile=opds-catalog")) || type.startsWith(QLatin1String("application/atom+xml")) ||
           type.startsWith(QLatin1String("application/opds+json"));
}

// ------------------------------------------------------------------ Atom

Feed parseAtom(const QByteArray &body, const QUrl &base) {
    Feed feed;
    QXmlStreamReader xml(body);
    Entry entry;
    bool inEntry = false, inAuthor = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("entry")) {
                entry = Entry();
                inEntry = true;
            } else if (name == QLatin1String("author")) {
                inAuthor = true;
            } else if (name == QLatin1String("name") && inAuthor && inEntry) {
                const auto text = xml.readElementText().trimmed();
                entry.author = entry.author.isEmpty() ? text : entry.author + QStringLiteral(", ") + text;
            } else if (name == QLatin1String("title")) {
                const auto text = xml.readElementText().trimmed();
                if (inEntry)
                    entry.title = text;
                else if (feed.title.isEmpty())
                    feed.title = text;
            } else if (inEntry && name == QLatin1String("id")) {
                entry.id = xml.readElementText().trimmed();
            } else if (inEntry && name == QLatin1String("updated")) {
                entry.updated = xml.readElementText().trimmed();
            } else if (inEntry && (name == QLatin1String("summary") || name == QLatin1String("content"))) {
                const auto text = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                if (entry.summary.isEmpty() || name == QLatin1String("summary"))
                    entry.summary = text;
            } else if (inEntry && name == QLatin1String("category")) {
                const auto a = xml.attributes();
                const auto label = a.value(QStringLiteral("label")).toString();
                const auto term = label.isEmpty() ? a.value(QStringLiteral("term")).toString() : label;
                if (!term.isEmpty())
                    entry.genres.append(term);
            } else if (name == QLatin1String("link")) {
                const auto a = xml.attributes();
                Link link;
                link.rel = a.value(QStringLiteral("rel")).toString();
                link.href = resolve(base, a.value(QStringLiteral("href")).toString());
                link.type = a.value(QStringLiteral("type")).toString();
                link.title = a.value(QStringLiteral("title")).toString();
                link.pseCount = a.value(kPseNs, QStringLiteral("count")).toInt();
                if (!link.pseCount)
                    link.pseCount = a.value(QStringLiteral("pse:count")).toInt();
                if (inEntry) {
                    if (link.rel == QLatin1String("http://opds-spec.org/image") || link.rel == QLatin1String("http://opds-spec.org/cover") ||
                        link.rel == QLatin1String("x-stanza-cover-image"))
                        entry.cover = link.href;
                    else if ((link.rel == QLatin1String("http://opds-spec.org/image/thumbnail") ||
                              link.rel == QLatin1String("x-stanza-cover-image-thumbnail")) && entry.cover.isEmpty())
                        entry.cover = link.href;
                    entry.links.append(link);
                } else if (link.rel == QLatin1String("next")) {
                    feed.next = link.href;
                } else if (link.rel == QLatin1String("search")) {
                    if (link.type.contains(QLatin1String("opensearchdescription")))
                        feed.searchDescription = link.href;
                    else if (link.href.contains(QLatin1String("{searchTerms}")))
                        feed.searchTemplate = link.href;
                }
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("entry")) {
                inEntry = false;
                feed.entries.append(entry);
            } else if (xml.name() == QLatin1String("author")) {
                inAuthor = false;
            }
        }
    }
    return feed;
}

// ------------------------------------------------------------------ JSON

QString jsonAuthor(const QJsonValue &v) {
    if (v.isString())
        return v.toString();
    if (v.isObject())
        return v.toObject().value(QStringLiteral("name")).toString();
    QStringList names;
    for (const auto &a : v.toArray())
        if (const auto n = jsonAuthor(a); !n.isEmpty())
            names.append(n);
    return names.join(QStringLiteral(", "));
}

Link jsonLink(const QJsonObject &o, const QUrl &base) {
    Link link;
    const auto rel = o.value(QStringLiteral("rel"));
    link.rel = rel.isArray() ? rel.toArray().first().toString() : rel.toString();
    link.href = resolve(base, o.value(QStringLiteral("href")).toString());
    link.type = o.value(QStringLiteral("type")).toString();
    link.title = o.value(QStringLiteral("title")).toString();
    return link;
}

void jsonPublications(const QJsonArray &list, const QUrl &base, Feed &feed) {
    for (const auto &v : list) {
        const auto p = v.toObject();
        const auto meta = p.value(QStringLiteral("metadata")).toObject();
        Entry e;
        e.id = meta.value(QStringLiteral("identifier")).toString();
        e.title = meta.value(QStringLiteral("title")).toString();
        e.author = jsonAuthor(meta.value(QStringLiteral("author")));
        e.summary = meta.value(QStringLiteral("description")).toString();
        e.updated = meta.value(QStringLiteral("modified")).toString();
        for (const auto &s : meta.value(QStringLiteral("subject")).toArray())
            e.genres.append(s.isObject() ? s.toObject().value(QStringLiteral("name")).toString() : s.toString());
        for (const auto &l : p.value(QStringLiteral("links")).toArray())
            e.links.append(jsonLink(l.toObject(), base));
        for (const auto &i : p.value(QStringLiteral("images")).toArray())
            if (e.cover.isEmpty())
                e.cover = resolve(base, i.toObject().value(QStringLiteral("href")).toString());
        feed.entries.append(e);
    }
}

void jsonNavigation(const QJsonArray &list, const QUrl &base, Feed &feed) {
    for (const auto &v : list) {
        const auto o = v.toObject();
        Entry e;
        e.title = o.value(QStringLiteral("title")).toString();
        e.id = o.value(QStringLiteral("href")).toString();
        auto link = jsonLink(o, base);
        if (link.rel.isEmpty())
            link.rel = QStringLiteral("subsection");
        if (link.type.isEmpty())
            link.type = QStringLiteral("application/opds+json");
        e.links.append(link);
        feed.entries.append(e);
    }
}

Feed parseJson(const QByteArray &body, const QUrl &base) {
    Feed feed;
    const auto root = QJsonDocument::fromJson(body).object();
    feed.title = root.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("title")).toString();
    for (const auto &v : root.value(QStringLiteral("links")).toArray()) {
        const auto link = jsonLink(v.toObject(), base);
        if (link.rel == QLatin1String("next"))
            feed.next = link.href;
        else if (link.rel == QLatin1String("search"))
            feed.searchTemplate = link.href;
    }
    jsonNavigation(root.value(QStringLiteral("navigation")).toArray(), base, feed);
    jsonPublications(root.value(QStringLiteral("publications")).toArray(), base, feed);
    for (const auto &g : root.value(QStringLiteral("groups")).toArray()) {
        const auto group = g.toObject();
        jsonNavigation(group.value(QStringLiteral("navigation")).toArray(), base, feed);
        jsonPublications(group.value(QStringLiteral("publications")).toArray(), base, feed);
    }
    return feed;
}

} // namespace

QString kindOf(const QString &type, const QString &href) {
    const auto t = type.toLower();
    const auto path = QUrl(href).path().toLower();
    if (t.contains(QLatin1String("epub")) || path.endsWith(QLatin1String(".epub")) || path.endsWith(QLatin1String(".kepub")))
        return QStringLiteral("novel");
    if (t.contains(QLatin1String("comicbook")) || t.contains(QLatin1String("x-cbz")) || t.contains(QLatin1String("x-cbr")) ||
        t.contains(QLatin1String("x-cb7")) || t.contains(QLatin1String("pdf")) || t == QLatin1String("application/zip") ||
        t == QLatin1String("application/x-zip-compressed") || t.contains(QLatin1String("rar")) || t.contains(QLatin1String("7z")))
        return QStringLiteral("comic");
    for (const auto ext : {".cbz", ".cbr", ".cb7", ".cbt", ".pdf", ".zip", ".rar", ".7z"})
        if (path.endsWith(QLatin1String(ext)))
            return QStringLiteral("comic");
    return {};
}

QString Entry::navigation() const {
    for (const auto &l : links)
        if (l.rel == QLatin1String("subsection") || l.rel == QLatin1String("http://opds-spec.org/subsection") ||
            l.rel.startsWith(QLatin1String("http://opds-spec.org/sort/")) ||
            l.rel.startsWith(QLatin1String("http://opds-spec.org/featured")) ||
            ((l.rel.isEmpty() || l.rel == QLatin1String("alternate") || l.rel == QLatin1String("start")) && isNavigationType(l.type)))
            return l.href;
    return {};
}

const Link *Entry::stream() const {
    for (const auto &l : links)
        if (l.rel == kPseStream && l.pseCount > 0)
            return &l;
    return nullptr;
}

const Link *Entry::acquisition(const QString &kind) const {
    const Link *best = nullptr;
    for (const auto &l : links) {
        if (!l.rel.startsWith(kAcquisition) || l.rel.contains(QLatin1String("buy")) || l.rel.contains(QLatin1String("subscribe")))
            continue;
        if (kindOf(l.type, l.href) != kind)
            continue;
        // A plain acquisition (or open-access) over "borrow" and "sample".
        if (!best || l.rel == kAcquisition || l.rel.endsWith(QLatin1String("open-access")))
            best = &l;
    }
    return best;
}

bool Entry::isBook() const {
    for (const auto &l : links)
        if (l.rel.startsWith(kAcquisition) || l.rel == kPseStream)
            return true;
    return false;
}

Feed parse(const QByteArray &body, const QUrl &base) {
    for (const char c : body) {
        if (c == '{')
            return parseJson(body, base);
        if (c == '<')
            return parseAtom(body, base);
        if (!QChar::isSpace(uchar(c)))
            break;
    }
    return {};
}

QString openSearchTemplate(const QByteArray &body, const QUrl &base) {
    QXmlStreamReader xml(body);
    QString fallback;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("Url")) {
            const auto type = xml.attributes().value(QStringLiteral("type")).toString();
            const auto templ = resolve(base, xml.attributes().value(QStringLiteral("template")).toString());
            if (type.contains(QLatin1String("atom")) || type.contains(QLatin1String("opds")))
                return templ;
            if (fallback.isEmpty())
                fallback = templ;
        }
    }
    return fallback;
}

QString searchUrl(const QString &templ, const QString &query) {
    const auto q = QString::fromUtf8(QUrl::toPercentEncoding(query));
    auto out = templ;
    out.replace(QLatin1String("{searchTerms}"), q).replace(QLatin1String("{query}"), q);
    // RFC 6570's form query: {?query} or {?query,…}.
    static const QRegularExpression form(QStringLiteral(R"(\{\?([^}]*)\})"));
    const auto m = form.match(out);
    if (m.hasMatch()) {
        const auto names = m.captured(1).split(QLatin1Char(','));
        QString part;
        for (const auto &n : names)
            if (n.trimmed() == QLatin1String("query") || n.trimmed() == QLatin1String("searchTerms"))
                part = (out.contains(QLatin1Char('?')) && out.indexOf(QLatin1Char('?')) < m.capturedStart() ? QStringLiteral("&") : QStringLiteral("?")) +
                       n.trimmed() + QLatin1Char('=') + q;
        out.replace(m.capturedStart(), m.capturedLength(), part);
    }
    // Optional parameters left empty.
    static const QRegularExpression rest(QStringLiteral(R"(\{[^}]*\})"));
    out.remove(rest);
    return out;
}

namespace {

// ------------------------------------------------------------------ client

// The catalogue of one server: feeds (cached a few minutes), its search,
// its sections, and its files fetched into the cache.
class Client {
public:
    explicit Client(Server server) : m_server(std::move(server)) {}
    const Server &server() const { return m_server; }

    // The login, and SCANCORE by name as OPDS readers go (a browser's
    // name from a non-browser is refused by some catalogues).
    Headers headers() const {
        auto h = authHeaders(m_server);
        h.append(QPair<QByteArray, QByteArray>("User-Agent", "SCANCORE/" + QCoreApplication::applicationVersion().toLatin1() + " (OPDS)"));
        return h;
    }

    Feed feed(const QString &url, bool fresh = false) {
        const auto key = QStringLiteral("opds:%1:%2").arg(m_server.id, url);
        if (!fresh)
            if (const auto cached = Database::cacheGet(key))
                return parse(*cached, QUrl(url));
        auto headers = this->headers();
        headers.append(QPair<QByteArray, QByteArray>("Accept", "application/atom+xml, application/opds+json;q=0.9, application/json;q=0.8, */*;q=0.5"));
        const auto response = Http::get(QUrl(url), headers, 30000);
        if (response.status == 401 || response.status == 403)
            throw Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(m_server.label), response.status);
        if (!response.ok())
            throw Error(QObject::tr("%1 answered with an error (%2).").arg(m_server.label).arg(response.status), response.status);
        auto parsed = parse(response.body, response.finalUrl.isValid() ? response.finalUrl : QUrl(url));
        if (parsed.entries.isEmpty() && parsed.title.isEmpty() && parsed.next.isEmpty())
            throw Error(QObject::tr("%1 is not an OPDS catalogue (its address may be the server's page, not its OPDS feed).").arg(m_server.label));
        Database::cachePut(key, response.body, 300);
        return parsed;
    }

    // Every entry of a feed, with its next pages (up to `pages`).
    QList<Entry> all(const QString &url, int pages = 20) {
        QList<Entry> out;
        QString at = url;
        for (int i = 0; i < pages && !at.isEmpty(); ++i) {
            const auto f = feed(at);
            out += f.entries;
            if (f.next == at)
                break;
            at = f.next;
        }
        return out;
    }

    QString searchTemplate() {
        const auto root = feed(m_server.url);
        if (!root.searchTemplate.isEmpty())
            return root.searchTemplate;
        if (root.searchDescription.isEmpty())
            return {};
        const auto key = QStringLiteral("opds-search:%1").arg(m_server.id);
        if (const auto cached = Database::cacheGet(key))
            return QString::fromUtf8(*cached);
        const auto response = Http::get(QUrl(root.searchDescription), headers(), 20000);
        const auto templ = response.ok() ? openSearchTemplate(response.body, QUrl(root.searchDescription)) : QString();
        Database::cachePut(key, templ.toUtf8(), 3600);
        return templ;
    }

    // Where a source's own lists start, when no section is picked: the
    // catalogue's popular (or newest) feed, else its start. "latest" is
    // its newest. The start itself when it already lists books.
    QString startOf(const QString &listing) {
        const auto root = feed(m_server.url);
        for (const auto &e : root.entries)
            if (e.isBook())
                return m_server.url;
        const auto byRel = [&](const QString &rel, const QStringList &words) -> QString {
            for (const auto &e : root.entries)
                for (const auto &l : e.links)
                    if (l.rel.startsWith(rel))
                        return l.href;
            for (const auto &e : root.entries)
                for (const auto &w : words)
                    if (e.title.contains(w, Qt::CaseInsensitive) && !e.navigation().isEmpty())
                        return e.navigation();
            return {};
        };
        const auto newest = byRel(QStringLiteral("http://opds-spec.org/sort/new"), {QStringLiteral("latest"), QStringLiteral("recent"), QStringLiteral("new")});
        if (listing == QLatin1String("latest"))
            return newest.isEmpty() ? m_server.url : newest;
        const auto popular = byRel(QStringLiteral("http://opds-spec.org/sort/popular"), {QStringLiteral("popular"), QStringLiteral("hot")});
        return !popular.isEmpty() ? popular : !newest.isEmpty() ? newest : m_server.url;
    }

    // The catalogue's folders two levels down, for the Section filter:
    // [{title, url}], "Libraries › Manga" for the second level.
    QList<QPair<QString, QString>> sections() {
        QList<QPair<QString, QString>> out;
        const auto root = feed(m_server.url);
        int asked = 0;
        for (const auto &e : root.entries) {
            const auto url = e.navigation();
            if (url.isEmpty() || e.isBook())
                continue;
            out.append({e.title, url});
            // A folder of folders (libraries, authors…): its folders too,
            // a few requests at most.
            if (asked >= 6)
                continue;
            ++asked;
            try {
                const auto sub = feed(url);
                int added = 0;
                bool folders = true;
                for (const auto &s : sub.entries)
                    if (s.isBook()) folders = false;
                if (!folders)
                    continue;
                for (const auto &s : sub.entries) {
                    const auto subUrl = s.navigation();
                    if (subUrl.isEmpty() || added >= 30)
                        continue;
                    out.append({e.title + QStringLiteral(" › ") + s.title, subUrl});
                    ++added;
                }
            } catch (const Error &) {
            }
        }
        return out;
    }

    // A book's file, fetched once into the cache a piece at a time (big
    // files never sit whole in memory); its local path.
    QString file(const Link &link) {
        const auto ext = [&] {
            const auto t = link.type.toLower();
            if (t.contains(QLatin1String("epub"))) return QStringLiteral("epub");
            if (t.contains(QLatin1String("pdf"))) return QStringLiteral("pdf");
            if (t.contains(QLatin1String("rar"))) return QStringLiteral("cbr");
            if (t.contains(QLatin1String("7z"))) return QStringLiteral("cb7");
            const auto suffix = QFileInfo(QUrl(link.href).path()).suffix().toLower();
            return suffix.isEmpty() ? QStringLiteral("cbz") : suffix;
        }();
        const auto dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/opds/") + m_server.id;
        QDir().mkpath(dir);
        const auto path = dir + QLatin1Char('/') +
                          QString::fromLatin1(QCryptographicHash::hash(link.href.toUtf8(), QCryptographicHash::Sha1).toHex().left(20)) +
                          QLatin1Char('.') + ext;
        if (QFileInfo(path).size() > 0)
            return path;
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly))
            throw Error(QObject::tr("The book could not be saved."));
        constexpr qint64 piece = 8 * 1024 * 1024;
        qint64 from = 0;
        for (;;) {
            Request request;
            request.url = QUrl(link.href);
            request.headers = headers();
            request.headers.append(QPair<QByteArray, QByteArray>("Range", QStringLiteral("bytes=%1-%2").arg(from).arg(from + piece - 1).toLatin1()));
            request.timeoutMs = 120000;
            request.maxBytes = piece + 1024;
            auto response = Http::send(request);
            if (response.status == 200) {
                // No ranges: the whole file in one answer (up to 1 GB).
                if (from > 0)
                    throw Error(QObject::tr("%1 stopped sending the book.").arg(m_server.label));
                request.headers.removeLast();
                request.maxBytes = 1024LL * 1024 * 1024;
                response = Http::send(request);
                if (!response.ok())
                    throw Error(QObject::tr("%1 answered with an error (%2).").arg(m_server.label).arg(response.status), response.status);
                out.write(response.body);
                break;
            }
            if (response.status == 416 && from > 0)
                break;
            if (response.status != 206)
                throw Error(QObject::tr("%1 answered with an error (%2).").arg(m_server.label).arg(response.status), response.status);
            out.write(response.body);
            from += response.body.size();
            if (response.body.size() < piece)
                break;
            const auto total = response.header("Content-Range").split('/').value(1).toLongLong();
            if (total > 0 && from >= total)
                break;
        }
        if (!out.commit())
            throw Error(QObject::tr("The book could not be saved."));
        return path;
    }

private:
    Server m_server;
};

// A book's own address (no feed of its own): "opds-book:" + the entry as
// JSON, so it can be opened from a list without asking again.
QString bookUrl(const Entry &e) {
    QJsonArray links;
    for (const auto &l : e.links)
        links.append(QJsonObject{{"rel", l.rel}, {"href", l.href}, {"type", l.type}, {"count", l.pseCount}});
    const QJsonObject o{{"id", e.id}, {"title", e.title}, {"author", e.author}, {"cover", e.cover}, {"summary", e.summary}, {"links", links}};
    return QStringLiteral("opds-book:") + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding));
}
std::optional<Entry> bookFrom(const QString &url) {
    if (!url.startsWith(QLatin1String("opds-book:")))
        return std::nullopt;
    const auto o = QJsonDocument::fromJson(QByteArray::fromBase64(url.mid(10).toLatin1(), QByteArray::Base64UrlEncoding)).object();
    Entry e;
    e.id = o.value(QStringLiteral("id")).toString();
    e.title = o.value(QStringLiteral("title")).toString();
    e.author = o.value(QStringLiteral("author")).toString();
    e.cover = o.value(QStringLiteral("cover")).toString();
    e.summary = o.value(QStringLiteral("summary")).toString();
    for (const auto &v : o.value(QStringLiteral("links")).toArray()) {
        const auto l = v.toObject();
        e.links.append({l.value(QStringLiteral("rel")).toString(), l.value(QStringLiteral("href")).toString(),
                        l.value(QStringLiteral("type")).toString(), {}, l.value(QStringLiteral("count")).toInt()});
    }
    return e;
}

// Whether an entry has something for this kind of reader.
bool hasFor(const Entry &e, const QString &kind) {
    return kind == QLatin1String("comic") ? (e.stream() || e.acquisition(kind)) : e.acquisition(kind) != nullptr;
}

// A chapter's address: the stream's template (with how many pages after
// "#"), or the file's address.
QString chapterUrl(const Entry &e, const QString &kind) {
    if (kind == QLatin1String("comic"))
        if (const auto *s = e.stream())
            return s->href + QLatin1Char('#') + kPseMark + QString::number(s->pseCount);
    const auto *a = e.acquisition(kind);
    return a ? a->href + QStringLiteral("#type=") + QString::fromLatin1(QUrl::toPercentEncoding(a->type)) : QString();
}
Link linkFrom(const QString &url) {
    const int hash = url.lastIndexOf(QLatin1Char('#'));
    Link link;
    link.href = hash >= 0 ? url.left(hash) : url;
    const auto fragment = hash >= 0 ? url.mid(hash + 1) : QString();
    if (fragment.startsWith(QLatin1String("type=")))
        link.type = QUrl::fromPercentEncoding(fragment.mid(5).toLatin1());
    return link;
}

// What a list shows of an entry: a book of this kind, or a folder.
std::optional<comics::Summary> comicSummary(const Entry &e, const QString &key) {
    comics::Summary s;
    s.title = e.title;
    s.cover = e.cover;
    s.author = e.author;
    s.source = key;
    if (e.isBook()) {
        if (!hasFor(e, QStringLiteral("comic")))
            return std::nullopt;
        s.url = bookUrl(e);
        s.units = 1;
    } else {
        s.url = e.navigation();
        if (s.url.isEmpty())
            return std::nullopt;
        s.type = QStringLiteral("Folder");
    }
    return s;
}
std::optional<novels::Book> novelSummary(const Entry &e, const QString &key) {
    novels::Book b;
    b.title = e.title;
    b.cover = e.cover;
    b.author = e.author;
    b.description = e.summary;
    b.source = key;
    b.genres = e.genres;
    if (e.isBook()) {
        if (!hasFor(e, QStringLiteral("novel")))
            return std::nullopt;
        b.url = bookUrl(e);
        b.chapterCount = 1;
    } else {
        b.url = e.navigation();
        if (b.url.isEmpty())
            return std::nullopt;
    }
    return b;
}

QJsonArray sectionFilter(Client &client) {
    QJsonArray values;
    values.append(QJsonObject{{"value", ""}, {"label", QObject::tr("The catalogue's start")}});
    try {
        for (const auto &[title, url] : client.sections())
            values.append(QJsonObject{{"value", url}, {"label", title}});
    } catch (const Error &) {
    }
    return QJsonArray{QJsonObject{{"type", "select"}, {"key", "section"}, {"name", QObject::tr("Section")}, {"values", values}, {"state", 0}}};
}
QString sectionFrom(const QJsonArray &filters) {
    for (const auto &v : filters) {
        const auto f = v.toObject();
        if (f.value(QStringLiteral("key")).toString() != QLatin1String("section"))
            continue;
        const auto values = f.value(QStringLiteral("values")).toArray();
        const int state = f.value(QStringLiteral("state")).toInt();
        return values.at(state).toObject().value(QStringLiteral("value")).toString();
    }
    return {};
}

// The page `page` (1-based) of a feed, walking its next links.
Feed feedPage(Client &client, const QString &url, int page) {
    Feed f = client.feed(url);
    int at = 1;
    for (; at < page && !f.next.isEmpty(); ++at)
        f = client.feed(f.next);
    // Asked past the end: nothing more.
    if (at < page)
        return {};
    return f;
}

// ------------------------------------------------------------------ comics

class ComicSource : public comics::Source {
public:
    ComicSource(comics::SourceConfig config, Server server)
        : comics::Source(std::move(config)), m_client(std::move(server)) {}
    QString imageReferer() const override { return QStringLiteral("server:") + m_client.server().id; }
    bool owns(const QUrl &url) const override {
        return url.scheme() == QLatin1String("opds-book") || comics::Source::owns(url);
    }

    QList<comics::Summary> search(const QString &query) override {
        if (query.trimmed().isEmpty())
            return browse(QStringLiteral("popular"), 1).items;
        const auto templ = m_client.searchTemplate();
        if (templ.isEmpty())
            return {};
        QList<comics::Summary> out;
        for (const auto &e : m_client.feed(searchUrl(templ, query), true).entries)
            if (auto s = comicSummary(e, key()))
                out.append(*s);
        return out;
    }

    bool canBrowse() const override { return true; }
    bool hasLatestListing() const override { return true; }
    QJsonArray filters() override { return sectionFilter(m_client); }
    comics::Listing browse(const QString &listing, int page, const QString &query = {}, const QJsonArray &filters = {}) override {
        comics::Listing out;
        if (listing == QLatin1String("search") && !query.trimmed().isEmpty() && sectionFrom(filters).isEmpty()) {
            out.items = search(query);
            return out;
        }
        const auto section = sectionFrom(filters);
        const auto f = feedPage(m_client, section.isEmpty() ? m_client.startOf(listing) : section, std::max(1, page));
        for (const auto &e : f.entries)
            if (auto s = comicSummary(e, key()))
                out.items.append(*s);
        out.hasNextPage = !f.next.isEmpty();
        return out;
    }

    std::optional<comics::Details> details(const QString &url) override {
        comics::Details d;
        if (const auto book = bookFrom(url)) {
            d.title = book->title;
            d.cover = book->cover;
            d.description = book->summary;
            d.author = book->author;
            return d;
        }
        const auto f = m_client.feed(url);
        d.title = f.title;
        for (const auto &e : f.entries) {
            if (d.cover.isEmpty() && !e.cover.isEmpty())
                d.cover = e.cover;
            if (d.author.isEmpty())
                d.author = e.author;
            if (d.description.isEmpty() && e.isBook())
                d.description = e.summary;
        }
        return d;
    }

    QList<comics::Chapter> chapters(const QString &url) override {
        QList<comics::Chapter> out;
        QList<Entry> books;
        if (const auto book = bookFrom(url))
            books = {*book};
        else
            books = m_client.all(url);
        int n = 0;
        for (const auto &e : books) {
            if (!e.isBook() || !hasFor(e, QStringLiteral("comic")))
                continue;
            comics::Chapter ch;
            ch.title = e.title;
            ch.number = comics::chapterNumberFrom(e.title);
            if (ch.number == QLatin1String("0"))
                ch.number = QString::number(n + 1);
            ch.url = chapterUrl(e, QStringLiteral("comic"));
            ch.thumbnail = e.cover;
            ch.released = e.updated.left(10);
            ch.slug = e.id;
            out.append(ch);
            ++n;
        }
        if (out.isEmpty() && !books.isEmpty()) {
            if (std::none_of(books.begin(), books.end(), [](const Entry &e) { return e.isBook(); }))
                throw Error(QObject::tr("This is a folder of series: pick it under Section in this source's filters to see them."));
            throw Error(QObject::tr("This folder has no comics SCANCORE can read (CBZ, CBR, CB7, PDF or streamed pages). Books in EPUB are under Novels."));
        }
        return out;
    }

    QList<comics::Panel> panels(const QString &url) override {
        // Split by hand: QUrl would escape the template's braces.
        const int hash = url.lastIndexOf(QLatin1Char('#'));
        const auto fragment = hash >= 0 ? url.mid(hash + 1) : QString();
        if (fragment.startsWith(kPseMark)) {
            // OPDS-PSE: page n (from 0) at the template's {pageNumber}.
            const int count = fragment.mid(kPseMark.size()).toInt();
            const auto templ = url.left(hash);
            QList<comics::Panel> out;
            for (int i = 0; i < count; ++i) {
                auto page = templ;
                page.replace(QLatin1String("{pageNumber}"), QString::number(i)).replace(QLatin1String("{maxWidth}"), QStringLiteral("2000"));
                out.append({page, 0, 0});
            }
            return out;
        }
        // A file: fetched, then read as a file on this device.
        const auto path = m_client.file(linkFrom(url));
        return local()->panels(QUrl::fromLocalFile(path).toString());
    }

private:
    comics::Source *local() {
        if (!m_local) {
            comics::SourceConfig config;
            config.key = QStringLiteral("local");
            config.engine = QStringLiteral("local");
            config.base = QStringLiteral("file://");
            m_local = local::createComicSource(config);
        }
        return m_local.get();
    }
    Client m_client;
    std::unique_ptr<comics::Source> m_local;
};

// ------------------------------------------------------------------ novels

class NovelSource : public novels::Source {
public:
    NovelSource(novels::SourceConfig config, Server server)
        : novels::Source(std::move(config)), m_client(std::move(server)) {}
    QString imageReferer() const override { return QStringLiteral("server:") + m_client.server().id; }
    bool owns(const QUrl &url) const override {
        return url.scheme() == QLatin1String("opds-book") || novels::Source::owns(url);
    }

    QList<novels::Book> search(const QString &query) override {
        if (query.trimmed().isEmpty())
            return browse(QStringLiteral("popular"), 1, {}, {}).items;
        const auto templ = m_client.searchTemplate();
        if (templ.isEmpty())
            return {};
        QList<novels::Book> out;
        for (const auto &e : m_client.feed(searchUrl(templ, query), true).entries)
            if (auto b = novelSummary(e, key()))
                out.append(*b);
        return out;
    }

    bool canBrowse() const override { return true; }
    bool hasLatestListing() const override { return true; }
    QJsonArray filters() override { return sectionFilter(m_client); }
    novels::Listing browse(const QString &listing, int page, const QString &query, const QJsonArray &filters) override {
        novels::Listing out;
        if (listing == QLatin1String("search") && !query.trimmed().isEmpty() && sectionFrom(filters).isEmpty()) {
            out.items = search(query);
            return out;
        }
        const auto section = sectionFrom(filters);
        const auto f = feedPage(m_client, section.isEmpty() ? m_client.startOf(listing) : section, std::max(1, page));
        for (const auto &e : f.entries)
            if (auto b = novelSummary(e, key()))
                out.items.append(*b);
        out.hasNextPage = !f.next.isEmpty();
        return out;
    }

    // A folder of books: each book (a volume) is a chapter. A book on its
    // own: its own chapters, from the file.
    novels::Book book(const QString &url, QList<novels::ChapterRef> &chapters) override {
        novels::Book b;
        b.source = key();
        b.url = url;
        if (const auto one = bookFrom(url)) {
            b.title = one->title;
            b.author = one->author;
            b.cover = one->cover;
            b.description = one->summary;
            const auto *a = one->acquisition(QStringLiteral("novel"));
            if (!a)
                throw Error(QObject::tr("This book is not in EPUB."));
            const auto path = m_client.file(*a);
            QList<novels::ChapterRef> inner;
            const auto fileBook = local()->book(QUrl::fromLocalFile(path).toString(), inner);
            if (b.title.isEmpty())
                b.title = fileBook.title;
            chapters = inner;
            b.chapterCount = int(chapters.size());
            return b;
        }
        const auto f = m_client.feed(url);
        b.title = f.title;
        int n = 0;
        for (const auto &e : m_client.all(url)) {
            if (!e.isBook() || !hasFor(e, QStringLiteral("novel")))
                continue;
            if (b.cover.isEmpty())
                b.cover = e.cover;
            if (b.author.isEmpty())
                b.author = e.author;
            if (b.description.isEmpty())
                b.description = e.summary;
            chapters.append({chapterUrl(e, QStringLiteral("novel")), e.title, QString::number(++n)});
        }
        if (chapters.isEmpty())
            throw Error(QObject::tr("This folder has no books in EPUB here: it may be a folder of series (pick it under Section in this source's filters), or hold comics (under Comics)."));
        b.chapterCount = n;
        return b;
    }

    // A chapter of a book read from its file, or a whole volume (a book of
    // a folder) as one chapter.
    QList<novels::Block> chapter(const QString &url, QString &title) override {
        if (url.startsWith(QLatin1String("file:")))
            return local()->chapter(url, title);
        const auto path = m_client.file(linkFrom(url));
        QList<novels::ChapterRef> inner;
        local()->book(QUrl::fromLocalFile(path).toString(), inner);
        QList<novels::Block> out;
        for (const auto &ref : inner) {
            QString part;
            const auto blocks = local()->chapter(ref.url, part);
            if (!part.isEmpty() && inner.size() > 1)
                out.append({QStringLiteral("heading"), part, part.toHtmlEscaped()});
            out += blocks;
        }
        return out;
    }

private:
    novels::Source *local() {
        if (!m_local) {
            novels::SourceConfig config;
            config.key = QStringLiteral("local");
            config.engine = QStringLiteral("local");
            config.base = QStringLiteral("file://");
            m_local = local::createNovelSource(config);
        }
        return m_local.get();
    }
    Client m_client;
    std::unique_ptr<novels::Source> m_local;
};

} // namespace

std::unique_ptr<comics::Source> createComicSource(const comics::SourceConfig &config, const Server &server) {
    return std::make_unique<ComicSource>(config, server);
}

std::unique_ptr<novels::Source> createNovelSource(const novels::SourceConfig &config, const Server &server) {
    return std::make_unique<NovelSource>(config, server);
}

} // namespace sc::servers::opds
