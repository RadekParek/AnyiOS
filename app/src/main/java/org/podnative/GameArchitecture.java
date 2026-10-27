package org.podnative;

import java.util.Locale;
import java.util.regex.Pattern;

/** Render supported ARM32 slice labels without mistaking negative notes for slices. */
final class GameArchitecture {
    private static final Pattern NEGATIVE = Pattern.compile("\\bNO\\s+ARM64(?:_32)?\\b");
    private static final Pattern ARM64 = Pattern.compile("\\bARM64\\b");
    private static final Pattern ARM64_32 = Pattern.compile("\\bARM64_32\\b");
    private GameArchitecture() {}

    static String chip(String architecture) {
        String name = architecture.toUpperCase(Locale.ROOT);
        int annotation = name.indexOf('(');
        if (annotation >= 0)
            name = name.substring(0, annotation);
        // Historical labels can put the negative note outside parentheses.
        name = NEGATIVE.matcher(name).replaceAll("");
        String prefix = ARM64.matcher(name).find() ? "ARM64 + " : "";
        if (ARM64_32.matcher(name).find())
            prefix += "ARM64_32 + ";
        return prefix + "ARM32";
    }
}
