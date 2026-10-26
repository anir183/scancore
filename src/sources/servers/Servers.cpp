#include "Servers.h"

#include "Opds.h"

#include "core/Database.h"
#include "core/ImageCache.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QUrlQuery>

namespace sc::servers {

using namespace comics;

namespace {

QString trimmedUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}

QJsonObject get(const Server &server, const QString &path, const QList<QPair<QString, QString>> &query = {}) {
    QUrl url(server.url + path);
    if (!query.isEmpty()) {
        QUrlQuery q(url);
        for (const auto &[k, v] : query)
            q.addQueryItem(k, v);
        url.setQuery(q);
    }
    auto headers = authHeaders(server);
    headers.append(QPair<QByteArray, QByteArray>("Accept", "application/json"));
    auto response = Http::get(url, headers, 30000);
    if (response.status == 401 && server.kind == QLatin1String("kavita")) {
        // The token ran out: log in again once.
        headers = authHeaders(server, true);
        headers.append(QPair<QByteArray, QByteArray>("Accept", "application/json"));
        response = Http::get(url, headers, 30000);
    }
    if (response.status == 401 || response.status == 403)
        throw Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(server.label), response.status);
    if (!response.ok())
        throw Error(QObject::tr("%1 answered with an error (%2).").arg(server.label).arg(response.status), response.status);
    const auto json = response.json();
    // Some endpoints answer with a list: handed back under "list".
    return json.isArray() ? QJsonObject{{"list", json.array()}} : json.object();
}

QJsonObject graphql(const Server &server, const QString &query, const QJsonObject &variables = {}) {
    auto headers = authHeaders(server);
    const auto response = Http::postJson(QUrl(server.url + QStringLiteral("/api/graphql")),
                                         QJsonDocument(QJsonObject{{"query", query}, {"variables", variables}}).toJson(QJsonDocument::Compact), headers);
    if (response.status == 401 || response.status == 403)
        throw Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(server.label), response.status);
    const auto json = response.json().object();
    const auto errors = json.value(QStringLiteral("errors")).toArray();
    if (!errors.isEmpty())
        throw Error(server.label + QStringLiteral(": ") + errors.first().toObject().value(QStringLiteral("message")).toString());
    if (!response.ok())
        throw Error(QObject::tr("%1 answered with an error (%2).").arg(server.label).arg(response.status), response.status);
    return json.value(QStringLiteral("data")).toObject();
}

QString number(const QJsonValue &v) {
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == double(qint64(d)) ? QString::number(qint64(d)) : QString::number(d);
    }
    return v.toString();
}

// ------------------------------------------------------------------ Komga

class KomgaSource : public Source {
public:
    KomgaSource(SourceConfig config, Server server) : Source(std::move(config)), m_server(std::move(server)) {}
    QString imageReferer() const override { return QStringLiteral("server:") + m_server.id; }

