#include "Speech.h"

#include "Piper.h"

#include <atomic>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

#ifdef SCANCORE_HAVE_TTS
#include <QTextToSpeech>
#include <QVoice>
#endif

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace sc {

namespace {

// Engines take a few thousand characters at a time (Android: 4000).
constexpr int kMaxChunk = 3000;

QStringList chunks(const QString &text) {
    if (text.size() <= kMaxChunk)
        return {text};
    QStringList out;
    static const QRegularExpression sentenceEnd(QStringLiteral(R"([.!?。！？…]["'”’)\]]*\s+)"));
    qsizetype start = 0;
    while (start < text.size()) {
        if (text.size() - start <= kMaxChunk) {
            out.append(text.mid(start));
            break;
        }
        // The last sentence end that fits, else the last space, else a cut.
        qsizetype cut = -1;
        for (auto it = sentenceEnd.globalMatch(text, start); it.hasNext();) {
            const auto m = it.next();
            if (m.capturedEnd() - start > kMaxChunk)
                break;
            cut = m.capturedEnd();
        }
        if (cut <= start)
            cut = text.lastIndexOf(QLatin1Char(' '), start + kMaxChunk);
        if (cut <= start)
            cut = start + kMaxChunk;
        out.append(text.mid(start, cut - start));
        start = cut;
    }
    return out;
}

// Voices in the app's language first, then by language and name.
void sortVoices(QVariantList &voices) {
    const auto mine = QLocale().name().section(QLatin1Char('_'), 0, 0);
    std::stable_sort(voices.begin(), voices.end(), [&](const QVariant &a, const QVariant &b) {
        const auto la = a.toMap().value(QStringLiteral("language")).toString();
        const auto lb = b.toMap().value(QStringLiteral("language")).toString();
        const bool ma = la.startsWith(mine), mb = lb.startsWith(mine);
        if (ma != mb)
            return ma;
        if (la != lb)
            return la < lb;
        return a.toMap().value(QStringLiteral("name")).toString() < b.toMap().value(QStringLiteral("name")).toString();
    });
}

#ifdef Q_OS_ANDROID
constexpr auto kSpeechClass = "com/mediumdev/scancore/Speech";

QString javaString(JNIEnv *env, jstring s) {
    if (!s)
        return {};
    const jchar *chars = env->GetStringChars(s, nullptr);
    QString out(reinterpret_cast<const QChar *>(chars), env->GetStringLength(s));
    env->ReleaseStringChars(s, chars);
    return out;
}

// The engine's callbacks use this, never Speech::instance(): Android can
// answer during TextToSpeech's own constructor (with no engine to bind, it
// reports the failure at once), which would re-enter instance() while it
// is still being initialised and abort (2.2.1 crash on opening Sources).
std::atomic<Speech *> g_speech{nullptr};

void jniReady(JNIEnv *env, jclass, jboolean ok, jstring voicesJson) {
    QVariantList voices;
    for (const auto &v : QJsonDocument::fromJson(javaString(env, voicesJson).toUtf8()).array())
        voices.append(v.toObject().toVariantMap());
    if (auto *speech = g_speech.load())
        speech->engineReady(ok, voices);
}
void jniStarted(JNIEnv *, jclass, jint chunk) {
    if (auto *speech = g_speech.load())
        speech->engineStarted(chunk);
}
void jniDone(JNIEnv *, jclass, jint chunk) {
    if (auto *speech = g_speech.load())
        speech->engineFinished(chunk);
}
void jniError(JNIEnv *env, jclass, jint chunk, jstring message) {
    if (auto *speech = g_speech.load())
        speech->engineFailed(chunk, javaString(env, message));
}
#endif

} // namespace

Speech *Speech::object() {
    static auto *speech = [] {
        auto *created = new Speech;
        QJSEngine::setObjectOwnership(created, QJSEngine::CppOwnership);
        return created;
    }();
    return speech;
}

Speech *Speech::instance() {
    auto *speech = object();
    // Started once the singleton exists, outside its initialisation, so an
    // engine answering straight away finds it whole.
    speech->ensureEngine();
    return speech;
}

Speech::Speech(QObject *parent) : QObject(parent) {
#if defined(Q_OS_ANDROID) && !defined(SCANCORE_HAVE_TTS)
    g_speech = this;
#endif
}

Speech::~Speech() = default;

void Speech::ensureEngine() {
    if (m_engineStarted)
        return;
    m_engineStarted = true;
    if (auto *piper = Piper::instance(); piper->available()) {
        connect(piper, &Piper::voicesChanged, this, &Speech::updateVoices);
        connect(piper, &Piper::started, this, &Speech::engineStarted);
        connect(piper, &Piper::finished, this, &Speech::engineFinished);
        connect(piper, &Piper::failed, this, &Speech::engineFailed);
        updateVoices();
    }
    if (qEnvironmentVariable("SCANCORE_TTS_ENGINE").startsWith(QLatin1String("timer"))) {
        m_testEngine = true;
        m_testSilence = qEnvironmentVariable("SCANCORE_TTS_ENGINE") == QLatin1String("timer-silence");
        m_testTimer = new QTimer(this);
        m_testTimer->setSingleShot(true);
        connect(m_testTimer, &QTimer::timeout, this, &Speech::testNext);
        engineReady(true, {QVariantMap{{"id", "test"}, {"name", "Test voice"}, {"language", "en_US"}}});
        return;
    }
#if defined(SCANCORE_HAVE_TTS)
    // Development hook: SCANCORE_TTS_ENGINE=mock speaks silently (tests).
    const auto engine = qEnvironmentVariable("SCANCORE_TTS_ENGINE");
    m_tts = engine.isEmpty() ? new QTextToSpeech(this) : new QTextToSpeech(engine, this);
    const auto ready = [this] {
        if (m_tts->state() == QTextToSpeech::Error) {
            engineReady(false, {});
            return;
        }
        QVariantList voices;
        for (const auto &voice : m_tts->findVoices())
            voices.append(QVariantMap{{"id", voice.name() + QLatin1Char('|') + voice.locale().name()},
                                      {"name", voice.name()},
                                      {"language", voice.locale().name()}});
        engineReady(true, voices);
    };
    connect(m_tts, &QTextToSpeech::aboutToSynthesize, this, [this](qsizetype id) {
        if (m_current)
            engineFinished(m_current);
        m_current = m_qtIds.take(id);
        if (m_current)
            engineStarted(m_current);
    });
    connect(m_tts, &QTextToSpeech::stateChanged, this, [this, ready](QTextToSpeech::State state) {
        if (!m_available && state != QTextToSpeech::Synthesizing)
            ready();
        // The queue ran out.
        if (state == QTextToSpeech::Ready && m_current) {
            const auto last = m_current;
            m_current = 0;
            engineFinished(last);
        }
    });
    connect(m_tts, &QTextToSpeech::errorOccurred, this, [this](QTextToSpeech::ErrorReason, const QString &message) {
        engineFailed(m_current, message);
    });
    if (m_tts->state() != QTextToSpeech::Synthesizing)
        ready();
#elif defined(Q_OS_ANDROID)
    static const bool registered = [] {
        const JNINativeMethod methods[] = {
            {"nativeReady", "(ZLjava/lang/String;)V", reinterpret_cast<void *>(jniReady)},
            {"nativeStarted", "(I)V", reinterpret_cast<void *>(jniStarted)},
            {"nativeDone", "(I)V", reinterpret_cast<void *>(jniDone)},
            {"nativeError", "(ILjava/lang/String;)V", reinterpret_cast<void *>(jniError)},
        };
        QJniEnvironment env;
        return env.registerNativeMethods(kSpeechClass, methods, 4);
    }();
    if (registered)
        QJniObject::callStaticMethod<void>(kSpeechClass, "init", "(Landroid/content/Context;)V",
                                           QNativeInterface::QAndroidApplication::context().object());
#endif
}

void Speech::engineReady(bool ok, const QVariantList &voices) {
    QMetaObject::invokeMethod(this, [this, ok, voices] {
        m_systemVoices = voices;
        m_available = ok;
        updateVoices();
    });
}

// The neural voices first (they sound best), then the system's.
void Speech::updateVoices() {
    QVariantList neural;
    for (const auto &v : Piper::instance()->voices()) {
        auto m = v.toMap();
        m.insert(QStringLiteral("name"), m.value(QStringLiteral("name")).toString() + tr(" (neural)"));
        neural.append(m);
    }
    m_hasNeural = !neural.isEmpty();
    sortVoices(neural);
    // espeak-ng (Linux) lists each language once more for every variant
    // ("English (Great Britain)+Adam", ~135 each, 14,000 in all): only the
    // language itself, where it is there.
    QSet<QString> bases;
    for (const auto &v : std::as_const(m_systemVoices)) {
        const auto m = v.toMap();
        if (!m.value(QStringLiteral("name")).toString().contains(QLatin1Char('+')))
            bases.insert(m.value(QStringLiteral("name")).toString() + QLatin1Char('|') + m.value(QStringLiteral("language")).toString());
    }
    QVariantList system;
    for (const auto &v : std::as_const(m_systemVoices)) {
        const auto m = v.toMap();
        const auto name = m.value(QStringLiteral("name")).toString();
        const auto plus = name.indexOf(QLatin1Char('+'));
        if (plus > 0 && bases.contains(name.left(plus) + QLatin1Char('|') + m.value(QStringLiteral("language")).toString()))
            continue;
        system.append(v);
    }
    sortVoices(system);
    m_voices = neural + system;
    emit voicesChanged();
    emit changed();
}

void Speech::engineStarted(int chunk) {
    QMetaObject::invokeMethod(this, [this, chunk] {
        const auto it = m_queued.constFind(chunk);
        if (it == m_queued.cend())
            return;
        // A long paragraph is several chunks; it starts with its first.
        if (!m_queued.contains(chunk - 1) || m_queued.value(chunk - 1).tag != it->tag)
            emit started(it->tag);
    });
}

void Speech::engineFinished(int chunk) {
    QMetaObject::invokeMethod(this, [this, chunk] {
        const auto it = m_queued.constFind(chunk);
        if (it == m_queued.cend())
            return;
        const auto done = *it;
        m_queued.erase(it);
        if (done.last)
            emit finished(done.tag);
        if (m_queued.isEmpty())
            emit changed();
    });
}

void Speech::engineFailed(int chunk, const QString &message) {
    QMetaObject::invokeMethod(this, [this, chunk, message] {
        Q_UNUSED(chunk)
        m_queued.clear();
        emit changed();
        emit failed(message.isEmpty() ? tr("The voice could not read this.") : message);
    });
}

void Speech::testNext() {
    if (m_testCurrent) {
        const int done = m_testCurrent;
        m_testCurrent = 0;
        engineFinished(done);
    }
    if (m_testQueue.isEmpty())
        return;
    const auto [chunk, ms] = m_testQueue.takeFirst();
    m_testCurrent = chunk;
    engineStarted(chunk);
    m_testTimer->start(ms);
}

void Speech::sendChunk(int chunk, const QString &text) {
    if (!m_neural.isEmpty()) {
        Piper::instance()->speak(chunk, text);
        return;
    }
    if (m_testEngine) {
        m_testQueue.append({chunk, int(15 * text.size())});
        if (!m_testCurrent && !m_testTimer->isActive())
            m_testTimer->start(0);
        return;
    }
#if defined(SCANCORE_HAVE_TTS)
    m_qtIds.insert(m_tts->enqueue(text), chunk);
#elif defined(Q_OS_ANDROID)
    QJniObject::callStaticMethod<void>(kSpeechClass, "speak", "(Ljava/lang/String;I)V", QJniObject::fromString(text).object<jstring>(),
                                       jint(chunk));
#else
    Q_UNUSED(chunk)
    Q_UNUSED(text)
#endif
}

void Speech::enqueue(const QString &text, int tag) {
    const auto parts = chunks(text.simplified());
    if (parts.isEmpty() || parts.first().isEmpty())
        return;
    const bool wasEmpty = m_queued.isEmpty();
    for (int i = 0; i < parts.size(); ++i) {
        const int chunk = m_nextChunk++;
        m_queued.insert(chunk, {tag, i == parts.size() - 1});
        sendChunk(chunk, parts[i]);
    }
    if (wasEmpty)
        emit changed();
}

bool Speech::queuesSilence() const {
    if (!m_neural.isEmpty())
        return false;
    if (m_testEngine)
        return m_testSilence;
#if defined(Q_OS_ANDROID) && !defined(SCANCORE_HAVE_TTS)
    return true;
#else
    return false;
#endif
}

void Speech::enqueueSilence(int ms, int tag) {
    if (m_testEngine && m_testSilence && ms > 0) {
        const int chunk = m_nextChunk++;
        m_queued.insert(chunk, {tag, true});
        m_testQueue.append({chunk, ms});
        if (!m_testCurrent && !m_testTimer->isActive())
            m_testTimer->start(0);
        return;
    }
#if defined(Q_OS_ANDROID) && !defined(SCANCORE_HAVE_TTS)
    if (ms <= 0)
        return;
    const bool wasEmpty = m_queued.isEmpty();
    const int chunk = m_nextChunk++;
    m_queued.insert(chunk, {tag, true});
    QJniObject::callStaticMethod<void>(kSpeechClass, "silence", "(II)V", jint(ms), jint(chunk));
    if (wasEmpty)
        emit changed();
#else
    Q_UNUSED(ms)
    Q_UNUSED(tag)
#endif
}

void Speech::stop() {
    const bool had = !m_queued.isEmpty();
    m_queued.clear();
    Piper::instance()->stop();
    if (m_testEngine) {
        m_testQueue.clear();
        m_testCurrent = 0;
        m_testTimer->stop();
    }
#if defined(SCANCORE_HAVE_TTS)
    m_qtIds.clear();
    m_current = 0;
    if (m_tts)
        m_tts->stop(QTextToSpeech::BoundaryHint::Immediate);
#elif defined(Q_OS_ANDROID)
    QJniObject::callStaticMethod<void>(kSpeechClass, "stop", "()V");
#endif
    if (had)
        emit changed();
}

void Speech::configure(const QString &voice, double rate, double pitch) {
    rate = std::clamp(rate, 0.25, 4.0);
    pitch = std::clamp(pitch, 0.5, 2.0);
    // A neural voice keeps its own pitch; the speed is the player's.
    m_neural = voice.startsWith(QLatin1String("piper:")) ? voice.mid(6) : QString();
    if (!m_neural.isEmpty()) {
        Piper::instance()->setVoice(m_neural);
        Piper::instance()->setRate(rate);
        return;
    }
#if defined(SCANCORE_HAVE_TTS)
    if (!m_tts)
        return;
    // Qt's scale is -1..1 around the voice's own; one step is a doubling.
    m_tts->setRate(std::clamp(std::log2(rate), -1.0, 1.0));
    m_tts->setPitch(std::clamp(std::log2(pitch), -1.0, 1.0));
    if (!voice.isEmpty())
        for (const auto &v : m_tts->findVoices())
            if (v.name() + QLatin1Char('|') + v.locale().name() == voice) {
                m_tts->setVoice(v);
                break;
            }
#elif defined(Q_OS_ANDROID)
    QJniObject::callStaticMethod<void>(kSpeechClass, "configure", "(Ljava/lang/String;FF)V",
                                       QJniObject::fromString(voice).object<jstring>(), jfloat(rate), jfloat(pitch));
#else
    Q_UNUSED(voice)
#endif
}

} // namespace sc
