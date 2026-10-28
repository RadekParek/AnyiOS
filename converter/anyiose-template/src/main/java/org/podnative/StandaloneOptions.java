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
            boolean classic43, boolean offline, boolean performance, String orientation) {
        ArrayList<String> args = new ArrayList<>();
        Collections.addAll(args, "--anyiose-data-dir=" + dataPath, sourcePath,
            offline ? "--no-network-access" : "--allow-network-access", "--device-family=auto",
            "--fps-limit=" + String.format(Locale.ROOT, "%.3f", cap), "--boost-display-link",
            "--no-error-popup", "--anyiose-silent-fatal", "--no-gles-native", "--no-force-composition");
        if (classic43) Collections.addAll(args, "--screen-size=768x1024", "--scale-hack=2");
        if (performance) Collections.addAll(args, "--perf-hints", "--affinity=big");
        if ("portrait".equals(orientation)) args.add("--anyiose-initial-portrait");
        if ("landscape".equals(orientation)) args.add("--landscape-left");
        return args.toArray(new String[0]);
    }

    static String[] noGame() {
        return new String[] {"--no-error-popup", "--anyiose-silent-fatal", "--help"};
    }
}
