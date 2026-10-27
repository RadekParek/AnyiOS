package org.podnative;

import java.util.Locale;

/** The isolated APK always launches one game with maximum-FPS mode requested. */
final class StandaloneOptions {
    private StandaloneOptions() {}

    static String[] forGame(String dataPath, String sourcePath, float cap) {
        return new String[] {"--anyiose-data-dir=" + dataPath, sourcePath,
            "--allow-network-access", "--device-family=auto", "--fps-limit=" + String.format(Locale.ROOT, "%.3f", cap),
            "--boost-display-link", "--no-error-popup", "--anyiose-silent-fatal",
            "--no-gles-native", "--no-force-composition"};
    }

    static String[] noGame() {
        return new String[] {"--no-error-popup", "--anyiose-silent-fatal", "--help"};
    }
}
