#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace sc {

// What plays without a screen of its own, as the system's controls show it.
struct NowPlaying {
    bool active = false;
    bool playing = false;
    // "music" or "readAloud" (Android names the notification after it).
    QString kind = QStringLiteral("readAloud");
    QString title;  // the song, or the chapter read
    QString artist; // the song's artists, or the book
    QString album;
    QString cover;  // http(s) or file URL; "" for none
    double duration = 0; // seconds; 0 when not known
    double position = 0;
    bool seekable = false;

    bool sameTrack(const NowPlaying &o) const {
        return active == o.active && kind == o.kind && title == o.title && artist == o.artist && album == o.album &&
               cover == o.cover && duration == o.duration && seekable == o.seekable;
    }
};

// The system's controls for what SCANCORE is playing without a screen of
// its own (music, reading aloud), the way music apps show there:
// - Android: a media session and its notification (ReadAloudService.java):
//   the lock screen, the media controls, Samsung's Now Bar, headphones'
//   buttons, with the cover, the time and seeking.
// - Linux: MPRIS (media keys, the desktop's player widget).
// - Windows: the System Media Transport Controls (MediaControls_win.cpp).
// - macOS: Now Playing and the media keys (MediaControls_mac.mm).
class MediaControls : public QObject {
    Q_OBJECT

public:
    static MediaControls *instance();

    // Shows (or updates) the controls, or with `active` false takes them
    // away. Send it when the track, playing or paused, or the position
    // (a seek) changes; the systems move the time on by themselves.
    void show(const NowPlaying &now);
    // Reading aloud: the book and the chapter, no time.
    void show(bool active, bool playing, const QString &title, const QString &subtitle, const QString &cover);

    // From the platform (any thread is fine; delivered on the GUI thread):
    // "play", "pause", "toggle", "next", "previous", "stop", "seek:<seconds>".
    void command(const QString &word);

    // Where the track is now, moved on from the last show() while playing.
    double positionNow() const;
    const NowPlaying &current() const { return m_now; }

signals:
    void playRequested();
    void pauseRequested();
    void toggleRequested();
    void nextRequested();
    void previousRequested();
    void stopRequested();
    void seekRequested(double seconds);

private:
    explicit MediaControls(QObject *parent = nullptr);
    void platformShow(const NowPlaying &now, bool trackChanged, bool jumped);

    NowPlaying m_now;
    QElapsedTimer m_since;
    QObject *m_mpris = nullptr;
    void *m_platform = nullptr; // Windows and macOS keep their state here
};

} // namespace sc
