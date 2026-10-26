#pragma once

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

class QTextToSpeech;

namespace sc {

// Reading aloud with the system's voices: Qt Speech on desktop (SAPI or
// WinRT, AVSpeechSynthesizer, speech-dispatcher), Android's own engine on
// phones (packaging/android/src/.../Speech.java); and on computers the
// neural voices downloaded for Piper (ids "piper:…", see Piper). The novel reader queues a
// chapter's paragraphs with tags and hears back as each starts and ends, so
// it can follow along; Android's engine keeps reading the queue even while
// the app is in the background.
class Speech : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool speaking READ speaking NOTIFY changed)
    // [{id, name, language}], best for the app's language first.
    Q_PROPERTY(QVariantList voices READ voices NOTIFY voicesChanged)

public:
    static Speech *instance();
    // The same without starting the engine (the system's text to speech
    // takes a moment to start): for connecting to it at start-up.
    static Speech *object();
    static Speech *create(QQmlEngine *, QJSEngine *) { return instance(); }
    ~Speech() override;

    bool available() const { return m_available || m_hasNeural; }
    bool speaking() const { return !m_queued.isEmpty(); }
    QVariantList voices() const { return m_voices; }

    // Adds text to the queue; started(tag) and finished(tag) follow.
    Q_INVOKABLE void enqueue(const QString &text, int tag);
    // A pause in the queue, where the engine can queue one (Android); see
    // queuesSilence. started/finished(tag) follow as for text.
    Q_INVOKABLE void enqueueSilence(int ms, int tag);
    Q_INVOKABLE bool queuesSilence() const;
    // Empties the queue and stops at once.
    Q_INVOKABLE void stop();
    // rate and pitch are multipliers (1 = the voice's own); voice is an id
    // from voices, or empty for the system's choice.
    Q_INVOKABLE void configure(const QString &voice, double rate, double pitch);

    // From the platform engine (any thread).
    void engineReady(bool ok, const QVariantList &voices);
    void engineStarted(int chunk);
    void engineFinished(int chunk);
    void engineFailed(int chunk, const QString &message);

signals:
    void changed();
    void voicesChanged();
    void started(int tag);
    void finished(int tag);
    void failed(const QString &message);

private:
    explicit Speech(QObject *parent = nullptr);
    void ensureEngine();
    void sendChunk(int chunk, const QString &text);

    bool m_available = false;
    // The system's voices, and whether neural ones are installed.
    QVariantList m_systemVoices;
    bool m_hasNeural = false;
    // The neural voice in use (its Piper key), else the system's engine.
    QString m_neural;
    void updateVoices();
    bool m_engineStarted = false;
    QVariantList m_voices;
    // Engine chunk id -> {tag, last chunk of the tag}.
    struct Chunk {
        int tag;
        bool last;
    };
    QHash<int, Chunk> m_queued;
    int m_nextChunk = 1;
    // Development hook SCANCORE_TTS_ENGINE=timer (or timer-silence, which
    // queues pauses as Android does): a silent voice that "speaks" 15 ms a
    // character, for tests of reading aloud. {chunk, ms}.
    bool m_testEngine = false;
    bool m_testSilence = false;
    QList<QPair<int, int>> m_testQueue;
    int m_testCurrent = 0;
    QTimer *m_testTimer = nullptr;
    void testNext();
#ifdef SCANCORE_HAVE_TTS
    QTextToSpeech *m_tts = nullptr;
    QHash<qsizetype, int> m_qtIds; // Qt's enqueue id -> chunk
    int m_current = 0;
#endif
};

} // namespace sc
