#include "Activity.h"

#include "App.h"

#include "core/Database.h"
#include "core/Task.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace sc {

namespace {

constexpr qint64 kTickMs = 10000;
// Without input for this long the time stops counting: someone reading
// pauses longer between scrolls than someone browsing.
constexpr qint64 kIdleBrowsingMs = 2 * 60000;
constexpr qint64 kIdleReadingMs = 5 * 60000;
constexpr qint64 kPlayingMs = 30000;
// Chapters finished before activity was kept, worked out once from the
// reading history. Every device arrives at the same rows, so sync keeps the
// larger count rather than adding them up.
const QString kHistoryDevice = QStringLiteral("history");
const QStringList kMedia{QStringLiteral("comic"), QStringLiteral("novel"), QStringLiteral("anime")};

QString today() { return QDate::currentDate().toString(Qt::ISODate); }

struct Day {
    qint64 seconds[4] = {0, 0, 0, 0}; // comic, novel, anime, app
    qint64 units[3] = {0, 0, 0};
    qint64 mediaSeconds() const { return seconds[0] + seconds[1] + seconds[2]; }
    qint64 allSeconds() const { return mediaSeconds() + seconds[3]; }
    qint64 allUnits() const { return units[0] + units[1] + units[2]; }
    // A day on the streak: something finished, or a minute of reading or watching.
    bool active() const { return allUnits() > 0 || mediaSeconds() >= 60; }
};

int mediaIndex(const QString &media) {
    const int i = kMedia.indexOf(media);
    return i >= 0 ? i : 3;
}

QMap<QString, Day> loadDays(const QString &like = QStringLiteral("%")) {
    QMap<QString, Day> days;
    for (const auto &value : Database::query(QStringLiteral("SELECT day, media, SUM(seconds) AS s, SUM(units) AS u FROM activity "
                                                            "WHERE day LIKE ? GROUP BY day, media"),
                                             {like})) {
        const auto row = value.toMap();
        auto &day = days[row.value("day").toString()];
        const int i = mediaIndex(row.value("media").toString());
        day.seconds[i] += row.value("s").toLongLong();
        if (i < 3)
            day.units[i] += row.value("u").toLongLong();
    }
    return days;
}

// {seconds, comic, novel, anime, units, byMedia: {comic: seconds, …}}
QVariantMap totals(const QMap<QString, Day> &days, const QString &from, const QString &to) {
    Day sum;
    for (auto it = days.lowerBound(from); it != days.end() && it.key() <= to; ++it) {
        for (int i = 0; i < 4; ++i)
            sum.seconds[i] += it->seconds[i];
        for (int i = 0; i < 3; ++i)
            sum.units[i] += it->units[i];
    }
    return {{"seconds", sum.allSeconds()},
            {"mediaSeconds", sum.mediaSeconds()},
            {"units", sum.allUnits()},
            {"comic", sum.units[0]},
            {"novel", sum.units[1]},
            {"anime", sum.units[2]},
            {"byMedia", QVariantMap{{"comic", sum.seconds[0]}, {"novel", sum.seconds[1]}, {"anime", sum.seconds[2]}, {"app", sum.seconds[3]}}}};
}

// Longest run of active days, and the run that reaches today (or yesterday,
// so a streak is not lost before today's reading).
QPair<int, int> streaks(const QMap<QString, Day> &days, const QDate &end) {
    int longest = 0;
    int run = 0;
    QDate previous;
    for (auto it = days.begin(); it != days.end(); ++it) {
        if (!it->active())
            continue;
        const auto date = QDate::fromString(it.key(), Qt::ISODate);
        run = previous.isValid() && previous.addDays(1) == date ? run + 1 : 1;
        previous = date;
        longest = std::max(longest, run);
    }
    int current = 0;
    QDate day = end;
    if (!days.value(day.toString(Qt::ISODate)).active())
        day = day.addDays(-1);
    while (days.value(day.toString(Qt::ISODate)).active()) {
        ++current;
        day = day.addDays(-1);
    }
    return {current, longest};
}

QVariantMap dayMap(const Day &day) {
    QVariantMap out;
    for (int i = 0; i < 3; ++i)
        out.insert(kMedia[i], QVariantMap{{"seconds", day.seconds[i]}, {"units", day.units[i]}});
    out.insert(QStringLiteral("app"), QVariantMap{{"seconds", day.seconds[3]}});
    return out;
}

} // namespace

