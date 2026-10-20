package org.podnative;

import java.io.IOException;

final class LibraryRules {
    private LibraryRules() {}
    static String fileName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
            || name.length() > 255 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
            || name.indexOf('\0') >= 0)
            throw new IOException("Unsafe or unsupported file name in the selected folder");
        return name;
    }
    static String title(String name) {
        if (name == null || name.isEmpty())
            return "Untitled game";
        String result = name.replaceAll("(?i)\\.(ipa|app|bin|macho)$", "").replace('_', ' ').trim();
        return result.isEmpty() ? "Untitled game"
                                : result.substring(0, Math.min(120, result.length()));
    }
    static String metadataText(Object value, String fallback) {
        if (!(value instanceof String) || ((String) value).trim().isEmpty())
            return fallback;
        String text = ((String) value).replaceAll("[\\p{Cntrl}]", " ").trim();
        return text.isEmpty() ? fallback : text.substring(0, Math.min(240, text.length()));
    }
}
