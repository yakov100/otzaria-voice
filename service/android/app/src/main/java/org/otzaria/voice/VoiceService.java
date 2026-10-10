package org.otzaria.voice;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the voice service running: a foreground service whose thread runs the
 * C++ HTTP server on 127.0.0.1:47811.
 *
 * Android lets an app record only through a foreground service of type
 * "microphone" that was started while the app was on screen (Android 11+), and
 * never starts one from boot (Android 14+). So at boot the server runs without
 * the microphone (type "specialUse") and answers dictation with an error that
 * asks the user to open this app; opening it once upgrades the service.
 */
public final class VoiceService extends Service {
    static final String EXTRA_FROM_APP = "fromApp";
    private static final String CHANNEL = "voice";
    private static final int NOTIFICATION_ID = 1;
    private static Thread server;
    private static volatile boolean microphoneAllowed;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean fromApp = intent != null && intent.getBooleanExtra(EXTRA_FROM_APP, false);
        boolean canRecord = checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
        // Before Android 11 a foreground service may record whenever it runs.
        boolean mic = canRecord && (fromApp || Build.VERSION.SDK_INT < 30 || microphoneAllowed);
        goForeground(mic);
        microphoneAllowed = mic;
        NativeBridge.setMicrophoneAllowed(mic);
        startServer();
        return START_STICKY;
    }

    private void goForeground(boolean mic) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(
                CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(getString(mic ? R.string.notification_ready : R.string.notification_no_mic))
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) {
            int type = 0;
            if (mic) type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            if (Build.VERSION.SDK_INT >= 34) type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
            if (type == 0) type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST;
            startForeground(NOTIFICATION_ID, n, type);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
    }

    private synchronized void startServer() {
        if (server != null && server.isAlive()) return;
        final String files = getFilesDir().getAbsolutePath();
        final String libs = getApplicationInfo().nativeLibraryDir;
        server = new Thread(() -> {
            copyBundledPack();
            NativeBridge.serve(files, libs);
        }, "otzaria-voice");
        server.start();
    }

    /**
     * The "with models" APK carries the voice pack in assets/pack. The model
     * runtime reads files, so on first run (and after an update of the pack)
     * it is copied to <files>/stt/pack, where the service looks for it.
     */
    private void copyBundledPack() {
        android.content.res.AssetManager assets = getAssets();
        java.io.File target = new java.io.File(getFilesDir(), "stt/pack");
        try {
            String[] top = assets.list("pack");
            if (top == null || top.length == 0) return;  // the APK without models
            // Copied again only when the app (and with it the pack) was updated.
            String stamp = String.valueOf(
                    getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
            if (stamp.equals(readFile(new java.io.File(target, ".bundled")))) return;
            java.io.File staging = new java.io.File(getFilesDir(), "stt/pack.new");
            deleteTree(staging);
            copyAssetTree(assets, "pack", staging);
            deleteTree(target);
            target.getParentFile().mkdirs();
            if (!staging.renameTo(target)) throw new java.io.IOException("rename failed");
            try (java.io.OutputStream o = new java.io.FileOutputStream(new java.io.File(target, ".bundled"))) {
                o.write(stamp.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
        } catch (java.io.IOException | android.content.pm.PackageManager.NameNotFoundException e) {
            android.util.Log.e("OtzariaVoice", "copying the bundled voice pack failed", e);
        }
    }

    private static String readFile(java.io.File f) {
        try (java.io.InputStream in = new java.io.FileInputStream(f)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[256];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return new String(out.toByteArray(), java.nio.charset.StandardCharsets.UTF_8).trim();
        } catch (java.io.IOException e) {
            return null;
        }
    }

    private static void copyAssetTree(android.content.res.AssetManager assets, String path,
                                      java.io.File to) throws java.io.IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            to.mkdirs();
            for (String c : children) copyAssetTree(assets, path + "/" + c, new java.io.File(to, c));
            return;
        }
        try (java.io.InputStream in = assets.open(path);
             java.io.OutputStream out = new java.io.FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static void deleteTree(java.io.File f) {
        java.io.File[] children = f.listFiles();
        if (children != null) for (java.io.File c : children) deleteTree(c);
        f.delete();
    }

    @Override
    public void onDestroy() {
        NativeBridge.quit();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
