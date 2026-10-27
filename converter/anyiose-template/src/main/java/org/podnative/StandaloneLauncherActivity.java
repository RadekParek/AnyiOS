package org.podnative;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.io.InputStream;
import org.json.JSONObject;

/** Prepares the private IPA and optional OBB mirror off the UI thread. */
public final class StandaloneLauncherActivity extends Activity {
    private static final String TAG = "AnyiOSEMU";
    private static final Object SETUP_LOCK = new Object();

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Keep extraction and the full integrity check off the main thread. The
        // transparent launcher has no preparation or error screen.
        new Thread(() -> {
            try {
                // A recreated launcher must not race another instance's pending file.
                synchronized (SETUP_LOCK) {
                    prepareGame();
                }
                runOnUiThread(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    startActivity(new Intent(this, StandaloneGameActivity.class));
                    finish();
                });
            } catch (Exception error) {
                Log.e(TAG, "Standalone game setup failed", error);
                runOnUiThread(() -> {
                    if (!isFinishing() && !isDestroyed()) finish();
                });
            }
        }, "AnyiOSEMU-game-setup").start();
    }

    private File prepareGame() throws Exception {
        JSONObject manifest;
        try (InputStream input = getAssets().open("anyiose-game.json")) {
            java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (output.size() + count > 8192) throw new IllegalStateException("Invalid game metadata");
                output.write(buffer, 0, count);
            }
            if (output.size() == 0) throw new IllegalStateException("Invalid game metadata");
            manifest = new JSONObject(output.toString("UTF-8"));
        }
        String expected = manifest.getString("sourceSha256");
        long expectedBytes = manifest.getLong("sourceBytes");
        if (!expected.matches("[0-9a-f]{64}") || expectedBytes <= 0)
            throw new IllegalStateException("Invalid game identity");
        File obbDirectory;
        try {
            obbDirectory = getObbDir();
        } catch (RuntimeException unavailable) {
            obbDirectory = null;
            Log.w(TAG, "App-specific expansion storage unavailable; using private storage", unavailable);
        }
        return StandaloneGameStorage.prepare(getFilesDir(), obbDirectory, getPackageName(),
            expectedBytes, expected, () -> getAssets().open("anyiose-game.ipa"));
    }
}
