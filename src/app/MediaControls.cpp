#include "MediaControls.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QUrl>
#include <QWindow>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

#ifdef SCANCORE_HAVE_DBUS
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QVariantMap>
#endif

#include <algorithm>
#include <cmath>

namespace sc {

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
// MediaControls_win.cpp, MediaControls_mac.mm.
void showSystemControls(void *&state, const NowPlaying &now, bool trackChanged, bool jumped);
#endif

#ifdef Q_OS_ANDROID
namespace {
constexpr auto kServiceClass = "com/mediumdev/scancore/ReadAloudService";

void jniCommand(JNIEnv *env, jclass, jstring word) {
    const jchar *chars = env->GetStringChars(word, nullptr);
    const QString text(reinterpret_cast<const QChar *>(chars), env->GetStringLength(word));
    env->ReleaseStringChars(word, chars);
    MediaControls::instance()->command(text);
}
} // namespace
#endif

#ifdef SCANCORE_HAVE_DBUS
namespace {

const QString kMprisPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kTrackPath = QStringLiteral("/com/mediumdev/scancore/track");

// MPRIS (specifications.freedesktop.org/mpris-spec): what desktops' media
// keys and player widgets talk to.
class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry CONSTANT)
    Q_PROPERTY(bool CanQuit READ no CONSTANT)
    Q_PROPERTY(bool CanRaise READ yes CONSTANT)
    Q_PROPERTY(bool HasTrackList READ no CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none CONSTANT)

public:
    explicit MprisRoot(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    QString identity() const { return QStringLiteral("SCANCORE"); }
    QString desktopEntry() const { return QStringLiteral("com.mediumdev.scancore"); }
    bool no() const { return false; }
    bool yes() const { return true; }
    QStringList none() const { return {}; }

public slots:
    void Raise() {
        for (auto *w : QGuiApplication::topLevelWindows()) {
            w->show();
            w->raise();
            w->requestActivate();
        }
    }
    void Quit() {}
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ status)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Rate READ one CONSTANT)
    Q_PROPERTY(double MinimumRate READ one CONSTANT)
    Q_PROPERTY(double MaximumRate READ one CONSTANT)
    Q_PROPERTY(double Volume READ one CONSTANT)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(bool CanGoNext READ yes CONSTANT)
    Q_PROPERTY(bool CanGoPrevious READ yes CONSTANT)
    Q_PROPERTY(bool CanPlay READ yes CONSTANT)
    Q_PROPERTY(bool CanPause READ yes CONSTANT)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ yes CONSTANT)

public:
    explicit MprisPlayer(QObject *parent) : QDBusAbstractAdaptor(parent) {}

    const NowPlaying &now() const { return MediaControls::instance()->current(); }
    QString status() const { return now().playing ? QStringLiteral("Playing") : QStringLiteral("Paused"); }
    QVariantMap metadata() const {
        const auto &n = now();
        QVariantMap m{{QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(kTrackPath))},
                      {QStringLiteral("xesam:title"), n.title},
                      {QStringLiteral("xesam:album"), n.album},
                      {QStringLiteral("xesam:artist"), QStringList{n.artist}}};
        if (n.duration > 0)
            m.insert(QStringLiteral("mpris:length"), qlonglong(n.duration * 1e6));
        if (n.cover.startsWith(QLatin1String("http")) || n.cover.startsWith(QLatin1String("file:")))
            m.insert(QStringLiteral("mpris:artUrl"), n.cover);
        return m;
    }
    double one() const { return 1.0; }
    // Microseconds.
    qlonglong position() const { return qlonglong(MediaControls::instance()->positionNow() * 1e6); }
    bool canSeek() const { return now().seekable; }
    bool yes() const { return true; }
    bool no() const { return false; }

    // Tells listeners what changed (D-Bus properties do not by themselves).
    void announce() {
        auto signal = QDBusMessage::createSignal(kMprisPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                 QStringLiteral("PropertiesChanged"));
        signal << QStringLiteral("org.mpris.MediaPlayer2.Player")
               << QVariantMap{{QStringLiteral("PlaybackStatus"), status()}, {QStringLiteral("Metadata"), metadata()},
                              {QStringLiteral("CanSeek"), canSeek()}}
               << QStringList{};
        QDBusConnection::sessionBus().send(signal);
    }
    void seeked() { emit Seeked(position()); }

signals:
    void Seeked(qlonglong Position);

public slots:
    void Play() { MediaControls::instance()->command(QStringLiteral("play")); }
    void Pause() { MediaControls::instance()->command(QStringLiteral("pause")); }
    void PlayPause() { MediaControls::instance()->command(QStringLiteral("toggle")); }
    void Stop() { MediaControls::instance()->command(QStringLiteral("stop")); }
    void Next() { MediaControls::instance()->command(QStringLiteral("next")); }
    void Previous() { MediaControls::instance()->command(QStringLiteral("previous")); }
    // Offsets and positions in microseconds.
    void Seek(qlonglong offset) {
        if (canSeek())
            seekTo(MediaControls::instance()->positionNow() + offset / 1e6);
    }
    void SetPosition(const QDBusObjectPath &track, qlonglong position) {
        if (canSeek() && track.path() == kTrackPath)
            seekTo(position / 1e6);
    }
    void OpenUri(const QString &) {}