Activity::Activity(QObject *parent) : QObject(parent) {
    m_lastInput = QDateTime::currentMSecsSinceEpoch();
    if (auto *app = QCoreApplication::instance()) {
        app->installEventFilter(this);
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            tick();
            flush();
        });
    }
    if (auto *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            // Count up to the moment the app goes to the background.
            if (state != Qt::ApplicationActive) {
                tick();
                flush();
            } else {
                m_lastInput = QDateTime::currentMSecsSinceEpoch();
            }
        });
    connect(&m_timer, &QTimer::timeout, this, &Activity::tick);
    m_timer.start(kTickMs);
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    QTimer::singleShot(3000, this, &Activity::backfill);
}

Activity *Activity::instance() {
    static auto *activity = [] {
        auto *created = new Activity;
        QJSEngine::setObjectOwnership(created, QJSEngine::CppOwnership);
        return created;
    }();
    return activity;
}

QString Activity::deviceId() {
    auto id = Database::pref(QStringLiteral("device.id")).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Database::setPref(QStringLiteral("device.id"), id);
    }
    return id;
}

QString Activity::deviceName() {
    const auto host = QSysInfo::machineHostName();
    if (!host.isEmpty() && host != QLatin1String("localhost"))
        return host;
    return QSysInfo::prettyProductName();
}

bool Activity::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        m_lastInput = QDateTime::currentMSecsSinceEpoch();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void Activity::setMedia(const QString &media) {
    if (media == m_media)
        return;
    tick(); // the time so far belongs to what was open
    // Incognito reading and watching is not counted.
    m_media = kMedia.contains(media) && !App::instance()->incognito() ? media : QString();
    m_lastInput = QDateTime::currentMSecsSinceEpoch();
}

void Activity::playing() { m_lastPlaying = QDateTime::currentMSecsSinceEpoch(); }

void Activity::listened(const QVariantMap &song, int seconds) {
    if (seconds <= 0 || seconds > 600)
        return;
    const auto title = song.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty())
        return;
    const auto artist = song.value(QStringLiteral("artists")).toStringList().join(QStringLiteral(", "));
    auto key = song.value(QStringLiteral("id")).toString();
    if (key.isEmpty())
        key = title.toLower() + QLatin1Char('|') + artist.toLower();
    const auto cover = song.value(QStringLiteral("cover")).toString();
    const auto day = today();
    // Off the interface's thread: a write every few seconds while music plays.
    runInBackground([day, key, title, artist, cover, seconds] {
        try {
            Database::exec(QStringLiteral("INSERT INTO music_plays(day, song, title, artist, cover, seconds) VALUES(?, ?, ?, ?, ?, ?) "
                                          "ON CONFLICT(day, song) DO UPDATE SET seconds = seconds + excluded.seconds, "
                                          "title = excluded.title, artist = excluded.artist, cover = excluded.cover"),
                           {day, key, title, artist, cover, seconds});
        } catch (const Error &) {
        }
    });
}

void Activity::tick() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // A machine that slept leaves a long gap; count one tick of it at most.
    const qint64 elapsed = std::clamp<qint64>(now - m_lastTick, 0, kTickMs + 5000);
    m_lastTick = now;
    if (elapsed <= 0)
        return;
    const bool active = QGuiApplication::applicationState() == Qt::ApplicationActive;
    bool counts;
    if (m_media == QLatin1String("anime") && now - m_lastPlaying < kPlayingMs)
        counts = true; // watching, perhaps on another screen
    else if (!active)
        counts = false;
    else if (m_media == QLatin1String("comic") || m_media == QLatin1String("novel"))
        counts = now - m_lastInput < kIdleReadingMs;
    else
        counts = now - m_lastInput < kIdleBrowsingMs;
    if (counts)
        m_pending[m_media.isEmpty() ? QStringLiteral("app") : m_media] += elapsed;
    if (++m_ticks % 6 == 0)
        flush();
}

