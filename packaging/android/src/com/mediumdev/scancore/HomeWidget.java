package com.mediumdev.scancore;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.RemoteViews;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;

// The home-screen widget: Continue reading and watching, and how many new
// chapters wait. The app writes what it shows to files/widget/widget.json
// (src/app/HomeWidget.cpp), with the texts in the reader's language, and
// calls refresh(); Android never wakes the app for it. A row opens
// scancore://continue/<series>; the rest opens SCANCORE.
public final class HomeWidget extends AppWidgetProvider {
    private static final int ROWS = 3;
    private static final int[] ROW = {R.id.row0, R.id.row1, R.id.row2};
    private static final int[] COVER = {R.id.cover0, R.id.cover1, R.id.cover2};
    private static final int[] TITLE = {R.id.title0, R.id.title1, R.id.title2};
    private static final int[] DETAIL = {R.id.detail0, R.id.detail1, R.id.detail2};

    // From the app, once it has written the widget's data.
    public static void refresh(Context context) {
        Context app = context.getApplicationContext();
        AppWidgetManager manager = AppWidgetManager.getInstance(app);
        int[] ids = manager.getAppWidgetIds(new ComponentName(app, HomeWidget.class));
        if (ids.length > 0) draw(app, manager, ids);
    }

    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] ids) {
        draw(context, manager, ids);
    }

    // Resized: as many rows as now fit.
    @Override
    public void onAppWidgetOptionsChanged(Context context, AppWidgetManager manager, int id, Bundle options) {
        draw(context, manager, new int[] {id});
    }

    // Rows the widget's height has room for: its padding and header take
    // about 48 dp, a row (40 dp cover, padding, gap) 53 dp. Launchers give
    // the portrait height as the largest; unknown sizes show all three.
    static int rowsThatFit(AppWidgetManager manager, int id) {
        Bundle options = manager.getAppWidgetOptions(id);
        int height = options != null ? options.getInt(AppWidgetManager.OPTION_APPWIDGET_MAX_HEIGHT, 0) : 0;
        if (height <= 0) return ROWS;
        return Math.max(1, Math.min(ROWS, (height - 48) / 53));
    }

    private static JSONObject data(Context context) {
        File file = new File(context.getFilesDir(), "widget/widget.json");
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int) Math.min(file.length(), 256 * 1024)];
            int read = 0;
            while (read < bytes.length) {
                int n = in.read(bytes, read, bytes.length - read);
                if (n < 0) break;
                read += n;
            }
            return new JSONObject(new String(bytes, 0, read, StandardCharsets.UTF_8));
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    private static PendingIntent open(Context context, Uri uri, int code) {
        Intent intent;
        if (uri != null) {
            intent = new Intent(Intent.ACTION_VIEW, uri);
            intent.setClassName(context, "org.qtproject.qt.android.bindings.QtActivity");
        } else {
            intent = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
            if (intent == null) intent = new Intent();
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(context, code, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static void draw(Context context, AppWidgetManager manager, int[] ids) {
        JSONObject data = data(context);
        for (int id : ids) manager.updateAppWidget(id, views(context, data, rowsThatFit(manager, id)));
    }

    private static RemoteViews views(Context context, JSONObject data, int fit) {
        RemoteViews views = new RemoteViews(context.getPackageName(), R.layout.scancore_widget);
        views.setTextViewText(R.id.heading, data.optString("title", "SCANCORE"));
        views.setOnClickPendingIntent(R.id.widget, open(context, null, 100));
        views.setOnClickPendingIntent(R.id.header, open(context, null, 101));
        String updates = data.optString("updates", "");
        views.setViewVisibility(R.id.updates, updates.isEmpty() ? View.GONE : View.VISIBLE);
        views.setTextViewText(R.id.updates, updates);
        JSONArray rows = data.optJSONArray("rows");
        int shown = 0;
        for (int i = 0; i < ROWS; ++i) {
            JSONObject row = rows != null && i < fit ? rows.optJSONObject(i) : null;
            if (row == null) {
                views.setViewVisibility(ROW[i], View.GONE);
                continue;
            }
            ++shown;
            views.setViewVisibility(ROW[i], View.VISIBLE);
            views.setTextViewText(TITLE[i], row.optString("title"));
            views.setTextViewText(DETAIL[i], row.optString("detail"));
            Bitmap cover = null;
            String path = row.optString("cover", "");
            if (!path.isEmpty()) {
                try {
                    cover = BitmapFactory.decodeFile(path);
                } catch (OutOfMemoryError e) {
                    cover = null;
                }
            }
            if (cover != null) views.setImageViewBitmap(COVER[i], cover);
            else views.setImageViewResource(COVER[i], R.drawable.widget_cover);
            views.setOnClickPendingIntent(ROW[i], open(context, Uri.parse("scancore://continue/" + Uri.encode(row.optString("id"))), 200 + i));
        }
        views.setViewVisibility(R.id.empty, shown == 0 ? View.VISIBLE : View.GONE);
        views.setTextViewText(R.id.empty, data.optString("empty", ""));
        return views;
    }
}
