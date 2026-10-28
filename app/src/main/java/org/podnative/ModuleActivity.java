package org.podnative;

import android.app.Activity;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import dev.radek.conventor.SharedTheme;

/** The umbrella launcher; neither module receives a second source-file store. */
public final class ModuleActivity extends Activity {
    private int background, ink, muted, accent;
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
        scroll.setBackground(SharedTheme.canvas(this));
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            } else
                view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(24), dp(38), dp(24), dp(32));
        scroll.addView(body);
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        ImageView mark = new ImageView(this);
        mark.setImageResource(R.drawable.ic_brand);
        mark.setContentDescription("AnyiOS pocket player");
        header.addView(mark, new LinearLayout.LayoutParams(dp(48), dp(48)));
        LinearLayout headerText = new LinearLayout(this);
        headerText.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams headerParams = new LinearLayout.LayoutParams(0, -2, 1);
        headerParams.leftMargin = dp(12);
        header.addView(headerText, headerParams);
        TextView wordmark = label(headerText, "AnyiOS", 19, ink, true);
        wordmark.setPadding(0, 0, 0, 0);
        TextView caption = label(headerText, "YOUR POCKET LIBRARY", 10, muted, true);
        caption.setLetterSpacing(0.08f);
        caption.setPadding(0, dp(3), 0, 0);
        body.addView(header);
        TextView intro = label(body, "PLAY  /  DISCOVER  /  BUILD", 11, accent, true);
        intro.setLetterSpacing(0.08f);
        LinearLayout.LayoutParams introParams = (LinearLayout.LayoutParams) intro.getLayoutParams();
        introParams.topMargin = dp(32);
        label(body, "Your games.\nOne shared library.", 34, ink, true);
        label(body,
            "Play and explore your iOS games, or inspect them for Android. Your original files stay untouched.",
            15, muted, false);
        module(body, "01  /  PLAY & EXPLORE", "Emulator",
            "Launch eligible ARM32 apps with AnyiOSEMU. Compatibility and rendering vary by game and device.",
            MainActivity.class, 0);
        module(body, "02  /  ANALYZE & BUILD", "Converter",
            "Analyze your library and build experimental Android APKs. A build does not guarantee gameplay.",
            dev.radek.conventor.MainActivity.class, 1);
        label(body,
            "One private source, two ways to explore. ARM64-only games are not supported by AnyiOSEMU.",
            13, muted, false);
        label(body, "AnyiOS " + BuildConfig.VERSION_NAME, 12, muted, false);
        setContentView(scroll);
        scroll.requestApplyInsets();
        SharedTheme.reveal(body);
    }
    private TextView label(LinearLayout parent, String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setTypeface(SharedTheme.typeface(this, bold));
        view.setPadding(0, dp(8), 0, dp(8));
        parent.addView(view);
        return view;
    }
    private void module(LinearLayout parent, String eyebrow, String title, String subtitle,
        Class<?> activity, int index) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(22), dp(18), dp(22), dp(20));
        card.setBackground(SharedTheme.glassRipple(this, 22));
        SharedTheme.pressFeedback(card);
        card.setFocusable(true);
        card.setClickable(true);
        card.setContentDescription("Open " + title);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(18);
        params.bottomMargin = dp(8);
        parent.addView(card, params);
        TextView overline = label(card, eyebrow, 10, accent, true);
        overline.setLetterSpacing(0.08f);
        LinearLayout heading = new LinearLayout(this);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon = new ImageView(this);
        icon.setImageResource(index == 0 ? R.drawable.ic_library : R.drawable.ic_diagnostics);
        icon.setImageTintList(ColorStateList.valueOf(accent));
        icon.setPadding(dp(12), dp(12), dp(12), dp(12));
        GradientDrawable iconShape = new GradientDrawable();
        iconShape.setColor(index == 0 ? 0x185ce3b5 : 0x187db4ff);
        iconShape.setCornerRadius(dp(14));
        icon.setBackground(iconShape);
        icon.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        heading.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));
        TextView name = new TextView(this);
        name.setText(title);
        name.setTextSize(25);
        name.setTextColor(ink);
        name.setTypeface(SharedTheme.typeface(this, true));
        LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(0, -2, 1);
        nameParams.leftMargin = dp(14);
        heading.addView(name, nameParams);
        TextView open = new TextView(this);
        open.setText("Open");
        open.setTextSize(13);
        open.setTextColor(accent);
        open.setTypeface(SharedTheme.typeface(this, true));
        heading.addView(open);
        card.addView(heading);
        label(card, subtitle, 14, muted, false);
        card.setOnClickListener(v -> startActivity(new Intent(this, activity)));
    }
}