    QList<Summary> search(const QString &query) override {
        const auto json = get(m_server, QStringLiteral("/api/v1/series"), {{"search", query}, {"size", "50"}});
        QList<Summary> out;
        for (const auto &v : json.value(QStringLiteral("content")).toArray()) {
            const auto s = v.toObject();
            Summary item;
            item.title = s.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("title")).toString(s.value(QStringLiteral("name")).toString());
            item.url = m_server.url + QStringLiteral("/series/") + s.value(QStringLiteral("id")).toString();
            item.cover = m_server.url + QStringLiteral("/api/v1/series/%1/thumbnail").arg(s.value(QStringLiteral("id")).toString());
            item.units = s.value(QStringLiteral("booksCount")).toInt();
            item.source = key();
            out.append(item);
        }
        return out;
    }

    std::optional<Details> details(const QString &seriesUrl) override {
        const auto id = seriesUrl.section(QLatin1Char('/'), -1);
        const auto s = get(m_server, QStringLiteral("/api/v1/series/") + id);
        const auto meta = s.value(QStringLiteral("metadata")).toObject();
        const auto books = s.value(QStringLiteral("booksMetadata")).toObject();
        Details d;
        d.title = meta.value(QStringLiteral("title")).toString(s.value(QStringLiteral("name")).toString());
        d.cover = m_server.url + QStringLiteral("/api/v1/series/%1/thumbnail").arg(id);
        d.description = meta.value(QStringLiteral("summary")).toString();
        if (d.description.isEmpty())
            d.description = books.value(QStringLiteral("summary")).toString();
        d.status = meta.value(QStringLiteral("status")).toString();
        for (const auto &g : meta.value(QStringLiteral("genres")).toArray())
            d.genres.append(g.toString());
        QStringList authors;
        for (const auto &a : books.value(QStringLiteral("authors")).toArray())
            if (const auto name = a.toObject().value(QStringLiteral("name")).toString(); !name.isEmpty() && !authors.contains(name))
                authors.append(name);
        d.author = authors.join(QStringLiteral(", "));
        return d;
    }

    QList<Chapter> chapters(const QString &seriesUrl) override {
        const auto id = seriesUrl.section(QLatin1Char('/'), -1);
        const auto json = get(m_server, QStringLiteral("/api/v1/series/%1/books").arg(id), {{"unpaged", "true"}, {"sort", "metadata.numberSort,asc"}});
        QList<Chapter> out;
        for (const auto &v : json.value(QStringLiteral("content")).toArray()) {
            const auto b = v.toObject();
            const auto meta = b.value(QStringLiteral("metadata")).toObject();
            Chapter ch;
            ch.number = number(meta.value(QStringLiteral("numberSort")));
            ch.title = meta.value(QStringLiteral("title")).toString(b.value(QStringLiteral("name")).toString());
            ch.url = m_server.url + QStringLiteral("/book/") + b.value(QStringLiteral("id")).toString();
            ch.slug = b.value(QStringLiteral("id")).toString();
            ch.released = meta.value(QStringLiteral("releaseDate")).toString();
            out.append(ch);
        }
        return out;
    }

    QList<Panel> panels(const QString &chapterUrl) override {
        const auto id = chapterUrl.section(QLatin1Char('/'), -1);
        const auto json = get(m_server, QStringLiteral("/api/v1/books/%1/pages").arg(id));
        QList<Panel> out;
        for (const auto &v : json.value(QStringLiteral("list")).toArray()) {
            const auto p = v.toObject();
            out.append({m_server.url + QStringLiteral("/api/v1/books/%1/pages/%2").arg(id).arg(p.value(QStringLiteral("number")).toInt()),
                        p.value(QStringLiteral("width")).toInt(), p.value(QStringLiteral("height")).toInt()});
        }
        return out;
    }

private:
    Server m_server;
};

// ------------------------------------------------------------------ Kavita

class KavitaSource : public Source {
public:
    KavitaSource(SourceConfig config, Server server) : Source(std::move(config)), m_server(std::move(server)) {}
    QString imageReferer() const override { return QStringLiteral("server:") + m_server.id; }

    QList<Summary> search(const QString &query) override {
        const auto json = get(m_server, QStringLiteral("/api/Search/search"), {{"queryString", query}});
        QList<Summary> out;
        for (const auto &v : json.value(QStringLiteral("series")).toArray()) {
            const auto s = v.toObject();
            // Comics only: pictures and archives, not EPUB or PDF books.
            const int format = s.value(QStringLiteral("format")).toInt();
            if (format == 3 || format == 4)
                continue;
            const auto id = number(s.value(QStringLiteral("seriesId")));
            Summary item;
            item.title = s.value(QStringLiteral("name")).toString();
            item.url = m_server.url + QStringLiteral("/library/%1/series/%2").arg(number(s.value(QStringLiteral("libraryId"))), id);
            item.cover = m_server.url + QStringLiteral("/api/Image/series-cover?seriesId=") + id;
            item.source = key();
            out.append(item);
        }
        return out;
    }

