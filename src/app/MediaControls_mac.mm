// macOS' Now Playing for MediaControls: the menu bar's Now Playing, the
// Touch Bar, AirPods and the keyboard's media keys show the song (title,
// artists, cover, time) and answer play, pause, next, previous and seeking,
// as music apps do there. Built with ARC (src/CMakeLists.txt).

#include "MediaControls.h"

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

namespace sc {

namespace {

NSString *ns(const QString &s) { return s.toNSString(); }

void command(const QString &word) { MediaControls::instance()->command(word); }

void listen() {
    MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
    [center.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("play"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("pause"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("toggle"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("next"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("previous"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *) {
        command(QStringLiteral("stop"));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [center.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        auto *change = static_cast<MPChangePlaybackPositionCommandEvent *>(event);
        command(QStringLiteral("seek:%1").arg(change.positionTime));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
}

// The cover, fetched off the main thread, then put in the info.
void loadArtwork(const QString &cover) {
    NSURL *url = [NSURL URLWithString:ns(cover)];
    if (!url)
        return;
    NSString *wanted = ns(cover);
    [[[NSURLSession sharedSession] dataTaskWithURL:url
                                 completionHandler:^(NSData *data, NSURLResponse *, NSError *error) {
                                     if (error || !data)
                                         return;
                                     NSImage *image = [[NSImage alloc] initWithData:data];
                                     if (!image)
                                         return;
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         // Another song by now: its own cover comes.
                                         if (![MediaControls::instance()->current().cover.toNSString() isEqualToString:wanted])
                                             return;
                                         MPNowPlayingInfoCenter *center = [MPNowPlayingInfoCenter defaultCenter];
                                         NSMutableDictionary *info = [center.nowPlayingInfo mutableCopy] ?: [NSMutableDictionary dictionary];
                                         info[MPMediaItemPropertyArtwork] =
                                             [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                                             requestHandler:^NSImage *(CGSize) { return image; }];
                                         center.nowPlayingInfo = info;
                                     });
                                 }] resume];
}

} // namespace

void showSystemControls(void *&state, const NowPlaying &now, bool trackChanged, bool jumped) {
    Q_UNUSED(jumped)
    if (!state) {
        if (!now.active)
            return;
        listen();
        state = reinterpret_cast<void *>(1);
    }
    MPNowPlayingInfoCenter *center = [MPNowPlayingInfoCenter defaultCenter];
    MPRemoteCommandCenter *commands = [MPRemoteCommandCenter sharedCommandCenter];
    if (!now.active) {
        center.nowPlayingInfo = nil;
        center.playbackState = MPNowPlayingPlaybackStateStopped;
        return;
    }
    commands.changePlaybackPositionCommand.enabled = now.seekable;
    NSMutableDictionary *info = trackChanged ? [NSMutableDictionary dictionary]
                                             : ([center.nowPlayingInfo mutableCopy] ?: [NSMutableDictionary dictionary]);
    info[MPMediaItemPropertyTitle] = ns(now.title);
    info[MPMediaItemPropertyArtist] = ns(now.artist);
    info[MPMediaItemPropertyAlbumTitle] = ns(now.album);
    info[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);
    if (now.duration > 0)
        info[MPMediaItemPropertyPlaybackDuration] = @(now.duration);
    // With the elapsed time and rate the system moves the time on itself.
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(now.position);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(now.playing ? 1.0 : 0.0);
    center.nowPlayingInfo = info;
    center.playbackState = now.playing ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
    if (trackChanged && now.cover.startsWith(QLatin1String("http")))
        loadArtwork(now.cover);
}

} // namespace sc
