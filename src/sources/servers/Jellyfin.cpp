// Jellyfin as one of the reader's own servers: its shows and films as an
// anime source (browse, search, seasons, episodes), played from the server
// as they are or transcoded to HLS, with its intro and credits segments to
// skip, and resume points kept both ways (the server's own when an episode
// opens, SCANCORE's sent back while it plays).
//
// The anime source answers the extension host's calls itself (see
// ext::AnimeSource): links are "series/<id>", "movie/<id>",
// "season/<series>/<season>" and "episode/<id>", wrapped with the source
// key "aniyomi:server-<id>".

#include "Servers.h"

#include "core/Database.h"
#include "extensions/AnimeSource.h"
#include "extensions/Host.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QSysInfo>
#include <QUrlQuery>
#include <QUuid>

#include <cmath>

namespace sc::servers {

namespace {

// ------------------------------------------------------------------ login

struct Session {
    QByteArray token;
    QString userId;
};

QMutex g_sessionsLock;
QHash<QString, Session> g_sessions; // server id

// One id for this install, so the server lists SCANCORE as one device and
// a new login replaces the old one.
QString deviceId() {
    static const QString id = [] {
        auto saved = Database::pref(QStringLiteral("jellyfin.deviceId")).toString();
        if (saved.isEmpty()) {
            saved = QUuid::createUuid().toString(QUuid::WithoutBraces);
            Database::setPref(QStringLiteral("jellyfin.deviceId"), saved);
        }
        return saved;
    }();
    return id;
}

QByteArray clientHeader(const QByteArray &token = {}) {
    auto version = QCoreApplication::applicationVersion();
    if (version.isEmpty())
        version = QStringLiteral("2");
    auto device = QSysInfo::prettyProductName();
    device.remove(QLatin1Char('"'));
    auto header = QStringLiteral(R"(MediaBrowser Client="SCANCORE", Device="%1", DeviceId="%2", Version="%3")")
                      .arg(device, deviceId(), version)
                      .toUtf8();
    if (!token.isEmpty())
        header += ", Token=\"" + token + '"';
    return header;
}

Session session(const Server &server, bool fresh) {
    {
        QMutexLocker lock(&g_sessionsLock);
        if (const auto it = g_sessions.constFind(server.id); !fresh && it != g_sessions.cend())
            return *it;
    }
    const auto response = Http::postJson(QUrl(server.url + QStringLiteral("/Users/AuthenticateByName")),
                                         QJsonDocument(QJsonObject{{"Username", server.username}, {"Pw", server.password}})
                                             .toJson(QJsonDocument::Compact),
                                         {{"Authorization", clientHeader()}, {"Accept", "application/json"}});
    const auto refused = [&] {
        return Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(server.label), 401);
    };
    if (response.status == 401 || response.status == 403)
        throw refused();
    if (!response.ok())
        throw Error(response.status == 0 ? QObject::tr("%1 could not be reached.").arg(server.label)
                                         : QObject::tr("%1 answered with an error (%2).").arg(server.label).arg(response.status),
                    response.status);
    const auto answer = response.json().object();
    Session s{answer.value(QStringLiteral("AccessToken")).toString().toUtf8(),
              answer.value(QStringLiteral("User")).toObject().value(QStringLiteral("Id")).toString()};
    if (s.token.isEmpty() || s.userId.isEmpty())
        throw refused();
    QMutexLocker lock(&g_sessionsLock);
    g_sessions.insert(server.id, s);
    return s;
}

QUrl address(const Server &server, const QString &path, const QList<QPair<QString, QString>> &query = {}) {
    QUrl url(server.url + path);
    if (!query.isEmpty()) {
        QUrlQuery q;
        for (const auto &[k, v] : query)
            q.addQueryItem(k, v);
        url.setQuery(q);
    }
    return url;
}

// A call to the server with its login, logging in again once when the
// token is refused (the server restarted, the user signed SCANCORE out).
Response send(const Server &server, const QByteArray &method, const QUrl &url, const QByteArray &body = {}) {
    for (bool fresh : {false, true}) {
        const auto s = session(server, fresh);
        Request request;
        request.url = url;
        request.method = method;
        request.body = body;
        request.timeoutMs = 30000;
        request.headers = {{"Authorization", clientHeader(s.token)}, {"Accept", "application/json"}};
        if (!body.isEmpty())
            request.headers.append(QPair<QByteArray, QByteArray>("Content-Type", "application/json"));
        auto response = Http::send(request);
        if (response.status != 401 || fresh)
            return response;
    }
    return {};
}

QJsonValue getJson(const Server &server, const QString &path, const QList<QPair<QString, QString>> &query = {}) {
    const auto response = send(server, "GET", address(server, path, query));
    if (response.status == 401 || response.status == 403)
        throw Error(QObject::tr("%1 refused the login. Check it in Settings → Sources → Your servers.").arg(server.label), response.status);
    if (response.status == 404)
        throw Error(QObject::tr("This is no longer on %1.").arg(server.label), 404);
    if (!response.ok())
        throw Error(response.status == 0 ? QObject::tr("%1 could not be reached.").arg(server.label)
                                         : QObject::tr("%1 answered with an error (%2).").arg(server.label).arg(response.status),
                    response.status);
    const auto json = response.json();
    return json.isArray() ? QJsonValue(json.array()) : QJsonValue(json.object());
}

QString userId(const Server &server) { return session(server, false).userId; }

constexpr double kTicks = 10000000.0; // 100 ns ticks in a second

// ------------------------------------------------------------------ source

class JellyfinSource : public ext::AnimeSource {
public:
    JellyfinSource(const ext::SourceEntry &entry, Server server) : AnimeSource(entry), m_server(std::move(server)) {}