    std::optional<Details> details(const QString &seriesUrl) override {
        const auto id = seriesUrl.section(QLatin1Char('/'), -1);
        const auto s = get(m_server, QStringLiteral("/api/Series/") + id);
        const auto meta = get(m_server, QStringLiteral("/api/Series/metadata"), {{"seriesId", id}});
        Details d;
        d.title = s.value(QStringLiteral("name")).toString();
        d.cover = m_server.url + QStringLiteral("/api/Image/series-cover?seriesId=") + id;
        d.description = meta.value(QStringLiteral("summary")).toString();
        for (const auto &g : meta.value(QStringLiteral("genres")).toArray())
            d.genres.append(g.toObject().value(QStringLiteral("title")).toString());
        QStringList writers;
        for (const auto &w : meta.value(QStringLiteral("writers")).toArray())
            writers.append(w.toObject().value(QStringLiteral("name")).toString());
        d.author = writers.join(QStringLiteral(", "));
        return d;
    }

    QList<Chapter> chapters(const QString &seriesUrl) override {
        const auto id = seriesUrl.section(QLatin1Char('/'), -1);
        const auto json = get(m_server, QStringLiteral("/api/Series/volumes"), {{"seriesId", id}});
        QList<Chapter> out;
        for (const auto &v : json.value(QStringLiteral("list")).toArray()) {
            const auto volume = v.toObject();
            const auto volumeName = volume.value(QStringLiteral("name")).toString();
            for (const auto &c : volume.value(QStringLiteral("chapters")).toArray()) {
                const auto ch = c.toObject();
                Chapter item;
                // Kavita writes a loose volume or special as -100000.
                const auto n = number(ch.value(QStringLiteral("number")));
                item.number = n == QLatin1String("-100000") ? QString::number(out.size() + 1) : n;
                item.title = ch.value(QStringLiteral("title")).toString();
                if (item.title.isEmpty())
                    item.title = ch.value(QStringLiteral("range")).toString();
                if (item.title.isEmpty() || item.title == n)
                    item.title = volumeName.isEmpty() || volumeName == QLatin1String("-100000")
                                     ? chapterLabel(item.number) : QStringLiteral("Vol. %1 · %2").arg(volumeName, chapterLabel(item.number));
                item.url = m_server.url + QStringLiteral("/chapter/%1?pages=%2").arg(number(ch.value(QStringLiteral("id")))).arg(ch.value(QStringLiteral("pages")).toInt());
                item.slug = number(ch.value(QStringLiteral("id")));
                item.released = ch.value(QStringLiteral("releaseDate")).toString();
                out.append(item);
            }
        }
        sortChapters(out);
        return out;
    }

    QList<Panel> panels(const QString &chapterUrl) override {
        const QUrl url(chapterUrl);
        const auto id = url.path().section(QLatin1Char('/'), -1);
        int pages = QUrlQuery(url).queryItemValue(QStringLiteral("pages")).toInt();
        if (pages <= 0)
            pages = get(m_server, QStringLiteral("/api/Reader/chapter-info"), {{"chapterId", id}}).value(QStringLiteral("pages")).toInt();
        QList<Panel> out;
        for (int i = 0; i < pages; ++i)
            out.append({m_server.url + QStringLiteral("/api/Reader/image?chapterId=%1&page=%2&extractPdf=true").arg(id).arg(i)});
        return out;
    }

private:
    Server m_server;
};

// ------------------------------------------------------------------ Suwayomi

class SuwayomiSource : public Source {
public:
    SuwayomiSource(SourceConfig config, Server server) : Source(std::move(config)), m_server(std::move(server)) {}
    QString imageReferer() const override { return QStringLiteral("server:") + m_server.id; }

