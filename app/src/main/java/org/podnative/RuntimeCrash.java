package org.podnative;

import android.app.Activity;
import android.app.Dialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Locale;

/**
 * The emulator's equivalent of Android's "app has stopped" dialog. It names the exact
 * blocker the runtime reported — usually the first iOS API that has no implementation —
 * instead of a generic failure, and never claims a game ran when it did not.
 */
final class RuntimeCrash {
    private RuntimeCrash() {}
    /** Structured form of the native GUEST_BLOCKER / LAUNCH_BLOCKED report. */
    static final class Report {
        final String kind, subject, pc, detail, full;
        final boolean blocked;
        Report(
            String kind, String subject, String pc, String detail, boolean blocked, String full) {
            this.kind = kind;
            this.subject = subject;
            this.pc = pc;
            this.detail = detail;
            this.blocked = blocked;
            this.full = full;
        }
    }
    static Report parse(String outcome) {
        String kind = "", subject = "", pc = "", detail = "";
        boolean blocked = outcome.startsWith("LAUNCH_BLOCKED");
        for (String line : outcome.split("\n")) {
            if (line.startsWith("GUEST_BLOCKER: ")) {
                String rest = line.substring("GUEST_BLOCKER: ".length());
                kind = between(rest, "kind=", " subject=");
                subject = between(rest, "subject=", " pc=");
                pc = between(rest, "pc=", " instructions=");
            } else if (line.startsWith("GUEST_STOP: ")) {
                detail = line.substring("GUEST_STOP: ".length());
            } else if (detail.isEmpty() && line.startsWith("LAUNCH_BLOCKED: ")) {
                detail = line.substring("LAUNCH_BLOCKED: ".length());
            }
        }
        if (kind.isEmpty()) {
            // Load-time blockers never reach guest execution, so classify by the code the
            // loader raised rather than inventing a guest stop.
            kind = blocked ? "Startup blocked" : "Runtime stop";
            if (subject.isEmpty())
                subject = detail;
        }
        return new Report(kind, subject, pc, detail, blocked, outcome);
    }
    private static String between(String text, String start, String end) {
        int at = text.indexOf(start);
        if (at < 0)
            return "";
        at += start.length();
        int stop = text.indexOf(end, at);
        return stop < 0 ? text.substring(at) : text.substring(at, stop);
    }
    /** Shows the crash screen for a finished launch attempt. Safe to call off the UI thread. */
    static void show(Activity activity, String gameTitle, String outcome) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed())
            return;
        Report report = parse(outcome == null ? "" : outcome);
        activity.runOnUiThread(() -> present(activity, gameTitle, report));
    }
    private static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
    private static void present(Activity activity, String gameTitle, Report report) {
        if (activity.isFinishing() || activity.isDestroyed())
            return;
        boolean light = activity.getSharedPreferences("radek_settings", Context.MODE_PRIVATE)
                            .getBoolean("light_theme", false);
        int sheet = Color.parseColor(light ? "#FFFFFF" : "#141D2F");
        int ink = Color.parseColor(light ? "#16202E" : "#FFFFFF");
        int muted = Color.parseColor(light ? "#5C6877" : "#A0B2C7");
        int warning = Color.parseColor("#FF6B5E");
        int accent = dev.radek.conventor.SharedTheme.accent(activity);
        Dialog dialog = new Dialog(activity);
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(activity, 22), dp(activity, 22), dp(activity, 22), dp(activity, 16));
        GradientDrawable background = new GradientDrawable();
        background.setColor(sheet);
        background.setCornerRadius(dp(activity, 26));
        body.setBackground(background);
        TextView badge = new TextView(activity);
        badge.setText(report.blocked ? "STARTUP BLOCKED" : "RUNTIME STOPPED");
        badge.setTextSize(11);
        badge.setTextColor(warning);
        badge.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        badge.setLetterSpacing(0.12f);
        body.addView(badge);
        TextView title = new TextView(activity);
        title.setText(gameTitle);
        title.setTextSize(22);
        title.setTextColor(ink);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        title.setPadding(0, dp(activity, 8), 0, 0);
        body.addView(title);
        TextView sentence = new TextView(activity);
        sentence.setText(report.blocked
                ? "AnyiOS could not start this game. No iOS code ran to completion."
                : "The guest stopped before the app finished starting.");
        sentence.setTextSize(13);
        sentence.setTextColor(muted);
        sentence.setPadding(0, dp(activity, 6), 0, dp(activity, 14));
        body.addView(sentence);
        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(activity, 14), dp(activity, 12), dp(activity, 14), dp(activity, 12));
        GradientDrawable cardShape = new GradientDrawable();
        cardShape.setColor(Color.parseColor(light ? "#F1F4F8" : "#0E1626"));
        cardShape.setCornerRadius(dp(activity, 14));
        cardShape.setStroke(dp(activity, 1), Color.parseColor(light ? "#D8E0EA" : "#25344A"));
        card.setBackground(cardShape);
        row(activity, card, "Blocker", report.kind, ink, muted, accent);
        if (!report.subject.isEmpty())
            row(activity, card, "Details", report.subject, ink, muted, accent);
        if (!report.pc.isEmpty())
            row(activity, card, "Guest PC", report.pc, ink, muted, accent);
        body.addView(card);
        if (!report.detail.isEmpty() && !report.detail.equals(report.subject)) {
            TextView detail = new TextView(activity);
            detail.setText(report.detail);
            detail.setTextSize(11);
            detail.setTypeface(Typeface.MONOSPACE);
            detail.setTextColor(muted);
            detail.setPadding(0, dp(activity, 10), 0, 0);
            detail.setMaxLines(4);
            detail.setEllipsize(TextUtils.TruncateAt.END);
            body.addView(detail);
        }
        LinearLayout actions = new LinearLayout(activity);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.END);
        actions.setPadding(0, dp(activity, 14), 0, 0);
        Button copy = new Button(activity);
        copy.setText("Copy details");
        copy.setAllCaps(false);
        copy.setTextSize(13);
        copy.setTextColor(ink);
        copy.setOnClickListener(v -> {
            ClipboardManager clipboard =
                (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard != null)
                clipboard.setPrimaryClip(ClipData.newPlainText("AnyiOS crash report", report.full));
            copy.setText("Copied");
        });
        actions.addView(copy);
        Button close = new Button(activity);
        close.setText("Close");
        close.setAllCaps(false);
        close.setTextSize(13);
        close.setTextColor(accent);
        close.setOnClickListener(v -> dialog.dismiss());
        actions.addView(close);
        body.addView(actions);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(body);
        dialog.setContentView(scroll);
        dialog.setCanceledOnTouchOutside(true);
        dialog.show();
        Diagnostics.record("CRASH_UI",
            String.format(
                Locale.ROOT, "kind=%s subject=%s pc=%s", report.kind, report.subject, report.pc));
    }
    private static void row(Context context, LinearLayout parent, String label, String value,
        int ink, int muted, int accent) {
        LinearLayout line = new LinearLayout(context);
        line.setOrientation(LinearLayout.VERTICAL);
        line.setPadding(0, dp(context, 4), 0, dp(context, 6));
        TextView key = new TextView(context);
        key.setText(label.toUpperCase(Locale.ROOT));
        key.setTextSize(10);
        key.setLetterSpacing(0.1f);
        key.setTextColor(accent);
        key.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        line.addView(key);
        TextView text = new TextView(context);
        text.setText(value);
        text.setTextSize(14);
        text.setTextColor(ink);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextIsSelectable(true);
        line.addView(text);
        parent.addView(line);
    }
}