    QString imageReferer() const override { return QStringLiteral("server:") + m_server.id; }
    bool isServer() const override { return true; }
    QString webUrl(const QString &url) const override {
        const auto link = unwrap(url);
        const auto parts = link.split(QLatin1Char('/'));
        return m_server.url + QStringLiteral("/web/#/details?id=") + parts.value(parts.size() - 1);
    }

protected:
    QJsonValue call(const QString &method, QJsonObject params) override {
        try {
            const auto result = answer(method, params);
            ext::Manager::recordSourceResult(key(), {});
            return result;
        } catch (const Error &e) {
            if (e.status() != 499)
                ext::Manager::recordSourceResult(key(), e.message());
            throw;
        }
    }

private:
    Server m_server;

    QJsonValue answer(const QString &method, const QJsonObject &params) {
        const auto url = params.value(QStringLiteral("url")).toString();
        if (method == QLatin1String("popular") || method == QLatin1String("latest") || method == QLatin1String("search"))
            return listing(method, params.value(QStringLiteral("page")).toInt(1), params.value(QStringLiteral("query")).toString());
        if (method == QLatin1String("details"))
            return details(url);
        if (method == QLatin1String("episodes"))
            return episodes(url);
        if (method == QLatin1String("seasons"))
            return seasons(url);
        if (method == QLatin1String("hosters"))
            return hosters(url);
        if (method == QLatin1String("filters") || method == QLatin1String("preferences"))
            return QJsonArray();
        if (method == QLatin1String("setPreference"))
            return QJsonValue();
        throw Error(QObject::tr("%1 cannot do that.").arg(m_server.label));
    }

    QString image(const QString &id, const QString &type = QStringLiteral("Primary"), const QString &size = QStringLiteral("fillHeight=600")) const {
        return m_server.url + QStringLiteral("/Items/%1/Images/%2?%3&quality=90").arg(id, type, size);
    }

