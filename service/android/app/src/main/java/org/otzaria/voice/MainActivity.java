package org.otzaria.voice;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The one screen: explains the service, asks for the microphone, and starts
 * the service while the app is on screen (the only moment Android allows a
 * service to take the microphone).
 */
public final class MainActivity extends Activity {
    private static final int REQUEST = 1;
    private TextView state;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(pad, pad, pad, pad);
        layout.setGravity(Gravity.TOP);
        TextView explain = new TextView(this);
        explain.setText(R.string.explain);
        explain.setTextSize(18);
        Button start = new Button(this);
        start.setText(R.string.start);
        start.setOnClickListener(v -> askAndStart());
        state = new TextView(this);
        state.setTextSize(18);
        state.setPadding(0, pad, 0, 0);
        layout.addView(explain);
        layout.addView(start);
        layout.addView(state);
        setContentView(layout);
        askAndStart();
    }

    private void askAndStart() {
        boolean needMic = checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED;
        boolean needNotify = Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED;
        if (needMic || needNotify) {
            requestPermissions(needNotify
                    ? new String[] {Manifest.permission.RECORD_AUDIO, Manifest.permission.POST_NOTIFICATIONS}
                    : new String[] {Manifest.permission.RECORD_AUDIO}, REQUEST);
            return;
        }
        startVoiceService();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == REQUEST) startVoiceService();
    }

    private void startVoiceService() {
        Intent i = new Intent(this, VoiceService.class).putExtra(VoiceService.EXTRA_FROM_APP, true);
        startForegroundService(i);
        boolean mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED;
        state.setText(mic ? R.string.running : R.string.no_permission);
    }
}
