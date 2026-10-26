package org.podnative;

import android.app.Activity;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** The umbrella launcher; neither module receives a second source-file store. */
public final class ModuleActivity extends Activity {
    private int background, panel, ink, muted, accent;
    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
    @Override
    public void onCreate(Bundle state) {
        setTheme(R.style.AppThemeDark);
        super.onCreate(state);
    }
    @Override
    protected void onResume() {
        super.onResume();
        boolean light =
            getSharedPreferences("radek_settings", MODE_PRIVATE).getBoolean("light_theme", false);
        background = Color.parseColor(light ? "#F5F7FA" : "#0B101D");
        panel = Color.parseColor(light ? "#FFFFFF" : "#141D2F");
        ink = Color.parseColor(light ? "#16202E" : "#FFFFFF");
        muted = Color.parseColor(light ? "#5C6877" : "#A0B2C7");
        accent = dev.radek.conventor.SharedTheme.accent(this);
        getWindow().setStatusBarColor(background);
        getWindow().setNavigationBarColor(background);
        getWindow().getDecorView().setSystemUiVisibility(light
                ? View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
                : 0);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(background);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(24), dp(38), dp(24), dp(32));
        scroll.addView(body);
        ImageView mark = new ImageView(this);
        mark.setImageResource(R.drawable.ic_brand);
        mark.setContentDescription("AnyiOS pocket player");
        body.addView(mark, new LinearLayout.LayoutParams(dp(64), dp(64)));
        label(body, "ANYIOS", 13, accent, true);
        label(body, "Your apps.\nOne shared library.", 34, ink, true);
        label(body,
            "Choose how you want to work with your iOS apps. Original files stay untouched.", 16,
            muted, false);
        module(body, "01  /  PLAY & EXPLORE", "Emulator",
            "Try eligible ARM32 iPhone and iPad apps in the experimental runtime. Universal ARM64+ARM32 apps use ARM32; ARM64-only apps remain unsupported. Compatibility and device rendering require game-by-game validation.",
            MainActivity.class, 0);
        module(body, "02  /  ANALYZE & BUILD", "Converter",
            "Inspect an IPA, convert supported code, or build an explicitly experimental runtime APK. Conversion is not a promise of playability.",
            dev.radek.conventor.MainActivity.class, 1);
        label(body,
            "Imports appear in both modules. Generated APKs are separate outputs, never replacements for your IPA.",
            13, muted, false);
        label(body, "AnyiOS " + BuildConfig.VERSION_NAME, 12, muted, false);
        setContentView(scroll);
    }
    private TextView label(LinearLayout parent, String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setTypeface(
            Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        view.setPadding(0, dp(8), 0, dp(8));
        parent.addView(view);
        return view;
    }
    private void module(LinearLayout parent, String eyebrow, String title, String subtitle,
        Class<?> activity, int index) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(22), dp(18), dp(22), dp(20));
        GradientDrawable shape =
            new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {panel, background});
        shape.setCornerRadius(dp(22));
        shape.setStroke(dp(1), 0x505ce3b5);
        card.setBackground(new RippleDrawable(ColorStateList.valueOf(0x305ce3b5), shape, null));
        card.setFocusable(true);
        card.setClickable(true);
        card.setContentDescription("Open " + title);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(18);
        params.bottomMargin = dp(8);
        parent.addView(card, params);
        label(card, eyebrow, 11, accent, true);
        label(card, title + "  →", 26, ink, true);
        label(card, subtitle, 14, muted, false);
        card.setOnClickListener(v -> startActivity(new Intent(this, activity)));
        if (android.animation.ValueAnimator.areAnimatorsEnabled()) {
            card.setAlpha(0);
            card.setTranslationY(dp(14));
            card.animate()
                .alpha(1)
                .translationY(0)
                .setStartDelay(index * 80L)
                .setDuration(280)
                .start();
        }
    }
}
