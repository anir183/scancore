#include "ReadAloud.h"

#include "App.h"
#include "Library.h"
#include "MediaControls.h"
#include "Novels.h"
#include "Speech.h"
#include "core/Task.h"
#include "core/Text.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace sc {

namespace {

// Tags carry the generation, so answers about an old queue are told apart.
constexpr int kPerGeneration = 100000;
constexpr int kSilence = -7;

int tagFor(int generation, int sentence) { return (generation % 20000) * kPerGeneration + sentence; }
int generationOf(int tag) { return tag / kPerGeneration; }
int sentenceOf(int tag) { return tag % kPerGeneration; }

double settingNumber(const char *key, double fallback, double low, double high) {
    bool ok = false;
    const double v = App::instance()->settings().value(QLatin1String(key)).toDouble(&ok);
    return ok ? std::clamp(v, low, high) : fallback;
}

bool settingOn(const char *key, bool fallback) {
    const auto v = App::instance()->settings().value(QLatin1String(key));
    return v.isValid() && !v.isNull() ? v.toBool() : fallback;
}

QVariantMap voiceMap() {
    const auto v = App::instance()->settings().value(QStringLiteral("ttsVoices"));
    if (v.typeId() == QMetaType::QVariantMap)
        return v.toMap();
    return QJsonDocument::fromJson(v.toString().toUtf8()).object().toVariantMap();
}

} // namespace

ReadAloud *ReadAloud::instance() {
    static auto *reader = [] {
        auto *created = new ReadAloud;
        QJSEngine::setObjectOwnership(created, QJSEngine::CppOwnership);
        return created;
    }();
    return reader;
}

ReadAloud::ReadAloud(QObject *parent) : QObject(parent) {
    m_pause.setSingleShot(true);
    connect(&m_pause, &QTimer::timeout, this, &ReadAloud::feed);
    m_sleep.setSingleShot(true);
    connect(&m_sleep, &QTimer::timeout, this, [this] {
        pause();
        m_sleepMode = QStringLiteral("off");
        m_sleepEndsAt = 0;
        emit sleepChanged();
        emit slept();
    });

    // Its engine starts when something is read, not with the app.
    auto *speech = Speech::object();
    connect(speech, &Speech::started, this, &ReadAloud::began);
    connect(speech, &Speech::finished, this, &ReadAloud::spoken);
    connect(speech, &Speech::failed, this, [this](const QString &message) {
        if (!m_playing)
            return;
        setPlaying(false);
        emit failed(message);
    });

    // The lock screen, the notification, media keys.
    auto *controls = MediaControls::instance();
    connect(controls, &MediaControls::playRequested, this, &ReadAloud::play);
    connect(controls, &MediaControls::pauseRequested, this, &ReadAloud::pause);
    connect(controls, &MediaControls::toggleRequested, this, &ReadAloud::toggle);
    connect(controls, &MediaControls::stopRequested, this, &ReadAloud::stop);
    connect(controls, &MediaControls::nextRequested, this, [this] { skip(1); });
    connect(controls, &MediaControls::previousRequested, this, [this] { skip(-1); });
}

QList<QPair<int, int>> ReadAloud::sentences(const QString &text) { return sentenceSpans(text); }

int ReadAloud::block() const {
    return m_current >= 0 && m_current < m_sentences.size() ? m_sentences[m_current].block : -2;
}
int ReadAloud::sentenceStart() const {
    return m_current >= 0 && m_current < m_sentences.size() ? m_sentences[m_current].start : 0;
}
int ReadAloud::sentenceLength() const {
    return m_current >= 0 && m_current < m_sentences.size() ? m_sentences[m_current].length : 0;
}
double ReadAloud::progress() const {
    return m_sentences.isEmpty() ? 0 : double(std::clamp(m_current, 0, int(m_sentences.size()))) / m_sentences.size();
}

QString ReadAloud::language() const {
    const auto own = m_series.value(QStringLiteral("language")).toString();
    if (!own.isEmpty())
        return own;
    const auto source = m_series.value(QStringLiteral("source")).toString();
    for (const auto &s : Novels().sources()) {
        const auto map = s.toMap();
        if (map.value(QStringLiteral("key")).toString() == source)
            return map.value(QStringLiteral("language")).toString();
    }
    return {};
}

QString ReadAloud::voiceFor(const QString &language) const {
    const auto base = language.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0);
    const auto map = voiceMap();
    for (const auto &key : {language, base})
        if (!key.isEmpty() && !map.value(key).toString().isEmpty())
            return map.value(key).toString();
    return App::instance()->settings().value(QStringLiteral("ttsVoice")).toString();
}

void ReadAloud::setVoiceFor(const QString &language, const QString &voice) {
    auto map = voiceMap();
    if (voice.isEmpty())
        map.remove(language);
    else
        map.insert(language, voice);
    App::instance()->setSetting(QStringLiteral("ttsVoices"),
                                QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact)));
    if (m_playing) {
        pause();
        play();
    }
}

void ReadAloud::configureVoice() {
    Speech::instance()->configure(voiceFor(language()), settingNumber("ttsRate", 1, 0.25, 4), settingNumber("ttsPitch", 1, 0.5, 2));
}

