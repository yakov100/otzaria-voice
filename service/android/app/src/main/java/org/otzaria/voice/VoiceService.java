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
        server = new Thread(() -> NativeBridge.serve(files, libs), "otzaria-voice");
        server.start();
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