void Activity::flush() {
    const auto device = deviceId();
    const auto day = today();
    bool wrote = false;
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const qint64 seconds = it.value() / 1000;
        if (seconds <= 0)
            continue;
        it.value() -= seconds * 1000;
        try {
            Database::exec(QStringLiteral("INSERT INTO activity(day, device, media, seconds) VALUES(?, ?, ?, ?) "
                                          "ON CONFLICT(day, device, media) DO UPDATE SET seconds = seconds + excluded.seconds"),
                           {day, device, it.key(), seconds});
            wrote = true;
        } catch (const Error &) {
        }
    }
    if (wrote)
        notify();
}

void Activity::notify() {
    ++m_revision;
    emit changed();
}

void Activity::unitFinished(const QString &media) {
    if (!kMedia.contains(media))
        return;
    Database::exec(QStringLiteral("INSERT INTO activity(day, device, media, units) VALUES(?, ?, ?, 1) "
                                  "ON CONFLICT(day, device, media) DO UPDATE SET units = units + 1"),
                   {today(), deviceId(), media});
    QMetaObject::invokeMethod(instance(), [] { instance()->notify(); });
}

void Activity::backfill() {
    if (Database::pref(QStringLiteral("activity.backfilled")).toBool())
        return;
    runInBackground([] {
        try {
            Database::exec(QStringLiteral(R"(
                INSERT INTO activity(day, device, media, units)
                SELECT date(updated_at, 'unixepoch', 'localtime'), ?, substr(series_id, 1, instr(series_id, ':') - 1), COUNT(*)
                FROM progress WHERE completed = 1 AND instr(series_id, ':') > 0
                GROUP BY 1, 3
                ON CONFLICT(day, device, media) DO UPDATE SET units = MAX(units, excluded.units)
            )"),
                           {kHistoryDevice});
            Database::setPref(QStringLiteral("activity.backfilled"), true);
            QMetaObject::invokeMethod(instance(), [] { instance()->notify(); });
        } catch (const Error &e) {
            qWarning().noquote() << "Activity history:" << e.message();
        }
    });
}

QJsonArray Activity::rowsForSync() {
    QJsonArray rows;
    for (const auto &value : Database::query(QStringLiteral("SELECT day, device, media, seconds, units FROM activity "
                                                            "WHERE device = ? OR device = ?"),
                                             {deviceId(), kHistoryDevice})) {
        const auto row = value.toMap();
        rows.append(QJsonArray{row.value("day").toString(), row.value("device").toString() == kHistoryDevice,
                               row.value("media").toString(), row.value("seconds").toLongLong(),
                               row.value("units").toLongLong()});
    }
    return rows;
}

void Activity::mergeRows(const QString &device, const QJsonArray &rows) {
    if (device.isEmpty() || device == deviceId() || device == kHistoryDevice)
        return;
    Database::transaction([&] {
        for (const auto &value : rows) {
            const auto row = value.toArray();
            const auto day = row.at(0).toString();
            const auto media = row.at(2).toString();
            if (day.size() != 10 || (media != QLatin1String("app") && !kMedia.contains(media)))
                continue;
            if (row.at(1).toBool())
                Database::query(QStringLiteral("INSERT INTO activity(day, device, media, units) VALUES(?, ?, ?, ?) "
                                               "ON CONFLICT(day, device, media) DO UPDATE SET units = MAX(units, excluded.units)"),
                                {day, kHistoryDevice, media, row.at(4).toInteger()});
            else
                Database::query(QStringLiteral("INSERT OR REPLACE INTO activity(day, device, media, seconds, units) "
                                               "VALUES(?, ?, ?, ?, ?)"),
                                {day, device, media, row.at(3).toInteger(), row.at(4).toInteger()});
        }
    });
    QMetaObject::invokeMethod(instance(), [] { instance()->notify(); });
}

bool Activity::saveImage(const QImage &image, const QUrl &file) const {
    // A content:// address on Android, a path elsewhere.
    QFile out(file.isLocalFile() ? file.toLocalFile() : file.toString());
    return !image.isNull() && out.open(QIODevice::WriteOnly) && image.save(&out, "PNG");
}

