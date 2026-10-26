#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include <memory>

namespace sc {

struct AudioCore;

// Music: mpv with no picture, one per app, so a song keeps playing while
// pages change (MusicQueue in QML says what plays next). Every mpv call is
// asynchronous; events arrive on the interface thread.
class AudioPlayer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    // Playing and not waiting for the network.
    Q_PROPERTY(bool playing READ playing NOTIFY pausedChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    static AudioPlayer *instance();
    static AudioPlayer *create(QQmlEngine *, QJSEngine *) { return instance(); }
    ~AudioPlayer() override;

    bool available() const;
    QString source() const { return m_source; }
    bool paused() const { return m_paused; }
    bool playing() const { return !m_source.isEmpty() && !m_paused && !m_loading; }
    bool loading() const { return m_loading; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    QString error() const { return m_error; }

    // Plays `url` from `start` seconds.
    // `headers` are the ones a music plugin asks for (User-Agent, Referer,
    // others), for this song only.
    Q_INVOKABLE void load(const QString &url, double start = 0, const QVariantMap &headers = {});
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void stop();
    void setVolume(double volume);
    void setMuted(bool muted);
    // The system's controls (lock screen, media controls, MPRIS, Windows'
    // and macOS' now playing) for the song playing: its title, artists,
    // cover and album (or anime), with the time and seeking; none once
    // `title` is empty.
    Q_INVOKABLE void showControls(const QString &title, const QString &subtitle, const QString &cover,
                                  const QString &album = QString());

signals:
    void sourceChanged();
    void pausedChanged();
    void loadingChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void errorChanged();
    // The song played to its end.
    void finished();
    // The system's next and previous buttons (the queue answers them).
    void nextRequested();
    void previousRequested();

private:
    static QString playable(const QString &url);
    QVariantMap m_headers;
    explicit AudioPlayer(QObject *parent = nullptr);
    void drainEvents();
    void setError(const QString &error);

    std::shared_ptr<AudioCore> m_core;
    QString m_source;
    bool m_paused = true;
    bool m_loading = false;
    bool m_ended = false;
    double m_position = 0;
    double m_reported = -1;
    double m_duration = 0;
    double m_volume = 100;
    bool m_muted = false;
    QString m_error;
    QString m_title, m_subtitle, m_cover, m_album;
    void updateControls();
};

} // namespace sc
