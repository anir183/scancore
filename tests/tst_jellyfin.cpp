// Jellyfin as one of the reader's own servers: logging in (and again when
// the token is refused), its shows and films as an anime source (listings,
// details, seasons, episodes), the videos it plays (the file and an HLS
// transcode, with the login, subtitles, skips and the server's resume
// point) and the position sent back while an episode plays. The server is a
// mock on jellyfin.test (Http's test handler).

#include "core/Database.h"
#include "core/Http.h"
#include "extensions/AnimeSource.h"
#include "sources/anime/Playback.h"
#include "sources/anime/Registry.h"
#include "sources/servers/Servers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

using namespace sc;

namespace {

QJsonObject series(const QString &id, const QString &name, int seasons, bool image = true) {
    QJsonObject o{{"Id", id}, {"Name", name}, {"Type", "Series"}, {"ChildCount", seasons}};
    if (image)
        o.insert("ImageTags", QJsonObject{{"Primary", "tag"}});
    return o;
}

QJsonObject episode(const QString &id, const QString &name, int season, int number) {
    return {{"Id", id}, {"Name", name}, {"Type", "Episode"}, {"ParentIndexNumber", season}, {"IndexNumber", number},
            {"Overview", name + " overview"}, {"PremiereDate", "2023-09-29T00:00:00.0000000Z"}, {"ImageTags", QJsonObject{{"Primary", "t"}}}};
}

} // namespace