Task *Activity::summary() {
    flush();
    return Task::run([]() -> QVariant {
        const auto days = loadDays();
        const auto now = QDate::currentDate();
        const auto iso = [](const QDate &d) { return d.toString(Qt::ISODate); };
        const auto [current, longest] = streaks(days, now);
        // The heat map and graphs show the last 53 weeks, from a Monday.
        const auto first = now.addDays(-(52 * 7) - (now.dayOfWeek() - 1));
        QVariantMap recent;
        for (auto it = days.lowerBound(iso(first)); it != days.end(); ++it)
            recent.insert(it.key(), dayMap(*it));
        return QVariantMap{{"today", totals(days, iso(now), iso(now))},
                           {"week", totals(days, iso(now.addDays(1 - now.dayOfWeek())), iso(now))},
                           {"month", totals(days, iso(QDate(now.year(), now.month(), 1)), iso(now))},
                           {"all", totals(days, QStringLiteral("0000"), iso(now))},
                           {"streak", current},
                           {"longestStreak", longest},
                           {"since", days.isEmpty() ? QString() : days.firstKey()},
                           {"first", iso(first)},
                           {"todayKey", iso(now)},
                           {"days", recent}};
    });
}

Task *Activity::year(int year) {
    flush();
    return Task::run([year]() -> QVariant {
        const auto days = loadDays(QStringLiteral("%1-%").arg(year));
        const auto from = QDate(year, 1, 1);
        const auto to = QDate(year, 12, 31);
        QVariantList months;
        for (int m = 1; m <= 12; ++m) {
            const auto start = QDate(year, m, 1);
            months.append(totals(days, start.toString(Qt::ISODate), start.addMonths(1).addDays(-1).toString(Qt::ISODate)));
        }
        QVariantList weekdays;
        qint64 byWeekday[7] = {0, 0, 0, 0, 0, 0, 0};
        int activeDays = 0;
        QString bestDay;
        qint64 bestDaySeconds = -1;
        for (auto it = days.begin(); it != days.end(); ++it) {
            const auto date = QDate::fromString(it.key(), Qt::ISODate);
            byWeekday[date.dayOfWeek() - 1] += it->allSeconds();
            activeDays += it->active() ? 1 : 0;
            if (it->mediaSeconds() > bestDaySeconds) {
                bestDaySeconds = it->mediaSeconds();
                bestDay = it.key();
            }
        }
        for (const auto seconds : byWeekday)
            weekdays.append(seconds);
        const qint64 start = QDateTime(from, QTime(0, 0)).toSecsSinceEpoch();
        const qint64 end = QDateTime(to.addDays(1), QTime(0, 0)).toSecsSinceEpoch();
        // Every series something was finished in this year, in one pass:
        // the tops (overall and per media), genres and authors weighted by
        // what was finished, and how many series each media had.
        const auto rows = Database::query(QStringLiteral(R"(
            SELECT s.id, s.title, s.cover, s.media, s.source_url AS sourceUrl, s.extra, s.genres, s.author,
                   COUNT(*) AS units, MIN(p.updated_at) AS firstAt
            FROM progress p JOIN series s ON s.id = p.series_id
            WHERE p.completed = 1 AND p.updated_at >= ? AND p.updated_at < ?
            GROUP BY s.id ORDER BY units DESC, firstAt
        )"),
                                          {start, end});
        QVariantList series;
        QVariantMap topByMedia{{"comic", QVariantList()}, {"novel", QVariantList()}, {"anime", QVariantList()}};
        QVariantMap seriesCount{{"comic", 0}, {"novel", 0}, {"anime", 0}};
        QHash<QString, QPair<QString, qint64>> genres; // lower case -> {as written, units}
        QHash<QString, QPair<QString, qint64>> authors;
        QVariantMap first;
        qint64 firstAt = std::numeric_limits<qint64>::max();
        const auto weigh = [](QHash<QString, QPair<QString, qint64>> &into, const QString &name, qint64 units) {
            const auto key = name.trimmed().toLower();
            if (key.isEmpty() || key == QLatin1String("unknown"))
                return;
            auto &entry = into[key];
            if (entry.first.isEmpty())
                entry.first = name.trimmed();
            entry.second += units;
        };
        const auto ranked = [](const QHash<QString, QPair<QString, qint64>> &from, int count) {
            QList<QPair<QString, qint64>> list(from.begin(), from.end());
            std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
            QVariantList out;
            for (int i = 0; i < std::min<int>(count, int(list.size())); ++i)
                out.append(QVariantMap{{"name", list[i].first}, {"units", list[i].second}});
            return out;
        };
        // Units from series that know their genres or author: many library
        // entries never had their details fetched, so the genre and author
        // slides only count when most of the year is covered.
        qint64 allUnits = 0, genreUnits = 0, bookUnits = 0, authorUnits = 0;
        for (const auto &value : rows) {
            auto row = value.toMap();
            const auto media = row.value("media").toString();
            const auto units = row.value("units").toLongLong();
            const auto genreList = row.take("genres").toString().split(QStringLiteral(", "), Qt::SkipEmptyParts);
            for (const auto &genre : genreList)
                weigh(genres, genre, units);
            allUnits += units;
            if (!genreList.isEmpty())
                genreUnits += units;
            if (media != QLatin1String("anime")) {
                bookUnits += units;
                const auto author = row.value("author").toString().trimmed();
                weigh(authors, author, units);
                if (!author.isEmpty() && author.compare(QLatin1String("unknown"), Qt::CaseInsensitive) != 0)
                    authorUnits += units;
            }
            row.insert(QStringLiteral("extra"), QJsonDocument::fromJson(row.value("extra").toByteArray()).toVariant());
            if (series.size() < 6)
                series.append(row);
            if (seriesCount.contains(media)) {
                seriesCount[media] = seriesCount[media].toInt() + 1;
                auto list = topByMedia[media].toList();
                if (list.size() < 5) {
                    list.append(row);
                    topByMedia[media] = list;
                }
            }
            if (row.value("firstAt").toLongLong() < firstAt) {
                firstAt = row.value("firstAt").toLongLong();
                first = row;
            }
        }
        // Music: time listened, the songs and the artist played most.
        const auto songs = Database::query(QStringLiteral(R"(
            SELECT song, MAX(title) AS title, MAX(artist) AS artist, MAX(cover) AS cover, SUM(seconds) AS seconds,
                   COUNT(*) AS days
            FROM music_plays WHERE day LIKE ? GROUP BY song ORDER BY seconds DESC
        )"),
                                           {QStringLiteral("%1-%").arg(year)});
        qint64 musicSeconds = 0;
        QVariantList topSongs;
        QHash<QString, QPair<QString, qint64>> artists;
        for (const auto &value : songs) {
            const auto row = value.toMap();
            const auto s = row.value("seconds").toLongLong();
            musicSeconds += s;
            if (topSongs.size() < 5)
                topSongs.append(row);
            // Each artist of a song gets its time.
            for (const auto &name : row.value("artist").toString().split(QStringLiteral(", "), Qt::SkipEmptyParts))
                weigh(artists, name, s);
        }
        const QVariantMap music{{"seconds", musicSeconds}, {"songs", int(songs.size())}, {"top", topSongs},
                                {"artist", ranked(artists, 1).value(0)}};

        return QVariantMap{{"year", year},
                           {"music", music},
                           {"total", totals(days, from.toString(Qt::ISODate), to.toString(Qt::ISODate))},
                           {"months", months},
                           {"weekdays", weekdays},
                           {"activeDays", activeDays},
                           {"longestStreak", streaks(days, to).second},
                           {"bestDay", bestDay},
                           {"bestDaySeconds", std::max<qint64>(0, bestDaySeconds)},
                           {"top", series},
                           {"topByMedia", topByMedia},
                           {"seriesCount", seriesCount},
                           {"series", int(rows.size())},
                           {"genres", genreUnits * 2 >= allUnits ? ranked(genres, 5) : QVariantList()},
                           {"author", authorUnits * 2 >= bookUnits ? ranked(authors, 1).value(0) : QVariant()},
                           {"first", first}};
    });
}

} // namespace sc
