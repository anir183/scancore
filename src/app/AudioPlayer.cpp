#include "AudioPlayer.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

#include "MediaControls.h"
#include "core/Http.h"
#include "MpvPlayer.h"

#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <clocale>
#include <cmath>

#ifdef SCANCORE_HAVE_MPV
#include <mpv/client.h>
#endif

namespace sc {

struct AudioCore {
#ifdef SCANCORE_HAVE_MPV
    mpv_handle *mpv = nullptr;
    ~AudioCore() {
        if (mpv)
            mpv_terminate_destroy(mpv);
    }
#endif
};

namespace {

enum Property : uint64_t { TimePos = 1, Duration, Pause, PausedForCache, IdleActive, Volume, Mute };

#ifdef SCANCORE_HAVE_MPV
void command(mpv_handle *mpv, const QStringList &args) {
    QList<QByteArray> utf8;
    for (const auto &a : args)
        utf8.append(a.toUtf8());
    QList<const char *> argv;
    for (const auto &a : utf8)
        argv.append(a.constData());
    argv.append(nullptr);
    mpv_command_async(mpv, 0, argv.data());
}

void setFlag(mpv_handle *mpv, const char *name, bool on) {
    int value = on ? 1 : 0;
    mpv_set_property_async(mpv, 0, name, MPV_FORMAT_FLAG, &value);
}
#endif

} // namespace

AudioPlayer::AudioPlayer(QObject *parent) : QObject(parent), m_core(std::make_shared<AudioCore>()) {
#ifdef SCANCORE_HAVE_MPV
    std::setlocale(LC_NUMERIC, "C");
    MpvPlayer::prepareAndroid();
    auto *mpv = mpv_create();
    if (!mpv)
        return;
    const auto option = [mpv](const char *name, const char *value) { mpv_set_option_string(mpv, name, value); };
    option("vo", "null");
    option("vid", "no");
    option("config", "no");
    option("load-scripts", "no");
    option("ytdl", "no");
    option("terminal", "no");
    option("idle", "yes");
    option("audio-client-name", "SCANCORE");
    option("gapless-audio", "weak");
    option("cache", "yes");
    option("demuxer-max-bytes", "32MiB");
    option("network-timeout", "30");
    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        return;
    }
    m_core->mpv = mpv;
    mpv_request_log_messages(mpv, "warn");
    mpv_observe_property(mpv, TimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, Duration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, Pause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, IdleActive, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, Volume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, Mute, "mute", MPV_FORMAT_FLAG);
    // mpv's thread says there are events; they are read on this one.
    mpv_set_wakeup_callback(
        mpv,
        [](void *self) {
            QMetaObject::invokeMethod(static_cast<AudioPlayer *>(self), &AudioPlayer::drainEvents, Qt::QueuedConnection);
        },
        this);
#endif
    // The system's buttons, while a song is ours to answer for.
    auto *controls = MediaControls::instance();
    const auto ours = [this] { return !m_source.isEmpty() && !m_title.isEmpty(); };
    connect(controls, &MediaControls::playRequested, this, [this, ours] { if (ours()) play(); });
    connect(controls, &MediaControls::pauseRequested, this, [this, ours] { if (ours()) pause(); });
    connect(controls, &MediaControls::toggleRequested, this, [this, ours] { if (ours()) toggle(); });
    connect(controls, &MediaControls::nextRequested, this, [this, ours] { if (ours()) emit nextRequested(); });
    connect(controls, &MediaControls::previousRequested, this, [this, ours] { if (ours()) emit previousRequested(); });
    connect(controls, &MediaControls::stopRequested, this, [this, ours] { if (ours()) { stop(); showControls({}, {}, {}); } });
    connect(controls, &MediaControls::seekRequested, this, [this, ours](double seconds) { if (ours()) seek(seconds); });
    // Playing or paused, the length once known and seeks show on the
    // controls (they move the time on by themselves in between).
    connect(this, &AudioPlayer::pausedChanged, this, &AudioPlayer::updateControls);
    connect(this, &AudioPlayer::durationChanged, this, &AudioPlayer::updateControls);
    connect(this, &AudioPlayer::loadingChanged, this, &AudioPlayer::updateControls);
}