    QJsonObject item(const QJsonObject &o) const {
        const auto id = o.value(QStringLiteral("Id")).toString();
        const bool movie = o.value(QStringLiteral("Type")).toString() == QLatin1String("Movie");
        QJsonObject out{{"url", (movie ? QStringLiteral("movie/") : QStringLiteral("series/")) + id},
                        {"title", o.value(QStringLiteral("Name")).toString()},
                        {"fetchType", !movie && o.value(QStringLiteral("ChildCount")).toInt() > 1 ? "seasons" : "episodes"}};
        if (o.value(QStringLiteral("ImageTags")).toObject().contains(QStringLiteral("Primary")))
            out.insert("cover", image(id));
        return out;
    }

    // Shows and films across the user's libraries, a page of 40 at a time:
    // the best rated first (Popular), the newest added (Latest), or by name
    // for a search.
    QJsonObject listing(const QString &method, int page, const QString &query) {
        constexpr int kPage = 40;
        QList<QPair<QString, QString>> q{{"IncludeItemTypes", "Series,Movie"},
                                         {"Recursive", "true"},
                                         {"Fields", "ChildCount"},
                                         {"EnableImageTypes", "Primary"},
                                         {"ImageTypeLimit", "1"},
                                         {"StartIndex", QString::number((std::max(1, page) - 1) * kPage)},
                                         {"Limit", QString::number(kPage)}};
        if (method == QLatin1String("latest"))
            q += {{"SortBy", "DateCreated,SortName"}, {"SortOrder", "Descending"}};
        else if (method == QLatin1String("popular"))
            q += {{"SortBy", "CommunityRating,SortName"}, {"SortOrder", "Descending"}};
        else
            q += {{"SortBy", "SortName"}, {"SearchTerm", query.trimmed()}};
        const auto json = getJson(m_server, QStringLiteral("/Users/%1/Items").arg(userId(m_server)), q).toObject();
        QJsonArray items;
        for (const auto &v : json.value(QStringLiteral("Items")).toArray())
            items.append(item(v.toObject()));
        const int total = json.value(QStringLiteral("TotalRecordCount")).toInt();
        return {{"items", items}, {"hasNextPage", (std::max(1, page) - 1) * kPage + items.size() < total}};
    }

    QJsonObject fetchItem(const QString &id) {
        return getJson(m_server, QStringLiteral("/Users/%1/Items/%2").arg(userId(m_server), id)).toObject();
    }

    QJsonObject details(const QString &link) {
        const auto parts = link.split(QLatin1Char('/'));
        const auto id = parts.value(parts.size() - 1);
        const auto o = fetchItem(id);
        QJsonObject out{{"title", o.value(QStringLiteral("Name")).toString()},
                        {"description", o.value(QStringLiteral("Overview")).toString()},
                        {"genres", o.value(QStringLiteral("Genres")).toArray()}};
        if (o.value(QStringLiteral("ImageTags")).toObject().contains(QStringLiteral("Primary")))
            out.insert("cover", image(id));
        if (!o.value(QStringLiteral("BackdropImageTags")).toArray().isEmpty())
            out.insert("background", image(id, QStringLiteral("Backdrop"), QStringLiteral("fillWidth=1280")));
        if (const auto studios = o.value(QStringLiteral("Studios")).toArray(); !studios.isEmpty())
            out.insert("author", studios.first().toObject().value(QStringLiteral("Name")).toString());
        // Aniyomi's statuses: 1 ongoing, 2 completed.
        const auto status = o.value(QStringLiteral("Status")).toString();
        out.insert("status", status == QLatin1String("Continuing") ? 1 : status == QLatin1String("Ended") ? 2 : 0);
        out.insert("fetchType", parts.first() == QLatin1String("series") && o.value(QStringLiteral("ChildCount")).toInt() > 1
                                    ? QStringLiteral("seasons")
                                    : QStringLiteral("episodes"));
        return out;
    }

