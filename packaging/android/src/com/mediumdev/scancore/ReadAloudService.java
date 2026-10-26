package com.mediumdev.scancore;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// Music, and reading a novel aloud, with the screen off (src/app/AudioPlayer
// and ReadAloud through src/app/MediaControls.cpp): a media-playback
// foreground service keeps SCANCORE's process running, and its media session
// puts the song (title, artists, cover, time and a seek bar) or the chapter
// on the lock screen, in the media controls (Samsung's Now Bar, Android 13's
// player) and on headphones' buttons, as music apps do. The commands go back
// to the app as words ("play", "pause", "toggle", "next", "previous",
// "stop", "seek:<seconds>").
public final class ReadAloudService extends Service {
    private static final String CHANNEL = "read-aloud";
    private static final String MUSIC_CHANNEL = "music";
    private static final int ID = 4712;
    private static final String COMMAND = "com.mediumdev.scancore.READ_ALOUD";

    // What the notification shows; kept for a service that (re)starts.
    private static boolean playing, music, seekable;
    private static String title = "", artist = "", album = "", cover = "", channelName = "Reading aloud";
    private static long duration, position;
    // The cover, loaded off the main thread, for the URL it came from.
    private static Bitmap art;
    private static String artUrl = "";
    private static ReadAloudService running;
    private MediaSession session;

    static native void nativeCommand(String command);

