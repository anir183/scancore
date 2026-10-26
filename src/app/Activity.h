#pragma once

#include "core/Task.h"

#include <QHash>
#include <QImage>
#include <QUrl>
#include <QJsonArray>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>

namespace sc {

// Time spent in SCANCORE and chapters or episodes finished, per day and
// media ("comic", "novel", "anime", or "app" for the rest of the app). Each
// device keeps its own rows, which sync copies to the others, so the
// Activity page adds up every device. Time counts while the app is in use:
// in the foreground with recent input, or while an episode plays.
class Activity : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    static Activity *instance();
    static Activity *create(QQmlEngine *, QJSEngine *) { return instance(); }

    int revision() const { return m_revision; }

    // This installation, as sync names it.
    static QString deviceId();
    static QString deviceName();

    // What is open: "comic", "novel", "anime", or "" for the rest of the app.
    Q_INVOKABLE void setMedia(const QString &media);
    // The player calls this while an episode plays.
    Q_INVOKABLE void playing();
    // Music calls this while a song plays: `seconds` more of `song`
    // ({id, title, artists, cover}), for Wrapped.
    Q_INVOKABLE void listened(const QVariantMap &song, int seconds);

    // Totals for today, this week, this month and all time, the streak, and
    // every day of the last 53 weeks: {today, week, month, all, streak,
    // longestStreak, since, days: [{day, comic: {seconds, units}, novel,
    // anime, app}]}.
    Q_INVOKABLE sc::Task *summary();
    // A year in review: totals, top series (overall and per media), genres,
    // the author read most, the first series finished, busiest month and
    // weekday, longest streak. Wrapped and the year card show it.
    Q_INVOKABLE sc::Task *year(int year);
    // Saves a picture of the year (grabToImage) as a PNG, for sharing.
    Q_INVOKABLE bool saveImage(const QImage &image, const QUrl &file) const;

    // A chapter or episode was finished (Library).
    static void unitFinished(const QString &media);
    // This device's rows, and rows from another device (sync).
    static QJsonArray rowsForSync();
    static void mergeRows(const QString &device, const QJsonArray &rows);

    // Writes the time counted since the last write.
    void flush();

signals:
    void changed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit Activity(QObject *parent = nullptr);
    void tick();
    void backfill();
    void notify();

    QString m_media;
    qint64 m_lastInput = 0;
    qint64 m_lastPlaying = 0;
    qint64 m_lastTick = 0;
    // Seconds not yet written, by media.
    QHash<QString, int> m_pending;
    QTimer m_timer;
    int m_ticks = 0;
    int m_revision = 0;
};

} // namespace sc