class Jellyfin : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QMutex m_mutex;
    QStringList m_asked;
    QList<QByteArray> m_userData;
    int m_logins = 0;
    QByteArray m_token = "tok1";
    bool m_oldServer = false;
    QByteArray m_lastAuth;

    QStringList asked() {
        QMutexLocker lock(&m_mutex);
        return m_asked;
    }
    std::shared_ptr<ext::AnimeSource> source() { return anime::Registry::byKey(QStringLiteral("aniyomi:server-j1")); }

    std::optional<Response> answer(const Request &request) {
        if (request.url.host() != QLatin1String("jellyfin.test"))
            return std::nullopt;
        const auto path = request.url.path();
        const QUrlQuery query(request.url);
        QByteArray auth;
        for (const auto &[k, v] : request.headers)
            if (k.compare("Authorization", Qt::CaseInsensitive) == 0)
                auth = v;
        Response r;
        r.finalUrl = request.url;
        r.status = 200;
        r.contentType = "application/json";
        const auto json = [&](const QJsonValue &v) {
            r.body = v.isArray() ? QJsonDocument(v.toArray()).toJson() : QJsonDocument(v.toObject()).toJson();
        };
        QMutexLocker lock(&m_mutex);
        m_asked.append(QString::fromLatin1(request.method) + QLatin1Char(' ') + request.url.toString(QUrl::PrettyDecoded));
        m_lastAuth = auth;
        if (!auth.startsWith("MediaBrowser Client=\"SCANCORE\"") || !auth.contains("DeviceId=\"")) {
            r.status = 400;
            return r;
        }
        if (path == QLatin1String("/Users/AuthenticateByName")) {
            const auto body = QJsonDocument::fromJson(request.body).object();
            if (body.value("Username").toString() != QLatin1String("anna") || body.value("Pw").toString() != QLatin1String("secret")) {
                r.status = 401;
                return r;
            }
            ++m_logins;
            json(QJsonObject{{"AccessToken", QString::fromLatin1(m_token)}, {"User", QJsonObject{{"Id", "u1"}}}});
            return r;
        }
        if (!auth.contains("Token=\"" + m_token + '"')) {
            r.status = 401;
            return r;
        }
        if (path == QLatin1String("/Users/u1/Items")) {
            QJsonArray items{series("s1", "Frieren", 1), series("s2", "Bocchi the Rock!", 2),
                             QJsonObject{{"Id", "m1"}, {"Name", "Your Name"}, {"Type", "Movie"}}};
            if (query.queryItemValue("SearchTerm") == QLatin1String("frie"))
                items = {items.first()};
            json(QJsonObject{{"Items", items}, {"TotalRecordCount", query.queryItemValue("SearchTerm").isEmpty() ? 85 : int(items.size())}});
        } else if (path == QLatin1String("/Users/u1/Items/s1")) {
            json(QJsonObject{{"Id", "s1"}, {"Name", "Frieren"}, {"Type", "Series"}, {"ChildCount", 1}, {"Overview", "An elf mage."},
                             {"Genres", QJsonArray{"Fantasy"}}, {"Status", "Ended"}, {"Studios", QJsonArray{QJsonObject{{"Name", "Madhouse"}}}},
                             {"ImageTags", QJsonObject{{"Primary", "x"}}}, {"BackdropImageTags", QJsonArray{"b"}}});
        } else if (path == QLatin1String("/Users/u1/Items/s2")) {
            json(QJsonObject{{"Id", "s2"}, {"Name", "Bocchi the Rock!"}, {"Type", "Series"}, {"ChildCount", 2}, {"Status", "Continuing"}});
        } else if (path == QLatin1String("/Users/u1/Items/m1")) {
            json(QJsonObject{{"Id", "m1"}, {"Name", "Your Name"}, {"Type", "Movie"}, {"Overview", "A film."}});
        } else if (path == QLatin1String("/Users/u1/Items/e2")) {
            json(QJsonObject{
                {"Id", "e2"},
                {"Name", "Episode 2"},
                {"UserData", QJsonObject{{"PlaybackPositionTicks", 6000000000.0}}},
                {"MediaSources", QJsonArray{QJsonObject{
                                     {"Id", "ms2"},
                                     {"Container", "mkv"},
                                     {"MediaStreams", QJsonArray{QJsonObject{{"Type", "Video"}, {"Index", 0}},
                                                                 QJsonObject{{"Type", "Subtitle"}, {"Index", 2}, {"IsTextSubtitleStream", true},
                                                                             {"IsExternal", false}, {"DisplayTitle", "English - ASS"}},
                                                                 QJsonObject{{"Type", "Subtitle"}, {"Index", 3}, {"IsTextSubtitleStream", true},
                                                                             {"IsExternal", true}, {"DisplayTitle", "Español - SRT"}},
                                                                 QJsonObject{{"Type", "Subtitle"}, {"Index", 4}, {"IsTextSubtitleStream", false},
                                                                             {"DisplayTitle", "PGS"}}}}}}}});
        } else if (path == QLatin1String("/Users/u1/Items/e9")) {
            json(QJsonObject{{"Id", "e9"}, {"Name", "No file"}});
        } else if (path == QLatin1String("/Shows/s1/Episodes")) {
            json(QJsonObject{{"Items", QJsonArray{episode("e1", "The Journey's End", 1, 1), episode("e2", "It Didn't Have to Be Magic", 1, 2),
                                                  episode("e3", "Killing Magic", 1, 3)}}});
        } else if (path == QLatin1String("/Shows/s2/Seasons")) {
            json(QJsonObject{{"Items", QJsonArray{QJsonObject{{"Id", "sp"}, {"Name", "Specials"}, {"IndexNumber", 0}},
                                                  QJsonObject{{"Id", "se1"}, {"Name", "Season 1"}, {"IndexNumber", 1},
                                                              {"ImageTags", QJsonObject{{"Primary", "y"}}}}}}});
        } else if (path == QLatin1String("/Shows/s2/Episodes")) {
            if (query.queryItemValue("SeasonId") == QLatin1String("se1"))
                json(QJsonObject{{"Items", QJsonArray{episode("b1", "Lonely Rolling Bocchi", 1, 1)}}});
            else
                json(QJsonObject{{"Items", QJsonArray{episode("bs", "Special", 0, 1), episode("b1", "Lonely Rolling Bocchi", 1, 1),
                                                      episode("b2", "Second", 1, 2)}}});
        } else if (path == QLatin1String("/MediaSegments/e2")) {
            json(QJsonObject{{"Items", QJsonArray{QJsonObject{{"Type", "Intro"}, {"StartTicks", 0}, {"EndTicks", 900000000.0}},
                                                  QJsonObject{{"Type", "Outro"}, {"StartTicks", 13000000000.0}, {"EndTicks", 13900000000.0}},
                                                  QJsonObject{{"Type", "Commercial"}, {"StartTicks", 1}, {"EndTicks", 2}}}}});
        } else if (path.startsWith(QLatin1String("/UserItems/")) && path.endsWith(QLatin1String("/UserData"))) {
            if (m_oldServer) {
                r.status = 404;
                return r;
            }
            m_userData.append(path.toUtf8() + ' ' + request.body);
            r.status = 200;
        } else if (path == QLatin1String("/Sessions/Playing/Stopped") || path.startsWith(QLatin1String("/Users/u1/PlayedItems/"))) {
            m_userData.append(path.toUtf8() + ' ' + request.body);
            r.status = 204;
        } else {
            r.status = 404;
        }
        return r;
    }

