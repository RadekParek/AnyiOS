package org.podnative;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Locale;

/** The isolated APK launches one game with maximum-FPS mode requested. */
final class StandaloneOptions {
    private StandaloneOptions() {}

    static String[] forGame(String dataPath, String sourcePath, float cap) {
        return forGame(dataPath, sourcePath, cap, false, false, false, "default");
    }

    static String[] forGame(String dataPath, String sourcePath, float cap,
            boolean classic320, boolean offline, boolean performance, String orientation) {
        ArrayList<String> args = new ArrayList<>();
        Collections.addAll(args, "--anyiose-data-dir=" + dataPath, sourcePath,
            offline ? "--no-network-access" : "--allow-network-access", "--device-family=auto",
            "--fps-limit=" + String.format(Locale.ROOT, "%.3f", cap), "--boost-display-link",
            "--no-error-popup", "--anyiose-silent-fatal", "--no-gles-native", "--no-force-composition");
        // AnyiOSEMU normalizes this to 320x480 for portrait; landscape is
        // 480x320 points. Request 4x internal rendering in either orientation.
        if (classic320) Collections.addAll(args, "--screen-size=480x320", "--scale-hack=4");
        if (performance) Collections.addAll(args, "--perf-hints", "--affinity=big");
        if ("portrait".equals(orientation)) args.add("--anyiose-initial-portrait");
        if ("-90".equals(orientation) || "landscape".equals(orientation)) args.add("--landscape-left");
        if ("90".equals(orientation)) args.add("--landscape-right");
        if ("180".equals(orientation)) args.add("--upside-down");
        return args.toArray(new String[0]);
    }

    static String[] noGame() {
        return new String[] {"--no-error-popup", "--anyiose-silent-fatal", "--help"};
    }
}