    // The server's library (what was added there), by title.
    QList<Summary> search(const QString &query) override {
        const auto data = graphql(m_server, QStringLiteral(R"(query($q: String!) { mangas(condition: { inLibrary: true },
                filter: { title: { includesInsensitive: $q } }, first: 100) { nodes { id title thumbnailUrl } } })"),
                                  {{"q", query}});
        QList<Summary> out;
        for (const auto &v : data.value(QStringLiteral("mangas")).toObject().value(QStringLiteral("nodes")).toArray()) {
            const auto m = v.toObject();
            Summary item;
            item.title = m.value(QStringLiteral("title")).toString();
            item.url = m_server.url + QStringLiteral("/manga/") + number(m.value(QStringLiteral("id")));
            item.cover = resolve(m.value(QStringLiteral("thumbnailUrl")).toString());
            item.source = key();
            out.append(item);
        }
        return out;
    }

    std::optional<Details> details(const QString &seriesUrl) override {
        const auto data = graphql(m_server, QStringLiteral(R"(query($id: Int!) { manga(id: $id) { title description author artist genre status thumbnailUrl } })"),
                                  {{"id", seriesUrl.section(QLatin1Char('/'), -1).toInt()}});
        const auto m = data.value(QStringLiteral("manga")).toObject();
        Details d;
        d.title = m.value(QStringLiteral("title")).toString();
        d.description = m.value(QStringLiteral("description")).toString();
        d.author = m.value(QStringLiteral("author")).toString();
        d.status = m.value(QStringLiteral("status")).toString();
        d.cover = resolve(m.value(QStringLiteral("thumbnailUrl")).toString());
        for (const auto &g : m.value(QStringLiteral("genre")).toArray())
            d.genres.append(g.toString());
        return d;
    }

    QList<Chapter> chapters(const QString &seriesUrl) override {
        const auto data = graphql(m_server, QStringLiteral(R"(query($id: Int!) { chapters(condition: { mangaId: $id }, orderBy: SOURCE_ORDER) {
                nodes { id name chapterNumber uploadDate scanlator } } })"),
                                  {{"id", seriesUrl.section(QLatin1Char('/'), -1).toInt()}});
        QList<Chapter> out;
        for (const auto &v : data.value(QStringLiteral("chapters")).toObject().value(QStringLiteral("nodes")).toArray()) {
            const auto c = v.toObject();
            Chapter ch;
            ch.number = number(c.value(QStringLiteral("chapterNumber")));
            ch.title = c.value(QStringLiteral("name")).toString();
            ch.url = m_server.url + QStringLiteral("/chapter/") + number(c.value(QStringLiteral("id")));
            ch.slug = number(c.value(QStringLiteral("id")));
            ch.scanlator = c.value(QStringLiteral("scanlator")).toString();
            // A Long: a string in Suwayomi's GraphQL, a number in others.
            const auto when = c.value(QStringLiteral("uploadDate"));
            const auto upload = when.isDouble() ? qint64(when.toDouble()) : when.toString().toLongLong();
            if (upload > 0)
                ch.released = QDateTime::fromMSecsSinceEpoch(upload).toUTC().toString(Qt::ISODate);
            out.append(ch);
        }
        sortChapters(out);
        return out;
    }

    QList<Panel> panels(const QString &chapterUrl) override {
        const auto data = graphql(m_server, QStringLiteral(R"(mutation($id: Int!) { fetchChapterPages(input: { chapterId: $id }) { pages } })"),
                                  {{"id", chapterUrl.section(QLatin1Char('/'), -1).toInt()}});
        QList<Panel> out;
        for (const auto &p : data.value(QStringLiteral("fetchChapterPages")).toObject().value(QStringLiteral("pages")).toArray())
            out.append({resolve(p.toString())});
        return out;
    }

private:
    QString resolve(const QString &path) const { return path.startsWith(QLatin1String("http")) ? path : m_server.url + path; }
    Server m_server;
};

QMutex g_tokens;
QHash<QString, QPair<QByteArray, qint64>> g_kavitaTokens; // server id -> token, until (ms)
QHash<QString, QString> g_kavitaKeys;                     // server id -> the account's API key

} // namespace

