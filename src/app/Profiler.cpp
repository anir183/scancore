#include "Profiler.h"

#include <QQuickWindow>

#ifdef Q_OS_ANDROID
#include <sys/system_properties.h>
#endif

namespace sc {

namespace {

constexpr int kWatchMs = 20;

} // namespace

bool Profiler::enabled() {
    static const bool on = [] {
#ifdef Q_OS_ANDROID
        // Apps get no environment there: `adb shell setprop
        // debug.scancore.profile 1` before starting it (debug builds log).
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.scancore.profile", value) > 0 && value[0] == '1')
            return true;
#endif
        return qEnvironmentVariableIsSet("SCANCORE_PROFILE");
    }();
    return on;
}

Profiler *Profiler::instance() {
    static auto *profiler = new Profiler;
    return profiler;
}

Profiler::Profiler(QObject *parent) : QObject(parent) {
    m_placeClock.start();
    // A timer that should fire every 20 ms: when it comes late, the GUI
    // thread was busy (layout, JavaScript, a blocking call) for that long.
    m_watchdog.setInterval(kWatchMs);
    m_watchdog.setTimerType(Qt::PreciseTimer);
    connect(&m_watchdog, &QTimer::timeout, this, &Profiler::tick);
    m_summary.setInterval(10000);
    connect(&m_summary, &QTimer::timeout, this, &Profiler::summary);
}

void Profiler::attach(QQuickWindow *window) {
    if (!enabled() || !window)
        return;
    connect(window, &QQuickWindow::beforeSynchronizing, this, &Profiler::syncing, Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, &Profiler::frame, Qt::DirectConnection);
    m_watchClock.start();
    m_watchdog.start();
    m_summary.start();
}

void Profiler::setPlace(const QString &place) {
    if (!enabled())
        return;
    QMutexLocker lock(&m_mutex);
    if (place == m_place)
        return;
    m_place = place;
    m_placeClock.restart();
    m_waitingFirstFrame = true;
}

// Frames come from the render thread (threaded render loop) or the GUI
// thread; only counters and the log are touched here.
void Profiler::syncing() {
    QMutexLocker lock(&m_mutex);
    m_renderClock.start();
}

void Profiler::startFrameTimes() {
    QMutexLocker lock(&m_mutex);
    m_intervals.clear();
    m_renders.clear();
    m_swapClock.invalidate();
    m_recording = true;
}

QVariantMap Profiler::stopFrameTimes() {
    QMutexLocker lock(&m_mutex);
    m_recording = false;
    QVariantList intervals, renders;
    for (double v : std::as_const(m_intervals))
        intervals.append(v);
    for (double v : std::as_const(m_renders))
        renders.append(v);
    return {{QStringLiteral("intervals"), intervals}, {QStringLiteral("renders"), renders}};
}

void Profiler::frame() {
    QMutexLocker lock(&m_mutex);
    const qint64 now = m_placeClock.elapsed();
    if (m_waitingFirstFrame) {
        m_waitingFirstFrame = false;
        qInfo("profile: %-34s first frame after %4lld ms", qPrintable(m_place), now);
    }
    ++m_frames;
    const qint64 render = m_renderClock.isValid() ? m_renderClock.elapsed() : 0;
    if (m_recording) {
        // The first frame's interval would be the idle time before it.
        if (m_swapClock.isValid())
            m_intervals.append(m_swapClock.nsecsElapsed() / 1e6);
        m_swapClock.start();
        if (m_renderClock.isValid())
            m_renders.append(m_renderClock.nsecsElapsed() / 1e6);
    }
    bool slow = false;
    if (render > 24) {
        slow = true;
        qInfo("profile: %-34s render %4lld ms (%lld ms after opening)", qPrintable(m_place), render, now);
    }
    if (slow)
        m_worst = std::max(m_worst, render);
    if (slow)
        ++m_slow;
}

void Profiler::tick() {
    const qint64 late = m_watchClock.restart() - kWatchMs;
    QMutexLocker lock(&m_mutex);
    // Two frames' worth: an animation running then would stutter.
    if (late > 33) {
        ++m_stalls;
        m_worstStall = std::max(m_worstStall, late);
        qInfo("profile: %-34s GUI thread busy %4lld ms (%lld ms after opening)", qPrintable(m_place), late,
              m_placeClock.elapsed());
    }
}

void Profiler::summary() {
    QMutexLocker lock(&m_mutex);
    if (m_frames > 0 || m_stalls > 0)
        qInfo("profile: %-34s %d frames, %d slow to render (worst %lld ms), GUI thread busy %d times (worst %lld ms)",
              qPrintable(m_place), m_frames, m_slow, m_worst, m_stalls, m_worstStall);
    m_stalls = 0;
    m_worstStall = 0;
    m_frames = 0;
    m_slow = 0;
    m_worst = 0;
}

} // namespace sc
