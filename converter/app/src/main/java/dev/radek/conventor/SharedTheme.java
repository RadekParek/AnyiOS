package dev.radek.conventor;

import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.animation.StateListAnimator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.widget.Button;
import android.widget.Toast;

/** Shared appearance and navigation for the two modules and the AnyiOS picker. */
public final class SharedTheme {
    private static Typeface regular, semibold;
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
    private static int dp(Context context, int value) {
        return Math.round(context.getResources().getDisplayMetrics().density * value);
    }
    private static int mix(int a, int b, float amount) {
        float inverse = 1f - amount;
        return Color.rgb(Math.round(Color.red(a) * inverse + Color.red(b) * amount),
            Math.round(Color.green(a) * inverse + Color.green(b) * amount),
            Math.round(Color.blue(a) * inverse + Color.blue(b) * amount));
    }
    private static int alpha(int color, int alpha) {
        return (color & 0x00ffffff) | (alpha << 24);
    }

    /** Static translucent gradients and highlights; no per-frame backdrop blur. */
    public static Drawable canvas(Context context) {
        boolean light = light(context);
        int base = Color.parseColor(light ? "#F5F7FA" : "#0B101D");
        int middle = mix(base, accent(context), light ? 0.035f : 0.075f);
        int bottom = Color.parseColor(light ? "#EAF0F5" : "#111C2B");
        return new GradientDrawable(
            GradientDrawable.Orientation.TL_BR, new int[] {base, middle, bottom});
    }
    public static GradientDrawable glass(Context context, int radiusDp) {
        boolean light = light(context);
        int base = Color.parseColor(light ? "#FFFFFF" : "#182437");
        int top = mix(base, Color.WHITE, light ? 0.02f : 0.08f);
        int bottom = mix(base, accent(context), light ? 0.035f : 0.045f);
        GradientDrawable surface = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {alpha(top, light ? 242 : 235), alpha(base, light ? 232 : 226),
                alpha(bottom, light ? 226 : 222)});
        surface.setCornerRadius(dp(context, radiusDp));
        surface.setStroke(dp(context, 1), light ? 0x99B8CBD7 : 0x6686A8B5);
        return surface;
    }
    public static GradientDrawable artwork(Context context, int radiusDp) {
        boolean light = light(context);
        int base = Color.parseColor(light ? "#E8F1F0" : "#14283A");
        int tint = accent(context);
        GradientDrawable drawable = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {
                mix(base, tint, light ? 0.18f : 0.23f), mix(base, tint, light ? 0.07f : 0.10f)});
        drawable.setCornerRadius(dp(context, radiusDp));
        return drawable;
    }
    public static RippleDrawable glassRipple(Context context, int radiusDp) {
        GradientDrawable mask = new GradientDrawable();
        mask.setColor(Color.WHITE);
        mask.setCornerRadius(dp(context, radiusDp));
        return new RippleDrawable(ColorStateList.valueOf(light(context) ? 0x20007D70 : 0x36FFFFFF),
            glass(context, radiusDp), mask);
    }

    /** The bundled Manrope variable font falls back to the system font if unavailable. */
    public static synchronized Typeface typeface(Context context, boolean emphasis) {
        if (regular == null || semibold == null) {
            try {
                Typeface font = context.getResources().getFont(R.font.manrope_family);
                if (Build.VERSION.SDK_INT >= 28) {
                    regular = Typeface.create(font, 400, false);
                    semibold = Typeface.create(font, 700, false);
                } else {
                    regular = Typeface.create(font, Typeface.NORMAL);
                    semibold = Typeface.create(font, Typeface.BOLD);
                }
            } catch (RuntimeException e) {
                regular = Typeface.create("sans-serif", Typeface.NORMAL);
                semibold = Typeface.create("sans-serif-medium", Typeface.NORMAL);
            }
        }
        return emphasis ? semibold : regular;
    }

    /** Same responsive tile sizing in Emulator and Converter, including larger text. */
    public static int gridColumns(int widthDp, float fontScale) {
        if (widthDp < 300 || (widthDp < 700 && fontScale >= 2f))
            return 1;
        if (widthDp >= 700)
            return fontScale >= 1.35f ? 3 : 4;
        return widthDp < 360 || fontScale >= 1.35f ? 2 : 3;
    }
    public static int artworkSizeDp(int widthDp, int columns) {
        return Math.max(76, Math.min(180, (widthDp - 44 - (columns - 1) * 8) / columns - 18));
    }

    /** Animate page changes only, not every card or every progress refresh. */
    public static void reveal(View view) {
        if (!ValueAnimator.areAnimatorsEnabled())
            return;
        view.setAlpha(0f);
        view.setTranslationY(dp(view.getContext(), 8));
        view.animate()
            .alpha(1f)
            .translationY(0f)
            .setDuration(200)
            .setInterpolator(new DecelerateInterpolator())
            .start();
    }
    /** State feedback does not intercept touch, keyboard or accessibility clicks. */
    public static void pressFeedback(View view) {
        if (!ValueAnimator.areAnimatorsEnabled())
            return;
        StateListAnimator states = new StateListAnimator();
        AnimatorSet pressed = new AnimatorSet();
        pressed.playTogether(ObjectAnimator.ofFloat(view, View.SCALE_X, 0.98f),
            ObjectAnimator.ofFloat(view, View.SCALE_Y, 0.98f));
        pressed.setDuration(90);
        AnimatorSet released = new AnimatorSet();
        released.playTogether(ObjectAnimator.ofFloat(view, View.SCALE_X, 1f),
            ObjectAnimator.ofFloat(view, View.SCALE_Y, 1f));
        released.setDuration(130);
        states.addState(
            new int[] {android.R.attr.state_pressed, android.R.attr.state_enabled}, pressed);
        states.addState(new int[] {}, released);
        view.setStateListAnimator(states);
    }

    public static Intent modulesIntent(Context context) {
        return new Intent()
            .setClassName(context, "org.podnative.ModuleActivity")
            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
    }
    public static void openModules(Activity activity) {
        try {
            activity.startActivity(modulesIntent(activity));
            activity.finish();
        } catch (ActivityNotFoundException e) {
            Toast.makeText(activity, "The module picker is unavailable", Toast.LENGTH_LONG).show();
        }
    }
    /** Visible on every module page; the shared job executor is not stopped. */
    public static Button moduleButton(Activity activity) {
        Button button = new Button(activity);
        button.setId(R.id.module_picker_button);
        button.setText("All modules");
        button.setAllCaps(false);
        button.setTextSize(13);
        button.setTypeface(typeface(activity, true));
        button.setTextColor(light(activity) ? Color.rgb(22, 32, 46) : Color.WHITE);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(dp(activity, 48));
        button.setPadding(dp(activity, 14), dp(activity, 8), dp(activity, 14), dp(activity, 8));
        button.setBackground(glassRipple(activity, 16));
        button.setContentDescription("Back to Emulator and Converter selection");
        button.setOnClickListener(v -> openModules(activity));
        pressFeedback(button);
        return button;
    }
}