QList<Server> load() {
    QList<Server> out;
    QString saved;
    try {
        saved = Database::pref(QStringLiteral("servers")).toString();
    } catch (const Error &) {
        // No settings yet (tools and tests without a profile): no servers.
        return out;
    }
    for (const auto &v : QJsonDocument::fromJson(saved.toUtf8()).array()) {
        const auto o = v.toObject();
        Server s;
        s.id = o.value(QStringLiteral("id")).toString();
        s.kind = o.value(QStringLiteral("kind")).toString();
        s.label = o.value(QStringLiteral("label")).toString();
        s.url = trimmedUrl(o.value(QStringLiteral("url")).toString());
        s.username = o.value(QStringLiteral("username")).toString();
        s.password = o.value(QStringLiteral("password")).toString();
        s.apiKey = o.value(QStringLiteral("apiKey")).toString();
        if (!s.id.isEmpty() && !s.url.isEmpty())
            out.append(s);
    }
    return out;
}

void save(const QList<Server> &servers) {
    QJsonArray list;
    for (const auto &s : servers)
        list.append(QJsonObject{{"id", s.id}, {"kind", s.kind}, {"label", s.label}, {"url", trimmedUrl(s.url)},
                                {"username", s.username}, {"password", s.password}, {"apiKey", s.apiKey}});
    Database::setPref(QStringLiteral("servers"), QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact)));
    QMutexLocker lock(&g_tokens);
    g_kavitaTokens.clear();
}

std::optional<Server> byId(const QString &id) {
    for (const auto &s : load())
        if (s.id == id)
            return s;
    return std::nullopt;
}

Headers authHeaders(const Server &server, bool fresh) {
    if (server.kind != QLatin1String("kavita")) {
        if (server.username.isEmpty())
            return {};
        return {{"Authorization", "Basic " + (server.username + QLatin1Char(':') + server.password).toUtf8().toBase64()}};
    }
    {
        QMutexLocker lock(&g_tokens);
        const auto it = g_kavitaTokens.constFind(server.id);
        if (!fresh && it != g_kavitaTokens.cend() && it->second > QDateTime::currentMSecsSinceEpoch())
            return {{"Authorization", "Bearer " + it->first}};
    }
    Response response;
    if (!server.apiKey.isEmpty()) {
        response = Http::postJson(withQuery(QUrl(server.url + QStringLiteral("/api/Plugin/authenticate")),
                                            {{"apiKey", server.apiKey}, {"pluginName", "SCANCORE"}}),
                                  "{}");
    } else {
        response = Http::postJson(QUrl(server.url + QStringLiteral("/api/Account/login")),
                                  QJsonDocument(QJsonObject{{"username", server.username}, {"password", server.password}}).toJson(QJsonDocument::Compact));
    }
    const auto answer = response.json().object();
    const auto token = answer.value(QStringLiteral("token")).toString().toUtf8();
    if (!response.ok() || token.isEmpty())
        throw Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(server.label), response.status);
    QMutexLocker lock(&g_tokens);
    // Kavita's tokens last days; asked again after a few hours.
    g_kavitaTokens.insert(server.id, {token, QDateTime::currentMSecsSinceEpoch() + 6LL * 3600 * 1000});
    // Its page images take only the API key (in the address).
    if (const auto key = answer.value(QStringLiteral("apiKey")).toString(); !key.isEmpty())
        g_kavitaKeys.insert(server.id, key);
    return {{"Authorization", "Bearer " + token}};
}