void AudioPlayer::showControls(const QString &title, const QString &subtitle, const QString &cover, const QString &album) {
    m_title = title;
    m_subtitle = subtitle;
    m_cover = cover;
    m_album = album;
    updateControls();
}

void AudioPlayer::updateControls() {
    NowPlaying now;
    now.active = !m_title.isEmpty();
    now.playing = playing() || m_loading;
    now.kind = QStringLiteral("music");
    now.title = m_title;
    now.artist = m_subtitle;
    now.album = m_album;
    now.cover = m_cover;
    now.duration = duration();
    now.position = position();
    now.seekable = duration() > 0;
    MediaControls::instance()->show(now);
}

AudioPlayer::~AudioPlayer() {
#ifdef SCANCORE_HAVE_MPV
    if (m_core->mpv)
        mpv_set_wakeup_callback(m_core->mpv, nullptr, nullptr);
#endif
}

AudioPlayer *AudioPlayer::instance() {
    static auto *player = new AudioPlayer(QCoreApplication::instance());
    return player;
}

bool AudioPlayer::available() const {
#ifdef SCANCORE_HAVE_MPV
    return m_core->mpv != nullptr;
#else
    return false;
#endif
}

// Android: a file in a folder chosen there is a content:// address, which
// mpv reads through a descriptor the system opens for it.
QString AudioPlayer::playable(const QString &url) {
#ifdef Q_OS_ANDROID
    if (url.startsWith(QLatin1String("content:"))) {
        const jint fd = QJniObject::callStaticMethod<jint>("com/mediumdev/scancore/Folders", "openFd",
                                                           "(Landroid/content/Context;Ljava/lang/String;)I",
                                                           QNativeInterface::QAndroidApplication::context().object(),
                                                           QJniObject::fromString(url).object<jstring>());
        if (fd >= 0)
            return QStringLiteral("fdclose://%1").arg(fd);
    }
#endif
    return url;
}

void AudioPlayer::load(const QString &url, double start, const QVariantMap &headers) {
#ifdef SCANCORE_HAVE_MPV
    if (!m_core->mpv)
        return setError(QStringLiteral("Songs cannot play here."));
    m_source = url;
    m_headers = headers;
    m_ended = false;
    m_position = 0;
    m_duration = 0;
    m_loading = true;
    setError({});
    emit sourceChanged();
    emit positionChanged();
    emit durationChanged();
    emit loadingChanged();
    setFlag(m_core->mpv, "pause", false);
    // Each song with its own headers; the last one's go back to defaults.
    QString agent = QString::fromUtf8(Http::userAgent());
    QString referer;
    QStringList fields;
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const auto name = it.key().toLower();
        if (name == QLatin1String("user-agent"))
            agent = it.value().toString();
        else if (name == QLatin1String("referer"))
            referer = it.value().toString();
        else
            fields.append(it.key() + QStringLiteral(": ") + it.value().toString());
    }
    mpv_set_property_string(m_core->mpv, "user-agent", agent.toUtf8().constData());
    mpv_set_property_string(m_core->mpv, "referrer", referer.toUtf8().constData());
    mpv_set_property_string(m_core->mpv, "http-header-fields", fields.join(QLatin1Char(',')).toUtf8().constData());
    QStringList args{QStringLiteral("loadfile"), playable(url), QStringLiteral("replace")};
    if (start > 0) {
        args << QStringLiteral("-1") << QStringLiteral("start=%1").arg(start);
    }
    command(m_core->mpv, args);
#else
    Q_UNUSED(url)
    Q_UNUSED(start)
#endif
}

void AudioPlayer::play() {
#ifdef SCANCORE_HAVE_MPV
    if (!m_core->mpv || m_source.isEmpty())
        return;
    // At the end: from the start again.
    if (m_ended)
        return load(m_source, 0, m_headers);
    setFlag(m_core->mpv, "pause", false);
#endif
}