int ReadAloud::pauseAfter(int sentence) const {
    if (sentence < 0 || sentence + 1 >= m_sentences.size())
        return 0;
    const bool newParagraph = m_sentences[sentence].block != m_sentences[sentence + 1].block;
    return newParagraph ? int(settingNumber("ttsParagraphPause", 350, 0, 5000)) : int(settingNumber("ttsSentencePause", 0, 0, 5000));
}

void ReadAloud::start(const QVariantMap &series, const QVariantList &chapters, int index, const QVariantList &blocks, int fromBlock,
                      const QString &title) {
    if (index < 0 || index >= chapters.size())
        return;
    Speech::instance()->stop();
    ++m_generation;
    m_pause.stop();
    const bool newBook = m_series.value(QStringLiteral("id")) != series.value(QStringLiteral("id")) || m_chapters != chapters;
    m_series = series;
    m_chapters = chapters;
    if (newBook)
        emit sessionChanged();
    m_playing = true;
    if (blocks.isEmpty()) {
        loadChapter(index, fromBlock);
    } else {
        m_index = index;
        setChapter({{QStringLiteral("title"), title}, {QStringLiteral("blocks"), blocks}}, fromBlock);
        configureVoice();
        feed();
    }
    emit stateChanged();
    updatePlatform();
}

void ReadAloud::setChapter(const QVariantMap &chapter, int fromBlock) {
    const auto unit = m_chapters.value(m_index).toMap();
    m_title = chapter.value(QStringLiteral("title")).toString();
    if (m_title.isEmpty())
        m_title = unit.value(QStringLiteral("title")).toString();
    m_blocks = chapter.value(QStringLiteral("blocks")).toList();
    m_sentences.clear();
    if (!m_title.isEmpty())
        m_sentences.append({-1, 0, int(m_title.size()), m_title});
    for (int i = 0; i < m_blocks.size(); ++i) {
        const auto b = m_blocks[i].toMap();
        const auto text = b.value(QStringLiteral("text")).toString();
        if (text.isEmpty() || b.value(QStringLiteral("kind")).toString() == QLatin1String("break"))
            continue;
        for (const auto &[start, length] : sentences(text))
            m_sentences.append({i, start, length, text.mid(start, length)});
    }
    m_current = 0;
    if (fromBlock >= 0)
        for (int i = 0; i < m_sentences.size(); ++i)
            if (m_sentences[i].block >= fromBlock) {
                m_current = i;
                break;
            }
    m_next = m_current;
    m_lastSave.invalidate();
    emit chapterChanged();
    emit positionChanged();
    updatePlatform();
}

void ReadAloud::loadChapter(int index, int fromBlock) {
    if (index < 0 || index >= m_chapters.size())
        return;
    m_index = index;
    m_sentences.clear();
    m_blocks.clear();
    m_title = m_chapters.value(index).toMap().value(QStringLiteral("title")).toString();
    emit chapterChanged();
    if (m_task)
        m_task->deleteLater();
    const auto url = m_chapters.value(index).toMap().value(QStringLiteral("url")).toString();
    m_task = Novels().chapter(url, m_series.value(QStringLiteral("id")).toString());
    m_task->setParent(this);
    const int generation = m_generation;
    connect(m_task, &Task::finished, this, [this, generation, fromBlock](const QVariant &result) {
        m_task->deleteLater();
        m_task = nullptr;
        emit stateChanged();
        if (generation != m_generation)
            return;
        setChapter(result.toMap(), fromBlock);
        if (m_sentences.isEmpty()) {
            // Nothing to read here (pictures only): on to the next.
            chapterDone();
            return;
        }
        configureVoice();
        feed();
    });
    connect(m_task, &Task::failed, this, [this](const QString &error) {
        m_task->deleteLater();
        m_task = nullptr;
        setPlaying(false);
        emit failed(error);
    });
    emit stateChanged();
}

// Hands the voice the next sentences: one at a time where the voice
// cannot pause by itself (desktop engines, with a timer between), a few
// ahead with silences between where it can (Android), so nothing waits on
// the app while the screen is off.
void ReadAloud::feed() {
    if (!m_playing || m_task || m_pause.isActive())
        return;
    auto *speech = Speech::instance();
    const int window = speech->queuesSilence() ? 3 : 1;
    while (m_next < m_sentences.size() && m_next - m_current < window) {
        if (m_next > m_current) {
            const int ms = pauseAfter(m_next - 1);
            if (ms > 0)
                speech->enqueueSilence(ms, kSilence);
        }
        speech->enqueue(m_sentences[m_next].text, tagFor(m_generation, m_next));
        ++m_next;
    }
}

void ReadAloud::began(int tag) {
    if (tag < 0 || generationOf(tag) != m_generation % 20000)
        return;
    const int sentence = sentenceOf(tag);
    if (sentence == m_current && m_lastSave.isValid())
        return;
    m_current = sentence;
    emit positionChanged();
    saveProgress(false);
}