    // From the app: show (start or update) or hide the controls.
    public static synchronized void show(Context context, boolean active, boolean isPlaying, String trackTitle, String trackArtist,
                                         String trackAlbum, String coverUrl, String channelLabel, long durationMs,
                                         long positionMs, boolean canSeek, boolean isMusic) {
        playing = isPlaying;
        title = trackTitle == null ? "" : trackTitle;
        artist = trackArtist == null ? "" : trackArtist;
        album = trackAlbum == null ? "" : trackAlbum;
        duration = Math.max(0, durationMs);
        position = Math.max(0, positionMs);
        seekable = canSeek;
        music = isMusic;
        if (channelLabel != null && !channelLabel.isEmpty()) channelName = channelLabel;
        String wanted = coverUrl == null ? "" : coverUrl;
        if (!wanted.equals(cover)) {
            cover = wanted;
            art = null;
            artUrl = "";
            if (!wanted.isEmpty()) loadArt(wanted);
        }
        Context app = context.getApplicationContext();
        if (!active) {
            if (running != null) running.finish();
            return;
        }
        if (running != null) {
            running.refresh();
            return;
        }
        // Started only while reading, which the reader began on screen.
        Intent intent = new Intent(app, ReadAloudService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(intent);
            else app.startService(intent);
        } catch (RuntimeException e) {
            // Android 12+ refuses a start from the background; the next
            // change made on screen starts it.
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        synchronized (ReadAloudService.class) {
            running = this;
        }
        session = new MediaSession(this, "SCANCORE");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { nativeCommand("play"); }
            @Override public void onPause() { nativeCommand("pause"); }
            @Override public void onSkipToNext() { nativeCommand("next"); }
            @Override public void onSkipToPrevious() { nativeCommand("previous"); }
            @Override public void onStop() { nativeCommand("stop"); }
            @Override public void onSeekTo(long ms) { nativeCommand("seek:" + (ms / 1000.0)); }
        });
        session.setActive(true);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String command = intent != null ? intent.getStringExtra(COMMAND) : null;
        if (command != null) nativeCommand(command);
        startInForeground();
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        synchronized (ReadAloudService.class) {
            if (running == this) running = null;
        }
        if (session != null) session.release();
        super.onDestroy();
    }

    private void finish() {
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void refresh() {
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        updateSession();
        if (manager != null) manager.notify(ID, build());
    }

    private void startInForeground() {
        updateSession();
        Notification n = build();
        if (Build.VERSION.SDK_INT >= 29) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        else startForeground(ID, n);
    }

    // A cover from the web or a file, at most 512 px across, then the
    // notification again with it.
    private static void loadArt(final String url) {
        new Thread(() -> {
            Bitmap bitmap = null;
            try {
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                byte[] data = read(url);
                if (data != null) {
                    bounds.inJustDecodeBounds = true;
                    BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inSampleSize = 1;
                    while (Math.max(bounds.outWidth, bounds.outHeight) / options.inSampleSize > 1024) options.inSampleSize *= 2;
                    bitmap = BitmapFactory.decodeByteArray(data, 0, data.length, options);
                    if (bitmap != null && Math.max(bitmap.getWidth(), bitmap.getHeight()) > 512) {
                        float scale = 512f / Math.max(bitmap.getWidth(), bitmap.getHeight());
                        bitmap = Bitmap.createScaledBitmap(bitmap, Math.round(bitmap.getWidth() * scale), Math.round(bitmap.getHeight() * scale), true);
                    }
                }
            } catch (Exception | OutOfMemoryError e) {
                bitmap = null;
            }
            final Bitmap loaded = bitmap;
            new Handler(Looper.getMainLooper()).post(() -> {
                synchronized (ReadAloudService.class) {
                    if (!url.equals(cover) || loaded == null) return;
                    art = loaded;
                    artUrl = url;
                    if (running != null) running.refresh();
                }
            });
        }, "scancore-cover").start();
    }

    private static byte[] read(String url) throws Exception {
        InputStream in;
        HttpURLConnection connection = null;
        if (url.startsWith("http")) {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(8000);
            connection.setReadTimeout(8000);
            connection.setRequestProperty("User-Agent", "SCANCORE");
            in = connection.getInputStream();
        } else if (url.startsWith("file:") || url.startsWith("/")) {
            in = new java.io.FileInputStream(url.startsWith("file:") ? android.net.Uri.parse(url).getPath() : url);
        } else {
            return null;
        }
        try {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                if (out.size() > 8 * 1024 * 1024) return null;
            }
            return out.toByteArray();
        } finally {
            in.close();
            if (connection != null) connection.disconnect();
        }
    }

    private void updateSession() {
        MediaMetadata.Builder metadata = new MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, album);
        if (duration > 0) metadata.putLong(MediaMetadata.METADATA_KEY_DURATION, duration);
        if (art != null && artUrl.equals(cover)) metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art);
        session.setMetadata(metadata.build());
        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
            | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_STOP;
        if (seekable) actions |= PlaybackState.ACTION_SEEK_TO;
        // With the position and speed the system moves the time on itself.
        session.setPlaybackState(new PlaybackState.Builder()
            .setActions(actions)
            .setState(playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                      duration > 0 ? position : PlaybackState.PLAYBACK_POSITION_UNKNOWN, playing ? 1f : 0f)
            .build());
    }

    private PendingIntent command(String word, int code) {
        Intent intent = new Intent(this, ReadAloudService.class).putExtra(COMMAND, word);
        return PendingIntent.getService(this, code, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification build() {
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        // Music and reading aloud each have their channel, named for it.
        String channelId = music ? MUSIC_CHANNEL : CHANNEL;
        if (Build.VERSION.SDK_INT >= 26 && manager != null && manager.getNotificationChannel(channelId) == null) {
            NotificationChannel channel = new NotificationChannel(channelId, channelName, NotificationManager.IMPORTANCE_LOW);
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        }
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent tap = null;
        if (open != null) {
            open.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
            tap = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        }
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, channelId) : new Notification.Builder(this);
        if (art != null && artUrl.equals(cover)) b.setLargeIcon(art);
        b.setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(title.isEmpty() ? artist : title)
            .setContentText(artist)
            .setOngoing(playing)
            .setShowWhen(false)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .addAction(new Notification.Action.Builder(android.R.drawable.ic_media_previous, "Back", command("previous", 1)).build())
            .addAction(playing
                ? new Notification.Action.Builder(android.R.drawable.ic_media_pause, "Pause", command("pause", 2)).build()
                : new Notification.Action.Builder(android.R.drawable.ic_media_play, "Play", command("play", 3)).build())
            .addAction(new Notification.Action.Builder(android.R.drawable.ic_media_next, "Next", command("next", 4)).build())
            .addAction(new Notification.Action.Builder(android.R.drawable.ic_menu_close_clear_cancel, "Stop", command("stop", 5)).build())
            .setStyle(new Notification.MediaStyle().setMediaSession(session.getSessionToken()).setShowActionsInCompactView(0, 1, 2));
        if (tap != null) b.setContentIntent(tap);
        b.setDeleteIntent(command("stop", 6));
        return b.build();
    }
}