private:
    void seekTo(double seconds) {
        const double end = now().duration > 0 ? now().duration : seconds;
        MediaControls::instance()->command(QStringLiteral("seek:%1").arg(std::clamp(seconds, 0.0, end)));
    }
};

} // namespace
#endif

MediaControls *MediaControls::instance() {
    static auto *controls = new MediaControls(QCoreApplication::instance());
    return controls;
}

MediaControls::MediaControls(QObject *parent) : QObject(parent) {
    m_since.start();
#ifdef Q_OS_ANDROID
    const JNINativeMethod methods[] = {
        {"nativeCommand", "(Ljava/lang/String;)V", reinterpret_cast<void *>(jniCommand)},
    };
    QJniEnvironment env;
    env.registerNativeMethods(kServiceClass, methods, 1);
#endif
}

void MediaControls::command(const QString &word) {
    QMetaObject::invokeMethod(this, [this, word] {
        if (word == QLatin1String("play"))
            emit playRequested();
        else if (word == QLatin1String("pause"))
            emit pauseRequested();
        else if (word == QLatin1String("toggle"))
            emit toggleRequested();
        else if (word == QLatin1String("next"))
            emit nextRequested();
        else if (word == QLatin1String("previous"))
            emit previousRequested();
        else if (word == QLatin1String("stop"))
            emit stopRequested();
        else if (word.startsWith(QLatin1String("seek:"))) {
            bool ok = false;
            const double seconds = word.mid(5).toDouble(&ok);
            if (ok && std::isfinite(seconds))
                emit seekRequested(seconds);
        }
    });
}

double MediaControls::positionNow() const {
    double at = m_now.position + (m_now.playing ? m_since.elapsed() / 1000.0 : 0);
    if (m_now.duration > 0)
        at = std::min(at, m_now.duration);
    return at;
}

void MediaControls::show(bool active, bool playing, const QString &title, const QString &subtitle, const QString &cover) {
    NowPlaying now;
    now.active = active;
    now.playing = playing;
    now.kind = QStringLiteral("readAloud");
    now.title = subtitle.isEmpty() ? title : subtitle;
    now.artist = title;
    now.album = title;
    now.cover = cover;
    show(now);
}

void MediaControls::show(const NowPlaying &now) {
    const bool trackChanged = !now.sameTrack(m_now);
    // A position that is not where the last one would have got to by now:
    // a seek (or a new track), which the systems need to hear about.
    const bool jumped = std::abs(now.position - positionNow()) > 1.5;
    if (!trackChanged && now.playing == m_now.playing && !jumped)
        return;
    m_now = now;
    m_since.restart();
    platformShow(now, trackChanged, jumped);
}

void MediaControls::platformShow(const NowPlaying &now, bool trackChanged, bool jumped) {
    Q_UNUSED(trackChanged)
    Q_UNUSED(jumped)
#ifdef Q_OS_ANDROID
    const bool music = now.kind == QLatin1String("music");
    QJniObject::callStaticMethod<void>(
        kServiceClass, "show",
        "(Landroid/content/Context;ZZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;JJZZ)V",
        QNativeInterface::QAndroidApplication::context().object(), jboolean(now.active), jboolean(now.playing),
        QJniObject::fromString(now.title).object<jstring>(), QJniObject::fromString(now.artist).object<jstring>(),
        QJniObject::fromString(now.album).object<jstring>(), QJniObject::fromString(now.cover).object<jstring>(),
        QJniObject::fromString(music ? tr("Music") : tr("Reading aloud")).object<jstring>(),
        jlong(now.duration * 1000), jlong(now.position * 1000), jboolean(now.seekable), jboolean(music));
#elif defined(SCANCORE_HAVE_DBUS)
    auto bus = QDBusConnection::sessionBus();
    const auto service = QStringLiteral("org.mpris.MediaPlayer2.scancore");
    if (!now.active) {
        if (m_mpris) {
            bus.unregisterObject(kMprisPath);
            bus.unregisterService(service);
            m_mpris->deleteLater();
            m_mpris = nullptr;
        }
        return;
    }
    if (!m_mpris) {
        m_mpris = new QObject(this);
        new MprisRoot(m_mpris);
        new MprisPlayer(m_mpris);
        bus.registerObject(kMprisPath, m_mpris, QDBusConnection::ExportAdaptors);
        bus.registerService(service);
    }
    if (auto *player = m_mpris->findChild<MprisPlayer *>()) {
        player->announce();
        if (jumped)
            player->seeked();
    }
#elif defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    showSystemControls(m_platform, now, trackChanged, jumped);
#endif
}

} // namespace sc

#ifdef SCANCORE_HAVE_DBUS
#include "MediaControls.moc"
#endif
