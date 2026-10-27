package org.podnative;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import org.json.JSONObject;

/** Unpacks the one embedded IPA into this APK's own private storage, off the UI thread. */
public final class StandaloneLauncherActivity extends Activity {
    private TextView status;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        status = new TextView(this);
        status.setGravity(Gravity.CENTER);
        status.setTextColor(0xffffffff);
        status.setBackgroundColor(0xff10141d);
        status.setTextSize(17);
        status.setPadding(24, 24, 24, 24);
        status.setText("Preparing your game…");
        setContentView(status);
        new Thread(() -> {
            try {
                prepareGame();
                runOnUiThread(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    startActivity(new Intent(this, StandaloneGameActivity.class));
                    finish();
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (!isFinishing() && !isDestroyed())
                        status.setText("Could not prepare the game: " + error.getMessage() + "\n\nClose and try again.");
                });
            }
        }, "AnyiOSEMU-game-setup").start();
    }

    private void prepareGame() throws Exception {
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
        File game = new File(getFilesDir(), "game.ipa");
        if (game.isFile() && game.length() == expectedBytes && hash(game).equals(expected)) return;
        if (getFilesDir().getUsableSpace() < expectedBytes + 64L * 1024 * 1024)
            throw new IllegalStateException("Not enough free storage");
        File pending = new File(getFilesDir(), "game.ipa.pending");
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long total = 0;
            try (InputStream input = getAssets().open("anyiose-game.ipa");
                 FileOutputStream output = new FileOutputStream(pending)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > expectedBytes) throw new IllegalStateException("Game payload is too large");
                    digest.update(buffer, 0, count);
                    output.write(buffer, 0, count);
                }
                output.getFD().sync();
            }
            if (total != expectedBytes || !hex(digest.digest()).equals(expected))
                throw new IllegalStateException("Game payload failed its integrity check");
            if (!pending.renameTo(game)) throw new IllegalStateException("Cannot save the game");
        } finally { pending.delete(); }
    }

    private static String hash(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return hex(digest.digest());
    }
    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
}
