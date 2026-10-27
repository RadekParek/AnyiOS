package org.podnative;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** Best-effort engine identification from bounded, engine-specific bundle evidence. */
final class GameEngineDetector {
    private static final int MAX_BINARY_BYTES = 32 * 1024 * 1024;
    private final Set<String> engines = new LinkedHashSet<>();

    // Unity's iOS project contains UnityFramework and its compiled Data folder:
    // https://docs.unity3d.com/6000.2/Documentation/Manual/StructureOfXcodeProject.html
    // Godot documents the godot_path Info.plist key for iOS development builds:
    // https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_ios.html
    void observeMetadata(Map<String, Object> info) {
        if (info.get("godot_path") instanceof String
            && !((String) info.get("godot_path")).trim().isEmpty())
            engines.add("Godot");
    }

    void observePath(String relativePath) {
        String path = relativePath.toLowerCase(Locale.ROOT);
        if (path.startsWith("plugins/") || path.contains("/plugins/") || path.contains("/../")
            || path.startsWith("../"))
            return;
        if (path.equals("frameworks/unityframework.framework/unityframework")
            || path.equals("data/managed/unityengine.dll")
            || path.equals("data/globalgamemanagers"))
            engines.add("Unity");
        // UE3 iOS packages include CookedIPhone/Engine.xxx; generic .pak or
        // "engine" filenames alone are not reliable evidence.
        int slash = path.indexOf('/');
        if ((path.equals("cookediphone/engine.xxx")
                || (slash > 0 && path.substring(slash + 1).equals("cookediphone/engine.xxx")))
            && !path.startsWith("frameworks/"))
            engines.add("Unreal Engine");
    }

    /**
     * Symbols are deliberately paired, and scanned only in the declared main
     * executable. Embedded libraries can still produce false positives, so the
     * UI describes these results as best effort. No full binary is retained.
     */
    void observeExecutable(InputStream input) throws IOException {
        Match[] symbols = {new Match("UnityAppController"), new Match("UnityInitRuntime"),
            new Match("CCDirector"), new Match("CCSprite")};
        byte[] buffer = new byte[16384];
        int remaining = MAX_BINARY_BYTES;
        while (remaining > 0) {
            int read = input.read(buffer, 0, Math.min(buffer.length, remaining));
            if (read < 0)
                break;
            if (read == 0)
                continue;
            remaining -= read;
            for (int i = 0; i < read; i++)
                for (Match symbol : symbols) symbol.accept(buffer[i]);
            if (symbols[0].found && symbols[1].found)
                engines.add("Unity");
            if (symbols[2].found && symbols[3].found)
                engines.add("Cocos2d");
            if (symbols[0].found && symbols[1].found && symbols[2].found && symbols[3].found)
                break;
        }
    }

    String result() {
        if (engines.isEmpty())
            return "Unknown";
        java.util.List<String> sorted = new java.util.ArrayList<>(engines);
        java.util.Collections.sort(sorted);
        return String.join(", ", sorted) + " (detected)";
    }

    /** Streaming KMP keeps signatures intact across read-buffer boundaries. */
    private static final class Match {
        final byte[] pattern;
        final int[] prefix;
        int matched;
        boolean found;
        Match(String text) {
            pattern = text.getBytes(StandardCharsets.US_ASCII);
            prefix = new int[pattern.length];
            for (int i = 1, j = 0; i < pattern.length; i++) {
                while (j > 0 && pattern[i] != pattern[j]) j = prefix[j - 1];
                if (pattern[i] == pattern[j])
                    j++;
                prefix[i] = j;
            }
        }
        void accept(byte value) {
            if (found)
                return;
            while (matched > 0 && value != pattern[matched]) matched = prefix[matched - 1];
            if (value == pattern[matched])
                matched++;
            if (matched == pattern.length)
                found = true;
        }
    }
}