void AudioPlayer::pause() {
#ifdef SCANCORE_HAVE_MPV
    if (m_core->mpv)
        setFlag(m_core->mpv, "pause", true);
#endif
}

void AudioPlayer::toggle() { m_paused ? play() : pause(); }

void AudioPlayer::seek(double seconds) {
#ifdef SCANCORE_HAVE_MPV
    if (!m_core->mpv || m_source.isEmpty())
        return;
    m_position = std::max(0.0, seconds);
    m_reported = m_position;
    emit positionChanged();
    command(m_core->mpv, {QStringLiteral("seek"), QString::number(m_position), QStringLiteral("absolute")});
    updateControls();
#endif
}

void AudioPlayer::stop() {
#ifdef SCANCORE_HAVE_MPV
    if (m_core->mpv)
        command(m_core->mpv, {QStringLiteral("stop")});
#endif
    m_source.clear();
    m_loading = false;
    emit sourceChanged();
    emit loadingChanged();
}

void AudioPlayer::setVolume(double volume) {
#ifdef SCANCORE_HAVE_MPV
    if (!m_core->mpv)
        return;
    double v = std::clamp(volume, 0.0, 100.0);
    mpv_set_property_async(m_core->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &v);
#else
    Q_UNUSED(volume)
#endif
}

void AudioPlayer::setMuted(bool muted) {
#ifdef SCANCORE_HAVE_MPV
    if (m_core->mpv)
        setFlag(m_core->mpv, "mute", muted);
#else
    Q_UNUSED(muted)
#endif
}

void AudioPlayer::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void AudioPlayer::drainEvents() {
#ifdef SCANCORE_HAVE_MPV
    auto *mpv = m_core->mpv;
    if (!mpv)
        return;
    while (true) {
        auto *event = mpv_wait_event(mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *p = static_cast<mpv_event_property *>(event->data);
            const bool has = p->format != MPV_FORMAT_NONE && p->data;
            const double number = has && p->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(p->data) : 0.0;
            const bool flag = has && p->format == MPV_FORMAT_FLAG && *static_cast<int *>(p->data) != 0;
            switch (event->reply_userdata) {
            case TimePos:
                if (!has || m_source.isEmpty())
                    break;
                m_position = number;
                // A few times a second is enough for the scrubber and lyrics.
                if (std::abs(m_position - m_reported) >= 0.1) {
                    m_reported = m_position;
                    emit positionChanged();
                }
                if (m_loading) {
                    m_loading = false;
                    emit loadingChanged();
                    emit pausedChanged();
                }
                break;
            case Duration:
                m_duration = number;
                emit durationChanged();
                break;
            case Pause:
                m_paused = flag;
                emit pausedChanged();
                break;
            case PausedForCache:
                // Waiting for the network mid-song; the start is the
                // first position (TimePos) that arrives.
                if (!m_source.isEmpty() && m_position > 0 && flag != m_loading) {
                    m_loading = flag;
                    emit loadingChanged();
                    emit pausedChanged();
                }
                break;
            case IdleActive:
                // Idle after a file that ran out: the end of the song.
                if (flag && !m_source.isEmpty() && !m_loading && m_duration > 0 && !m_ended) {
                    m_ended = true;
                    m_paused = true;
                    emit pausedChanged();
                    emit finished();
                }
                break;
            case Volume:
                m_volume = number;
                emit volumeChanged();
                break;
            case Mute:
                m_muted = flag;
                emit volumeChanged();
                break;
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                m_loading = false;
                emit loadingChanged();
                setError(QStringLiteral("This song could not be played (%1).").arg(QString::fromUtf8(mpv_error_string(end->error))));
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto *log = static_cast<mpv_event_log_message *>(event->data);
            qWarning("mpv audio [%s] %s", log->prefix, QByteArray(log->text).trimmed().constData());
            break;
        }
        default: break;
        }
    }
#endif
}

} // namespace sc