void ReadAloud::spoken(int tag) {
    if (tag < 0 || generationOf(tag) != m_generation % 20000 || !m_playing)
        return;
    const int sentence = sentenceOf(tag);
    if (sentence + 1 >= m_sentences.size()) {
        chapterDone();
        return;
    }
    m_current = sentence + 1;
    emit positionChanged();
    if (Speech::instance()->queuesSilence()) {
        feed();
        return;
    }
    const int ms = pauseAfter(sentence);
    if (ms > 0)
        m_pause.start(ms);
    else
        feed();
}

void ReadAloud::chapterDone() {
    m_current = int(m_sentences.size());
    saveProgress(true);
    emit positionChanged();
    if (m_sleepMode == QLatin1String("chapter")) {
        setSleep(0);
        setPlaying(false);
        emit slept();
        return;
    }
    if (settingOn("ttsContinue", true) && m_index + 1 < m_chapters.size()) {
        ++m_generation;
        loadChapter(m_index + 1, -1);
        return;
    }
    setPlaying(false);
}

void ReadAloud::saveProgress(bool force) {
    if (m_series.isEmpty() || m_index < 0 || m_sentences.isEmpty())
        return;
    if (App::instance()->settings().value(QStringLiteral("incognito")).toBool())
        return;
    if (!force && m_lastSave.isValid() && m_lastSave.elapsed() < 5000)
        return;
    m_lastSave.start();
    const auto unit = m_chapters.value(m_index).toMap();
    const bool done = m_current >= m_sentences.size();
    const int blockAt = done ? int(m_blocks.size()) - 1 : std::max(0, block());
    const double percent = done ? 1.0 : progress();
    Library::instance()->recordProgress(m_series, unit,
                                        {{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("block"), blockAt},
                                         {QStringLiteral("percent"), percent}},
                                        percent, done || percent > 0.97);
}

void ReadAloud::setPlaying(bool playing) {
    if (m_playing == playing)
        return;
    m_playing = playing;
    emit stateChanged();
    updatePlatform();
}

void ReadAloud::play() {
    if (!active() || m_playing)
        return;
    if (m_current >= m_sentences.size() && !m_task) {
        // At the end of the chapter: from its start again.
        m_current = 0;
    }
    ++m_generation;
    m_next = m_current;
    m_playing = true;
    configureVoice();
    emit stateChanged();
    feed();
    updatePlatform();
}

void ReadAloud::pause() {
    if (!m_playing)
        return;
    ++m_generation;
    m_pause.stop();
    Speech::instance()->stop();
    m_next = m_current;
    saveProgress(true);
    setPlaying(false);
}

void ReadAloud::toggle() { m_playing ? pause() : play(); }

void ReadAloud::stop() {
    if (!active())
        return;
    pause();
    saveProgress(true);
    setSleep(0);
    if (m_task) {
        m_task->deleteLater();
        m_task = nullptr;
    }
    m_series.clear();
    m_chapters.clear();
    m_blocks.clear();
    m_sentences.clear();
    m_index = -1;
    m_title.clear();
    emit sessionChanged();
    emit chapterChanged();
    emit positionChanged();
    emit stateChanged();
    updatePlatform();
}

void ReadAloud::skip(int sentences) {
    if (m_sentences.isEmpty())
        return;
    const int target = m_current + sentences;
    if (target >= m_sentences.size()) {
        skipChapter(1);
        return;
    }
    const bool was = m_playing;
    if (was) {
        ++m_generation;
        m_pause.stop();
        Speech::instance()->stop();
    }
    m_current = std::max(0, target);
    m_next = m_current;
    emit positionChanged();
    if (was)
        feed();
}

void ReadAloud::seekBlock(int block) {
    for (int i = 0; i < m_sentences.size(); ++i)
        if (m_sentences[i].block >= block) {
            skip(i - m_current);
            return;
        }
}

void ReadAloud::skipChapter(int by) {
    const int target = m_index + by;
    if (target < 0 || target >= m_chapters.size())
        return;
    saveProgress(true);
    ++m_generation;
    m_pause.stop();
    Speech::instance()->stop();
    loadChapter(target, -1);
}

void ReadAloud::setSleep(int minutes) {
    m_sleep.stop();
    if (minutes > 0) {
        m_sleepMode = QStringLiteral("minutes");
        m_sleepEndsAt = QDateTime::currentMSecsSinceEpoch() + qint64(minutes) * 60000;
        m_sleep.start(minutes * 60000);
    } else if (minutes < 0) {
        m_sleepMode = QStringLiteral("chapter");
        m_sleepEndsAt = 0;
    } else {
        m_sleepMode = QStringLiteral("off");
        m_sleepEndsAt = 0;
    }
    emit sleepChanged();
}

void ReadAloud::updatePlatform() {
    const auto unit = m_chapters.value(m_index).toMap();
    MediaControls::instance()->show(active(), m_playing, m_series.value(QStringLiteral("title")).toString(),
                                    m_title.isEmpty() ? unit.value(QStringLiteral("title")).toString() : m_title,
                                    m_series.value(QStringLiteral("cover")).toString());
}

} // namespace sc
