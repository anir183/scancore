// Windows' System Media Transport Controls for MediaControls: the media
// flyout by the volume, the lock screen and the keyboard's media keys show
// the song (title, artists, cover, time) and answer play, pause, next,
// previous and seeking, as music apps do there. A desktop app reaches them
// for its window through ISystemMediaTransportControlsInterop.

#include "MediaControls.h"

#include <QGuiApplication>
#include <QWindow>

#ifndef NOMINMAX
#define NOMINMAX // std::max, not windows.h's macro
#endif
#include <windows.h>
// Before C++/WinRT: its classic COM interop (the SMTC interop interface).
#include <unknwn.h>

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>

namespace sc {

namespace {

using namespace winrt::Windows::Media;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Storage::Streams::RandomAccessStreamReference;

struct Smtc {
    SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttons, seek;
};

winrt::hstring text(const QString &s) { return winrt::hstring(s.toStdWString()); }

TimeSpan span(double seconds) {
    return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>((std::max)(0.0, seconds)));
}

HWND mainWindow() {
    for (auto *window : QGuiApplication::topLevelWindows())
        if (window->isVisible() && !window->transientParent())
            return reinterpret_cast<HWND>(window->winId());
    return nullptr;
}

Smtc *open() {
    const HWND hwnd = mainWindow();
    if (!hwnd)
        return nullptr;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error &) {
        // Qt already set up the thread (RPC_E_CHANGED_MODE): fine.
    }
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        SystemMediaTransportControls controls{nullptr};
        winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(controls)));
        auto *smtc = new Smtc;
        smtc->controls = controls;
        controls.IsPlayEnabled(true);
        controls.IsPauseEnabled(true);
        controls.IsNextEnabled(true);
        controls.IsPreviousEnabled(true);
        controls.IsStopEnabled(true);
        smtc->buttons = controls.ButtonPressed([](const SystemMediaTransportControls &, const SystemMediaTransportControlsButtonPressedEventArgs &args) {
            QString word;
            switch (args.Button()) {
            case SystemMediaTransportControlsButton::Play: word = QStringLiteral("play"); break;
            case SystemMediaTransportControlsButton::Pause: word = QStringLiteral("pause"); break;
            case SystemMediaTransportControlsButton::Next: word = QStringLiteral("next"); break;
            case SystemMediaTransportControlsButton::Previous: word = QStringLiteral("previous"); break;
            case SystemMediaTransportControlsButton::Stop: word = QStringLiteral("stop"); break;
            default: return;
            }
            MediaControls::instance()->command(word);
        });
        smtc->seek = controls.PlaybackPositionChangeRequested(
            [](const SystemMediaTransportControls &, const PlaybackPositionChangeRequestedEventArgs &args) {
                const double seconds = std::chrono::duration<double>(args.RequestedPlaybackPosition()).count();
                MediaControls::instance()->command(QStringLiteral("seek:%1").arg(seconds));
            });
        return smtc;
    } catch (const winrt::hresult_error &) {
        return nullptr;
    }
}

} // namespace

void showSystemControls(void *&state, const NowPlaying &now, bool trackChanged, bool jumped) {
    auto *smtc = static_cast<Smtc *>(state);
    if (!smtc) {
        if (!now.active)
            return;
        smtc = open();
        if (!smtc)
            return;
        state = smtc;
    }
    try {
        auto controls = smtc->controls;
        if (!now.active) {
            controls.PlaybackStatus(MediaPlaybackStatus::Closed);
            controls.DisplayUpdater().ClearAll();
            controls.DisplayUpdater().Update();
            controls.IsEnabled(false);
            return;
        }
        controls.IsEnabled(true);
        controls.PlaybackStatus(now.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
        if (trackChanged) {
            auto display = controls.DisplayUpdater();
            display.ClearAll();
            display.Type(MediaPlaybackType::Music);
            auto music = display.MusicProperties();
            music.Title(text(now.title));
            music.Artist(text(now.artist));
            music.AlbumTitle(text(now.album));
            if (now.cover.startsWith(QLatin1String("http")))
                display.Thumbnail(RandomAccessStreamReference::CreateFromUri(Uri(text(now.cover))));
            display.Update();
        }
        if (trackChanged || jumped || now.duration > 0) {
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(span(0));
            timeline.MinSeekTime(span(0));
            timeline.EndTime(span(now.duration));
            timeline.MaxSeekTime(span(now.seekable ? now.duration : 0));
            timeline.Position(span(now.position));
            controls.UpdateTimelineProperties(timeline);
        }
    } catch (const winrt::hresult_error &) {
    }
}

} // namespace sc