    QJsonArray seasons(const QString &link) {
        const auto seriesId = link.section(QLatin1Char('/'), 1, 1);
        const auto series = fetchItem(seriesId).value(QStringLiteral("Name")).toString();
        const auto json = getJson(m_server, QStringLiteral("/Shows/%1/Seasons").arg(seriesId), {{"UserId", userId(m_server)}}).toObject();
        QJsonArray out;
        for (const auto &v : json.value(QStringLiteral("Items")).toArray()) {
            const auto s = v.toObject();
            const auto id = s.value(QStringLiteral("Id")).toString();
            QJsonObject season{{"url", QStringLiteral("season/%1/%2").arg(seriesId, id)},
                               {"title", series + QStringLiteral(" · ") + s.value(QStringLiteral("Name")).toString()},
                               {"season", s.value(QStringLiteral("IndexNumber")).toInt(-1)},
                               {"fetchType", "episodes"}};
            season.insert("cover", s.value(QStringLiteral("ImageTags")).toObject().contains(QStringLiteral("Primary")) ? image(id) : image(seriesId));
            out.append(season);
        }
        return out;
    }

    // Newest first, as extensions list them.
    QJsonArray episodes(const QString &link) {
        const auto parts = link.split(QLatin1Char('/'));
        QJsonArray list;
        if (parts.first() == QLatin1String("movie")) {
            const auto o = fetchItem(parts.value(1));
            return {QJsonObject{{"url", QStringLiteral("episode/") + parts.value(1)},
                                {"name", o.value(QStringLiteral("Name")).toString()},
                                {"number", 1},
                                {"summary", o.value(QStringLiteral("Overview")).toString()}}};
        }
        QList<QPair<QString, QString>> q{{"UserId", userId(m_server)}, {"Fields", "Overview,PremiereDate"}};
        if (parts.first() == QLatin1String("season"))
            q.append({"SeasonId", parts.value(2)});
        const auto items = getJson(m_server, QStringLiteral("/Shows/%1/Episodes").arg(parts.value(1)), q).toObject().value(QStringLiteral("Items")).toArray();
        // Several seasons in one list (a show whose specials are kept apart):
        // numbered in order, the season in the name.
        QSet<int> seasonNumbers;
        for (const auto &v : items)
            seasonNumbers.insert(v.toObject().value(QStringLiteral("ParentIndexNumber")).toInt(-1));
        const bool mixed = seasonNumbers.size() > 1;
        QJsonArray out;
        for (int i = 0; i < items.size(); ++i) {
            const auto e = items[i].toObject();
            const auto id = e.value(QStringLiteral("Id")).toString();
            const int index = e.value(QStringLiteral("IndexNumber")).toInt(-1);
            const auto name = e.value(QStringLiteral("Name")).toString();
            QJsonObject episode{{"url", QStringLiteral("episode/") + id},
                                {"number", mixed || index < 0 ? i + 1 : index},
                                {"summary", e.value(QStringLiteral("Overview")).toString()}};
            episode.insert("name", mixed ? QStringLiteral("S%1 E%2 · %3")
                                               .arg(e.value(QStringLiteral("ParentIndexNumber")).toInt())
                                               .arg(index)
                                               .arg(name)
                                         : name);
            if (const auto aired = QDateTime::fromString(e.value(QStringLiteral("PremiereDate")).toString(), Qt::ISODateWithMs); aired.isValid())
                episode.insert("uploaded", double(aired.toMSecsSinceEpoch()));
            if (e.value(QStringLiteral("ImageTags")).toObject().contains(QStringLiteral("Primary")))
                episode.insert("preview", image(id, QStringLiteral("Primary"), QStringLiteral("fillWidth=480")));
            out.prepend(episode);
        }
        return out;
    }