std::unique_ptr<comics::Source> createSource(const Server &server) {
    SourceConfig config;
    config.key = QStringLiteral("server-") + server.id;
    config.label = server.label.isEmpty() ? server.url : server.label;
    config.engine = server.kind;
    config.base = server.url;
    config.api = server.url;
    config.hosts = {QUrl(server.url).host().toLower()};
    // The reader's own library: whatever language it is in.
    config.language = QStringLiteral("*");
    config.options = QJsonObject{{"server", server.id}};
    if (server.kind == QLatin1String("komga"))
        return std::make_unique<KomgaSource>(config, server);
    if (server.kind == QLatin1String("kavita"))
        return std::make_unique<KavitaSource>(config, server);
    if (server.kind == QLatin1String("suwayomi"))
        return std::make_unique<SuwayomiSource>(config, server);
    if (server.kind == QLatin1String("opds"))
        return opds::createComicSource(config, server);
    return nullptr;
}

std::unique_ptr<novels::Source> createNovelSource(const Server &server) {
    if (server.kind != QLatin1String("opds"))
        return nullptr;
    novels::SourceConfig config;
    config.key = QStringLiteral("server-") + server.id;
    config.label = server.label.isEmpty() ? server.url : server.label;
    config.engine = server.kind;
    config.base = server.url;
    config.hosts = {QUrl(server.url).host().toLower()};
    config.language = QStringLiteral("*");
    config.options = QJsonObject{{"server", server.id}};
    return opds::createNovelSource(config, server);
}

void chapterRead(const QString &sourceKey, const QString &seriesUrl, const QString &chapterUrl, const QString &number) {
    if (!sourceKey.startsWith(QLatin1String("server-")))
        return;
    {
        const auto server = byId(sourceKey.mid(7));
        if (!server)
            return;
        try {
            Response response;
            if (server->kind == QLatin1String("komga")) {
                Request request;
                request.url = QUrl(server->url + QStringLiteral("/api/v1/books/%1/read-progress").arg(QUrl(chapterUrl).path().section(QLatin1Char('/'), -1)));
                request.method = "PATCH";
                request.headers = authHeaders(*server);
                request.headers.append(QPair<QByteArray, QByteArray>("Content-Type", "application/json"));
                request.body = R"({"completed":true})";
                response = Http::send(request);
            } else if (server->kind == QLatin1String("kavita")) {
                // Kavita's endpoint for Mihon: read up to this chapter.
                response = Http::postJson(withQuery(QUrl(server->url + QStringLiteral("/api/Tachiyomi/mark-chapter-until-as-read")),
                                                    {{"seriesId", seriesUrl.section(QLatin1Char('/'), -1)}, {"chapterNumber", number}}),
                                          "{}", authHeaders(*server));
            } else if (server->kind == QLatin1String("suwayomi")) {
                graphql(*server, QStringLiteral(R"(mutation($id: Int!) { updateChapter(input: { id: $id, patch: { isRead: true } }) { chapter { id } } })"),
                        {{"id", chapterUrl.section(QLatin1Char('/'), -1).toInt()}});
                return;
            }
            if (!response.ok())
                qWarning("server %s: marking read answered %d", qPrintable(server->label), response.status);
        } catch (const Error &e) {
            qWarning("server %s: marking read failed: %s", qPrintable(server->label), qPrintable(e.message()));
        }
    }
}

QVariantList trackerSearch(const Server &server, const QString &title) {
    QVariantList out;
    if (server.kind == QLatin1String("komga")) {
        Request request;
        request.url = withQuery(QUrl(server.url + QStringLiteral("/api/v1/series")), {{"search", title}, {"size", "20"}});
        request.headers = authHeaders(server);
        const auto response = Http::send(request);
        if (!response.ok())
            throw Error(QStringLiteral("%1 answered %2.").arg(server.label).arg(response.status), response.status);
        for (const auto &v : response.json().object().value(QStringLiteral("content")).toArray()) {
            const auto s = v.toObject();
            const auto name = s.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("title")).toString(s.value(QStringLiteral("name")).toString());
            const int books = s.value(QStringLiteral("booksCount")).toInt();
            out.append(QVariantMap{{"id", s.value(QStringLiteral("id")).toString()},
                                   {"title", name},
                                   {"titles", QStringList{name, s.value(QStringLiteral("name")).toString()}},
                                   {"detail", QStringLiteral("%1 books").arg(books)},
                                   {"total", books}});
        }
    } else if (server.kind == QLatin1String("kavita")) {
        Request request;
        request.url = withQuery(QUrl(server.url + QStringLiteral("/api/Search/search")), {{"queryString", title}});
        request.headers = authHeaders(server);
        const auto response = Http::send(request);
        if (!response.ok())
            throw Error(QStringLiteral("%1 answered %2.").arg(server.label).arg(response.status), response.status);
        for (const auto &v : response.json().object().value(QStringLiteral("series")).toArray()) {
            const auto s = v.toObject();
            const auto name = s.value(QStringLiteral("name")).toString();
            out.append(QVariantMap{{"id", s.value(QStringLiteral("seriesId")).toInteger()},
                                   {"title", name},
                                   {"titles", QStringList{name, s.value(QStringLiteral("originalName")).toString(), s.value(QStringLiteral("localizedName")).toString()}},
                                   {"detail", s.value(QStringLiteral("libraryName")).toString()},
                                   {"total", 0}});
        }
    }
    return out;
}

