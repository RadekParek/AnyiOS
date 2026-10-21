package dev.radek.conventor;

import android.content.Context;
import android.graphics.Color;

/** Converter appearance preferences are the common AnyiOS design tokens. */
public final class SharedTheme {
    private SharedTheme() {}
    public static boolean light(Context context) {
        return context.getSharedPreferences("radek_settings", Context.MODE_PRIVATE)
            .getBoolean("light_theme", false);
    }
    public static int accent(Context context) {
        boolean light = light(context);
        String scheme = context.getSharedPreferences("radek_settings", Context.MODE_PRIVATE)
                            .getString("accent_scheme", "teal");
        String value;
        switch (scheme == null ? "teal" : scheme) {
            case "blue":
                value = light ? "#1D4ED8" : "#7DB4FF";
                break;
            case "amber":
                value = light ? "#B45309" : "#FBBF24";
                break;
            case "violet":
                value = light ? "#6D28D9" : "#C4A7FF";
                break;
            default:
                value = light ? "#007D70" : "#5CE3B5";
                break;
        }
        return Color.parseColor(value);
    }
}