    // One hoster, the server, with its videos: the file as it is (the
    // player decodes nearly anything) and an HLS stream the server
    // transcodes to H.264 for devices that cannot.
    QJsonArray hosters(const QString &link) {
        const auto id = link.section(QLatin1Char('/'), 1, 1);
        const auto o = fetchItem(id);
        const auto sources = o.value(QStringLiteral("MediaSources")).toArray();
        if (sources.isEmpty())
            throw Error(QObject::tr("%1 has no video file for this.").arg(m_server.label), 404);
        const auto source = sources.first().toObject();
        const auto sourceId = source.value(QStringLiteral("Id")).toString();
        const auto token = session(m_server, false).token;
        const QJsonObject headers{{"Authorization", QString::fromUtf8(clientHeader(token))}};
        const double resume = o.value(QStringLiteral("UserData")).toObject().value(QStringLiteral("PlaybackPositionTicks")).toDouble() / kTicks;

        // Subtitles: files beside the video for both; the ones inside it
        // only for the transcoded stream (the file carries its own).
        QJsonArray external, inside;
        for (const auto &v : source.value(QStringLiteral("MediaStreams")).toArray()) {
            const auto s = v.toObject();
            if (s.value(QStringLiteral("Type")).toString() != QLatin1String("Subtitle") || !s.value(QStringLiteral("IsTextSubtitleStream")).toBool())
                continue;
            const QJsonObject track{{"url", m_server.url + QStringLiteral("/Videos/%1/%2/Subtitles/%3/0/Stream.vtt")
                                                               .arg(id, sourceId)
                                                               .arg(s.value(QStringLiteral("Index")).toInt())},
                                    {"lang", s.value(QStringLiteral("DisplayTitle")).toString(s.value(QStringLiteral("Language")).toString())}};
            (s.value(QStringLiteral("IsExternal")).toBool() ? external : inside).append(track);
        }
        QJsonArray transcodedSubs = external;
        for (const auto &t : inside)
            transcodedSubs.append(t);

        const auto skips = segments(id);
        const auto original = address(m_server, QStringLiteral("/Videos/%1/stream").arg(id),
                                      {{"static", "true"}, {"mediaSourceId", sourceId}, {"deviceId", deviceId()}});
        const auto transcoded = address(m_server, QStringLiteral("/Videos/%1/master.m3u8").arg(id),
                                        {{"MediaSourceId", sourceId},
                                         {"DeviceId", deviceId()},
                                         {"PlaySessionId", QUuid::createUuid().toString(QUuid::Id128)},
                                         {"VideoCodec", "h264"},
                                         {"AudioCodec", "aac"},
                                         {"MaxHeight", "1080"},
                                         {"VideoBitrate", "8000000"},
                                         {"AudioBitrate", "192000"},
                                         {"TranscodingMaxAudioChannels", "2"},
                                         {"SegmentContainer", "ts"},
                                         {"BreakOnNonKeyFrames", "True"}});
        const auto container = source.value(QStringLiteral("Container")).toString().toUpper();
        const QJsonArray videos{
            QJsonObject{{"id", QStringLiteral("jellyfin:%1:original").arg(id)},
                        {"url", original.toString(QUrl::FullyEncoded)},
                        {"title", QObject::tr("Original") + (container.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(container))},
                        {"preferred", true},
                        {"headers", headers},
                        {"subtitles", external},
                        {"timestamps", skips},
                        {"resume", resume}},
            QJsonObject{{"id", QStringLiteral("jellyfin:%1:hls").arg(id)},
                        {"url", transcoded.toString(QUrl::FullyEncoded)},
                        {"title", QObject::tr("Transcoded by the server (H.264)")},
                        {"resolution", 1080},
                        {"headers", headers},
                        {"subtitles", transcodedSubs},
                        {"timestamps", skips},
                        {"resume", resume}}};
        return {QJsonObject{{"id", QStringLiteral("jellyfin")}, {"name", m_server.label}, {"lazy", false}, {"videos", videos}}};
    }