private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        Database::open(m_dir.filePath(QStringLiteral("library.db")));
        Http::setTestHandler([this](const Request &request) { return answer(request); });
        servers::Server server;
        server.id = QStringLiteral("j1");
        server.kind = QStringLiteral("jellyfin");
        server.label = QStringLiteral("Home Jellyfin");
        server.url = QStringLiteral("http://jellyfin.test");
        server.username = QStringLiteral("anna");
        server.password = QStringLiteral("secret");
        servers::save({server});
        anime::Registry::rebuild();
        QVERIFY(source());
    }
    void cleanupTestCase() { Http::setTestHandler({}); }

    void listed() {
        auto s = source();
        QCOMPARE(s->label(), QStringLiteral("Home Jellyfin"));
        QVERIFY(s->isServer());
        QCOMPARE(s->imageReferer(), QStringLiteral("server:j1"));
        QCOMPARE(s->config().language, QStringLiteral("*"));
        // Only Jellyfin servers are anime sources; comics never see them.
        QVERIFY(!servers::createSource(*servers::byId(QStringLiteral("j1"))));
    }

    void listings() {
        auto s = source();
        const auto latest = s->browse(QStringLiteral("latest"), 1);
        QCOMPARE(m_logins, 1);
        QCOMPARE(latest.items.size(), 3);
        QVERIFY(latest.hasNextPage); // 85 in all, 40 a page
        QCOMPARE(latest.items[0].url, QStringLiteral("aniyomi:server-j1:series/s1"));
        QCOMPARE(latest.items[0].title, QStringLiteral("Frieren"));
        QCOMPARE(latest.items[0].cover, QStringLiteral("http://jellyfin.test/Items/s1/Images/Primary?fillHeight=600&quality=90"));
        QVERIFY(!latest.items[0].seasons);
        QVERIFY(latest.items[1].seasons);
        QCOMPARE(latest.items[2].url, QStringLiteral("aniyomi:server-j1:movie/m1"));
        QVERIFY(latest.items[2].cover.isEmpty());
        QVERIFY(asked().last().contains(QStringLiteral("SortBy=DateCreated,SortName&SortOrder=Descending")));
        QVERIFY(asked().last().contains(QStringLiteral("StartIndex=0&Limit=40")));

        s->browse(QStringLiteral("popular"), 3);
        QVERIFY(asked().last().contains(QStringLiteral("SortBy=CommunityRating")));
        QVERIFY(asked().last().contains(QStringLiteral("StartIndex=80")));
        const auto found = s->search(QStringLiteral("frie"));
        QCOMPARE(found.size(), 1);
        QVERIFY(asked().last().contains(QStringLiteral("SearchTerm=frie")));
        // One login for all of them.
        QCOMPARE(m_logins, 1);
    }

    void details() {
        auto s = source();
        const auto frieren = s->details(QStringLiteral("aniyomi:server-j1:series/s1"));
        QCOMPARE(frieren.value("title").toString(), QStringLiteral("Frieren"));
        QCOMPARE(frieren.value("description").toString(), QStringLiteral("An elf mage."));
        QCOMPARE(frieren.value("author").toString(), QStringLiteral("Madhouse"));
        QCOMPARE(frieren.value("status").toInt(), 2);
        QCOMPARE(frieren.value("fetchType").toString(), QStringLiteral("episodes"));
        QCOMPARE(frieren.value("background").toString(), QStringLiteral("http://jellyfin.test/Items/s1/Images/Backdrop?fillWidth=1280&quality=90"));
        QCOMPARE(frieren.value("genres").toArray().first().toString(), QStringLiteral("Fantasy"));
        const auto bocchi = s->details(QStringLiteral("aniyomi:server-j1:series/s2"));
        QCOMPARE(bocchi.value("fetchType").toString(), QStringLiteral("seasons"));
        QCOMPARE(bocchi.value("status").toInt(), 1);
        QCOMPARE(s->webUrl(QStringLiteral("aniyomi:server-j1:series/s2")), QStringLiteral("http://jellyfin.test/web/#/details?id=s2"));
    }

    void episodes() {
        auto s = source();
        // Oldest first, links wrapped, the server's numbers.
        const auto list = s->episodes(QStringLiteral("aniyomi:server-j1:series/s1"));
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].toObject().value("url").toString(), QStringLiteral("aniyomi:server-j1:episode/e1"));
        QCOMPARE(list[0].toObject().value("name").toString(), QStringLiteral("The Journey's End"));
        QCOMPARE(list[2].toObject().value("number").toDouble(), 3.0);
        QCOMPARE(list[1].toObject().value("preview").toString(), QStringLiteral("http://jellyfin.test/Items/e2/Images/Primary?fillWidth=480&quality=90"));
        QVERIFY(list[1].toObject().value("uploaded").toDouble() > 0);
        QCOMPARE(list[1].toObject().value("summary").toString(), QStringLiteral("It Didn't Have to Be Magic overview"));

        // Seasons, each an anime of its own; the series' cover where a
        // season has none.
        const auto seasons = s->seasons(QStringLiteral("aniyomi:server-j1:series/s2"));
        QCOMPARE(seasons.size(), 2);
        QCOMPARE(seasons[1].url, QStringLiteral("aniyomi:server-j1:season/s2/se1"));
        QCOMPARE(seasons[1].title, QStringLiteral("Bocchi the Rock! · Season 1"));
        QCOMPARE(seasons[1].season, 1.0);
        QCOMPARE(seasons[0].cover, QStringLiteral("http://jellyfin.test/Items/s2/Images/Primary?fillHeight=600&quality=90"));
        const auto one = s->episodes(seasons[1].url);
        QCOMPARE(one.size(), 1);
        QVERIFY(asked().last().contains(QStringLiteral("SeasonId=se1")));

        // Specials and a season in one list: numbered in order, the season
        // in the name.
        const auto mixed = s->episodes(QStringLiteral("aniyomi:server-j1:series/s2"));
        QCOMPARE(mixed.size(), 3);
        QCOMPARE(mixed[0].toObject().value("name").toString(), QStringLiteral("S0 E1 · Special"));
        QCOMPARE(mixed[2].toObject().value("number").toDouble(), 3.0);

        // A film: itself, as one episode.
        const auto film = s->episodes(QStringLiteral("aniyomi:server-j1:movie/m1"));
        QCOMPARE(film.size(), 1);
        QCOMPARE(film[0].toObject().value("url").toString(), QStringLiteral("aniyomi:server-j1:episode/m1"));
        QCOMPARE(film[0].toObject().value("name").toString(), QStringLiteral("Your Name"));
    }

    void videos() {
        auto s = source();
        const auto hosters = s->hosters(QStringLiteral("aniyomi:server-j1:episode/e2"));
        QCOMPARE(hosters.size(), 1);
        const auto hoster = hosters[0].toObject();
        QCOMPARE(hoster.value("name").toString(), QStringLiteral("Home Jellyfin"));
        QVERIFY(!hoster.value("lazy").toBool());
        const auto videos = anime::videoList(hoster.value("videos").toArray());
        QCOMPARE(videos.size(), 2);
        const auto original = videos[0].toMap();
        QVERIFY(original.value("preferred").toBool());
        QCOMPARE(original.value("title").toString(), QStringLiteral("Original (MKV)"));
        const QUrl file(original.value("url").toString());
        QCOMPARE(file.path(), QStringLiteral("/Videos/e2/stream"));
        QCOMPARE(QUrlQuery(file).queryItemValue("static"), QStringLiteral("true"));
        QCOMPARE(QUrlQuery(file).queryItemValue("mediaSourceId"), QStringLiteral("ms2"));
        // The login in a header, never in the address.
        QVERIFY(!original.value("url").toString().contains(QLatin1String("tok1")));
        QVERIFY(original.value("headers").toMap().value("Authorization").toString().contains(QLatin1String("Token=\"tok1\"")));
        QCOMPARE(original.value("resume").toDouble(), 600.0);
        // The file's own subtitles stay in it; the one beside it is added.
        const auto subs = original.value("subtitles").toList();
        QCOMPARE(subs.size(), 1);
        QCOMPARE(subs[0].toMap().value("url").toString(), QStringLiteral("http://jellyfin.test/Videos/e2/ms2/Subtitles/3/0/Stream.vtt"));
        QCOMPARE(subs[0].toMap().value("lang").toString(), QStringLiteral("Español - SRT"));
        // Intro and credits to skip; commercials are not.
        const auto skips = original.value("skips").toList();
        QCOMPARE(skips.size(), 2);
        QCOMPARE(skips[0].toMap().value("type").toString(), QStringLiteral("op"));
        QCOMPARE(skips[0].toMap().value("end").toDouble(), 90.0);
        QCOMPARE(skips[1].toMap().value("type").toString(), QStringLiteral("ed"));
        QCOMPARE(skips[1].toMap().value("start").toDouble(), 1300.0);

        const auto hls = videos[1].toMap();
        const QUrl stream(hls.value("url").toString());
        QCOMPARE(stream.path(), QStringLiteral("/Videos/e2/master.m3u8"));
        QCOMPARE(QUrlQuery(stream).queryItemValue("VideoCodec"), QStringLiteral("h264"));
        QCOMPARE(QUrlQuery(stream).queryItemValue("MediaSourceId"), QStringLiteral("ms2"));
        QVERIFY(!QUrlQuery(stream).queryItemValue("PlaySessionId").isEmpty());
        // A transcode loses the file's subtitles: both text ones are added
        // (pictures cannot be).
        QCOMPARE(hls.value("subtitles").toList().size(), 2);
        QCOMPARE(hls.value("resume").toDouble(), 600.0);

        QVERIFY_THROWS_EXCEPTION(Error, s->hosters(QStringLiteral("aniyomi:server-j1:episode/e9")));
    }

    void loginAgain() {
        // The server forgot SCANCORE's token: one new login, then it goes on.
        {
            QMutexLocker lock(&m_mutex);
            m_token = "tok2";
        }
        const int before = m_logins;
        QCOMPARE(source()->browse(QStringLiteral("latest"), 1).items.size(), 3);
        QCOMPARE(m_logins, before + 1);
        // The video's login is the new token.
        const auto videos = source()->hosters(QStringLiteral("aniyomi:server-j1:episode/e2"))[0].toObject().value("videos").toArray();
        QVERIFY(videos[0].toObject().value("headers").toObject().value("Authorization").toString().contains(QLatin1String("tok2")));
        // A wrong password says so.
        auto server = *servers::byId(QStringLiteral("j1"));
        server.password = QStringLiteral("wrong");
        server.id = QStringLiteral("j2");
        try {
            servers::authHeaders(server, true);
            QFAIL("no error");
        } catch (const Error &e) {
            QCOMPARE(e.status(), 401);
            QVERIFY2(e.message().contains(QLatin1String("refused the login")), qPrintable(e.message()));
        }
    }

    void positionSent() {
        const auto key = QStringLiteral("aniyomi:server-j1");
        const auto url = QStringLiteral("aniyomi:server-j1:episode/e1");
        servers::videoWatched(key, url, 61.5, false);
        // A few seconds later: not sent again yet.
        servers::videoWatched(key, url, 66.5, false);
        QCOMPARE(m_userData.size(), 1);
        auto sent = QJsonDocument::fromJson(m_userData.last().mid(m_userData.last().indexOf(' ') + 1)).object();
        QVERIFY(m_userData.last().startsWith("/UserItems/e1/UserData"));
        QCOMPARE(sent.value("PlaybackPositionTicks").toDouble(), 615000000.0);
        QCOMPARE(sent.value("Played").toBool(), false);
        // Watched: always sent, the position cleared.
        servers::videoWatched(key, url, 1400, true);
        QCOMPARE(m_userData.size(), 2);
        sent = QJsonDocument::fromJson(m_userData.last().mid(m_userData.last().indexOf(' ') + 1)).object();
        QCOMPARE(sent.value("Played").toBool(), true);
        QCOMPARE(sent.value("PlaybackPositionTicks").toDouble(), 0.0);
        // Other sources and links are left alone.
        servers::videoWatched(QStringLiteral("aniyomi:12345"), QStringLiteral("aniyomi:12345:/ep/1"), 10, true);
        servers::videoWatched(key, QStringLiteral("aniyomi:server-j1:series/s1"), 10, true);
        QCOMPARE(m_userData.size(), 2);

        // Before Jellyfin 10.9: played items, or a stopped playback.
        m_oldServer = true;
        servers::videoWatched(key, QStringLiteral("aniyomi:server-j1:episode/e3"), 30, false);
        QVERIFY(m_userData.last().startsWith("/Sessions/Playing/Stopped"));
        sent = QJsonDocument::fromJson(m_userData.last().mid(m_userData.last().indexOf(' ') + 1)).object();
        QCOMPARE(sent.value("ItemId").toString(), QStringLiteral("e3"));
        QCOMPARE(sent.value("PositionTicks").toDouble(), 300000000.0);
        servers::videoWatched(key, QStringLiteral("aniyomi:server-j1:episode/e3"), 1400, true);
        QVERIFY(m_userData.last().startsWith("/Users/u1/PlayedItems/e3"));
        m_oldServer = false;
    }
};

QTEST_GUILESS_MAIN(Jellyfin)
#include "tst_jellyfin.moc"
