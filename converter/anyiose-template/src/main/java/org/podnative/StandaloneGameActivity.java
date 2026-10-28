package org.podnative;

import android.content.Context;
import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;
import java.io.File;
import java.io.InputStream;
import org.json.JSONObject;

/** A private SDL host for exactly the IPA embedded in this APK. */
public final class StandaloneGameActivity extends org.anyiose.android.MainActivity {
    private File data;
    private float cap = 60;
    private boolean classic43, offline, performance;
    private String orientation = "default";
    @Override protected void onCreate(Bundle state) {
        data = new File(getFilesDir(), "game-data");
        if (!data.isDirectory() && !data.mkdirs()) { super.onCreate(state); finish(); return; }
        cap = highestMode();
        readConvertSettings();
        super.onCreate(state);
        requestMode();
        if (performance) requestSustainedPerformance();
    }
    @Override protected String[] getArguments() {
        File source = new File(getFilesDir(), "game.ipa");
        if (data == null || !data.isDirectory() || !source.isFile())
            return StandaloneOptions.noGame();
        return StandaloneOptions.forGame(data.getAbsolutePath(), source.getAbsolutePath(), cap,
                classic43, offline, performance, orientation);
    }
    @Override protected boolean shouldShowStartupErrors() {
        return false;
    }
    private void readConvertSettings() {
        try (InputStream input = getAssets().open("anyiose-game.json")) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (bytes.size() + count > 8192) throw new IllegalStateException("Invalid game metadata");
                bytes.write(buffer, 0, count);
            }
            JSONObject settings = new JSONObject(bytes.toString("UTF-8")).optJSONObject("convertSettings");
            if (settings == null) return; // Old generated APKs retain their defaults.
            classic43 = settings.optBoolean("classic43", false);
            offline = settings.optBoolean("offline", false);
            performance = settings.optBoolean("performance", false);
            String value = settings.optString("orientation", "default");
            if ("portrait".equals(value) || "landscape".equals(value)) orientation = value;
        } catch (Exception ignored) {
            // Signed metadata is normally valid; safe defaults preserve existing behavior.
        }
    }
    private void requestSustainedPerformance() {
        // This is a request to Android, not clock control or a performance guarantee.
        // https://developer.android.com/games/optimize/adpf/sustained-performance
        try {
            android.os.PowerManager power = (android.os.PowerManager) getSystemService(Context.POWER_SERVICE);
            if (power != null && power.isSustainedPerformanceModeSupported())
                getWindow().setSustainedPerformanceMode(true);
        } catch (RuntimeException ignored) { }
    }
    private Display display() {
        if (android.os.Build.VERSION.SDK_INT >= 30 && getDisplay() != null) return getDisplay();
        WindowManager manager = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
        return manager == null ? null : manager.getDefaultDisplay();
    }
    private float highestMode() {
        Display display = display();
        if (display == null) return 60;
        Display.Mode current = display.getMode();
        float highest = display.getRefreshRate();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (current == null || (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()))
                highest = Math.max(highest, mode.getRefreshRate());
        }
        return highest > 0 ? highest : 60;
    }
    private void requestMode() {
        Display display = display();
        if (display == null) return;
        Display.Mode current = display.getMode(), best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (current != null && (mode.getPhysicalWidth() != current.getPhysicalWidth()
                    || mode.getPhysicalHeight() != current.getPhysicalHeight())) continue;
            if (best == null || mode.getRefreshRate() > best.getRefreshRate()) best = mode;
        }
        if (best != null) {
            WindowManager.LayoutParams params = getWindow().getAttributes();
            params.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(params);
        }
    }
}