    // Intros, credits and recaps the server found (Jellyfin 10.10's media
    // segments), as the timestamps extensions give: none on older servers.
    QJsonArray segments(const QString &id) {
        static const QHash<QString, QString> types{{"Intro", "Opening"}, {"Outro", "Ending"}, {"Recap", "Recap"}};
        QJsonArray out;
        try {
            const auto json = getJson(m_server, QStringLiteral("/MediaSegments/") + id).toObject();
            for (const auto &v : json.value(QStringLiteral("Items")).toArray()) {
                const auto s = v.toObject();
                const auto type = types.value(s.value(QStringLiteral("Type")).toString());
                if (type.isEmpty())
                    continue;
                out.append(QJsonObject{{"start", s.value(QStringLiteral("StartTicks")).toDouble() / kTicks},
                                       {"end", s.value(QStringLiteral("EndTicks")).toDouble() / kTicks},
                                       {"name", s.value(QStringLiteral("Type")).toString()},
                                       {"type", type}});
            }
        } catch (const Error &) {
        }
        return out;
    }
};

} // namespace

Headers jellyfinHeaders(const Server &server, bool fresh) { return {{"Authorization", clientHeader(session(server, fresh).token)}}; }

void forgetJellyfinLogins() {
    QMutexLocker lock(&g_sessionsLock);
    g_sessions.clear();
}

std::unique_ptr<ext::AnimeSource> createAnimeSource(const Server &server) {
    if (server.kind != QLatin1String("jellyfin"))
        return nullptr;
    ext::SourceEntry entry;
    entry.kind = ext::Kind::Aniyomi;
    entry.id = QStringLiteral("server-") + server.id;
    entry.key = QStringLiteral("aniyomi:") + entry.id;
    entry.name = server.label.isEmpty() ? server.url : server.label;
    // The reader's own library: whatever language it is in.
    entry.lang = QStringLiteral("*");
    entry.baseUrl = server.url;
    entry.pkg = QStringLiteral("jellyfin");
    return std::make_unique<JellyfinSource>(entry, server);
}

void videoWatched(const QString &sourceKey, const QString &episodeUrl, double seconds, bool completed) {
    static QMutex lock;
    static QHash<QString, qint64> sent; // item -> when (ms)
    const auto wrapped = ext::unwrapUrl(episodeUrl);
    if (!sourceKey.startsWith(QLatin1String("aniyomi:server-")) || !wrapped || !wrapped->url.startsWith(QLatin1String("episode/")))
        return;
    const auto server = byId(sourceKey.mid(int(qstrlen("aniyomi:server-"))));
    if (!server || server->kind != QLatin1String("jellyfin"))
        return;
    const auto id = wrapped->url.section(QLatin1Char('/'), 1, 1);
    // The player saves every few seconds; the server hears every 15.
    {
        QMutexLocker locked(&lock);
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (!completed && now - sent.value(id) < 15000)
            return;
        sent.insert(id, now);
    }
    try {
        const auto ticks = completed ? 0.0 : std::round(seconds * kTicks);
        auto response = send(*server, "POST", address(*server, QStringLiteral("/UserItems/%1/UserData").arg(id)),
                             QJsonDocument(QJsonObject{{"PlaybackPositionTicks", ticks}, {"Played", completed}}).toJson(QJsonDocument::Compact));
        // Before 10.9: played, or the position kept as a stopped playback.
        if (response.status == 404 || response.status == 405) {
            if (completed)
                response = send(*server, "POST", address(*server, QStringLiteral("/Users/%1/PlayedItems/%2").arg(userId(*server), id)));
            else
                response = send(*server, "POST", address(*server, QStringLiteral("/Sessions/Playing/Stopped")),
                                QJsonDocument(QJsonObject{{"ItemId", id}, {"PositionTicks", ticks}}).toJson(QJsonDocument::Compact));
        }
        if (!response.ok())
            qWarning("Jellyfin did not take the position (%d)", response.status);
    } catch (const Error &e) {
        qWarning().noquote() << "Jellyfin position not sent:" << e.message();
    }
}

} // namespace sc::servers
