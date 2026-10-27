package org.podnative;

import android.content.Context;
import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;
import java.io.File;
import java.util.Locale;

/** A private SDL host for exactly the IPA embedded in this APK. */
public final class StandaloneGameActivity extends org.anyiose.android.MainActivity {
    private File data;
    private float cap = 60;
    @Override protected void onCreate(Bundle state) {
        data = new File(getFilesDir(), "game-data");
        if (!data.isDirectory() && !data.mkdirs()) { super.onCreate(state); finish(); return; }
        cap = highestMode();
        super.onCreate(state);
        requestMode();
    }
    @Override protected String[] getArguments() {
        File source = new File(getFilesDir(), "game.ipa");
        if (data == null || !data.isDirectory() || !source.isFile()) return new String[] {"--help"};
        return new String[] {"--anyiose-data-dir=" + data.getAbsolutePath(), source.getAbsolutePath(),
            "--device-family=auto", "--fps-limit=" + String.format(Locale.ROOT, "%.3f", cap),
            "--no-gles-native", "--no-force-composition"};
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