void trackerPush(const Server &server, const QVariant &id, int progress) {
    if (progress <= 0)
        return;
    Response response;
    if (server.kind == QLatin1String("komga")) {
        // Komga's endpoint for Mihon: books read up to this number. Komga
        // before 0.157 knows only the first version (by position).
        for (const auto &[version, field] : {std::pair{"v2", "lastBookNumberSortRead"}, std::pair{"v1", "lastBookRead"}}) {
            Request request;
            request.url = QUrl(server.url + QStringLiteral("/api/%1/series/%2/read-progress/tachiyomi").arg(QLatin1String(version), id.toString()));
            request.method = "PUT";
            request.headers = authHeaders(server);
            request.headers.append(QPair<QByteArray, QByteArray>("Content-Type", "application/json"));
            request.body = QJsonDocument(QJsonObject{{QLatin1String(field), progress}}).toJson(QJsonDocument::Compact);
            response = Http::send(request);
            if (response.status != 404)
                break;
        }
    } else if (server.kind == QLatin1String("kavita")) {
        response = Http::postJson(withQuery(QUrl(server.url + QStringLiteral("/api/Tachiyomi/mark-chapter-until-as-read")),
                                            {{"seriesId", id.toString()}, {"chapterNumber", QString::number(progress)}}),
                                  "{}", authHeaders(server));
    } else {
        return;
    }
    if (!response.ok())
        throw Error(QStringLiteral("%1 answered %2.").arg(server.label).arg(response.status), response.status == 401 ? 401 : response.status);
}

void registerImages() {
    static const bool once = [] {
        ImageCache::setRefererWriter(QStringLiteral("server"), [](const QString &url, const QString &referer, const QString &path) {
            const auto server = byId(referer.mid(int(qstrlen("server:"))));
            if (!server)
                throw Error(QObject::tr("This server is no longer in Settings → Sources → Your servers."), 404);
            QUrl target(url);
            auto headers = authHeaders(*server);
            if (server->kind == QLatin1String("kavita") && target.path().contains(QLatin1String("/api/Reader/"))) {
                QString key = server->apiKey;
                if (key.isEmpty()) {
                    QMutexLocker lock(&g_tokens);
                    key = g_kavitaKeys.value(server->id);
                }
                QUrlQuery query(target);
                query.addQueryItem(QStringLiteral("apiKey"), key);
                target.setQuery(query);
            }
            auto response = Http::get(target, headers, 60000);
            if (response.status == 401 && server->kind == QLatin1String("kavita"))
                response = Http::get(target, authHeaders(*server, true), 60000);
            if (!response.ok() || response.body.isEmpty())
                throw Error(QObject::tr("The server did not give the image (%1).").arg(response.status), response.status);
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size() || !file.commit())
                throw Error(QObject::tr("The image could not be saved."));
        });
        return true;
    }();
    Q_UNUSED(once)
}

} // namespace sc::servers
