package org.podnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity {
    private static final int IMPORT = 10, EXPORT = 11;
    private SharedPreferences prefs;
    private PrototypeView prototype;
    private TextView liveStatus;
    private String tab = "Library";
    private boolean resumeTest;
    private final int ink = Color.rgb(230, 239, 244), muted = Color.rgb(155, 175, 189);
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("settings", MODE_PRIVATE);
        Diagnostics.record("APP", "foundation started; guest execution unavailable");
        resumeTest = state != null && state.getBoolean("prototype", false);
        home();
    }
    private int dp(int n) {
        return Math.round(n * getResources().getDisplayMetrics().density);
    }
    private TextView text(String value, int size, int color) {
        TextView v = new TextView(this);
        v.setText(value);
        v.setTextSize(size);
        v.setTextColor(color);
        v.setPadding(0, dp(7), 0, dp(7));
        return v;
    }
    private LinearLayout column() {
        LinearLayout v = new LinearLayout(this);
        v.setOrientation(LinearLayout.VERTICAL);
        return v;
    }
    private void button(LinearLayout parent, String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(v -> action.run());
        parent.addView(b, new LinearLayout.LayoutParams(-1, dp(54)));
    }
    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = column();
        c.setPadding(dp(18), dp(12), dp(18), dp(16));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.rgb(28, 42, 54));
        bg.setCornerRadius(dp(18));
        bg.setStroke(dp(1), Color.rgb(49, 67, 80));
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.setMargins(0, dp(12), 0, dp(8));
        parent.addView(c, lp);
        return c;
    }
    private void home() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        if (android.os.Build.VERSION.SDK_INT >= 30)
            getWindow().getInsetsController().show(WindowInsets.Type.systemBars());
        LinearLayout root = column();
        root.setBackgroundColor(Color.rgb(17, 26, 35));
        root.setPadding(dp(20), dp(12), dp(20), dp(12));
        root.setFitsSystemWindows(true);
        TextView title = text("PodNative", 32, ink);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        root.addView(title);
        root.addView(text("YOUR LIBRARY. A NEW RUNTIME.", 11, muted));
        LinearLayout tabs = new LinearLayout(this);
        for (String name : new String[] {"Library", "Settings", "Diagnostics"}) {
            Button b = new Button(this);
            b.setText(name);
            b.setTextSize(12);
            b.setAllCaps(false);
            b.setSelected(tab.equals(name));
            b.setContentDescription(name + (tab.equals(name) ? ", selected" : ""));
            b.setOnClickListener(v -> {
                tab = name;
                home();
            });
            tabs.addView(b, new LinearLayout.LayoutParams(0, dp(52), 1));
        }
        root.addView(tabs);
        ScrollView scroll = new ScrollView(this);
        LinearLayout content = column();
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        if (tab.equals("Library"))
            library(content);
        else if (tab.equals("Settings"))
            settings(content);
        else
            diagnostics(content);
        setContentView(root);
    }
    private void library(LinearLayout c) {
        LinearLayout hero = card(c);
        hero.addView(text("A careful first step", 23, ink));
        hero.addView(text("Foundation preview · ARM64 Android", 13, muted));
        hero.addView(text(
            "Test the graphics path before testing a game. This build cannot execute iOS apps yet.",
            16, ink));
        button(hero, "Launch 3D graphics test", this::launch);
        LinearLayout games = card(c);
        games.addView(text("On this device", 21, ink));
        File[] files = new File(getFilesDir(), "imports").listFiles();
        if (files == null || files.length == 0)
            games.addView(text("No imports yet. Choose your own legal IPA or ARM32 Mach-O for "
                    + "structural inspection. Nothing is uploaded.",
                15, muted));
        else
            for (File file : files)
                button(games, "Inspect import · " + file.getName(), () -> inspect(file));
        button(games, "Import for inspection", () -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                           .setType("*/*")
                           .addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(i, IMPORT);
        });
        button(games, "Remove local imports",
            ()
                -> new AlertDialog.Builder(this)
                    .setTitle("Remove local copies?")
                    .setMessage("Original files are not changed.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Remove",
                        (d, w) -> {
                            File[] list = new File(getFilesDir(), "imports").listFiles();
                            if (list != null)
                                for (File f : list)
                                    if (!f.delete())
                                        Diagnostics.record("IMPORT", "delete failed");
                            home();
                        })
                    .show());
        c.addView(text("Ninjago: not tested · no compatibility claim\nGame launch, input overlays, "
                + "audio and saves are not implemented.",
            13, muted));
    }
    private void settings(LinearLayout c) {
        LinearLayout g = card(c);
        g.addView(text("Graphics", 23, ink));
        g.addView(
            text("Embedded upstream ANGLE → Vulkan\nNo system driver switch. No untested fallback.",
                15, muted));
        button(g, "Resolution · " + prefs.getInt("resolution", 1080) + "p short edge",
            ()
                -> choose("Render resolution", new String[] {"720p", "1080p (FHD)", "1440p (QHD)"},
                    new int[] {720, 1080, 1440}, "resolution"));
        button(g, "Frame cap · " + prefs.getInt("fps", 60) + " FPS",
            ()
                -> choose("Prototype frame cap",
                    new String[] {"30 FPS", "60 FPS", "120 FPS (experimental)"},
                    new int[] {30, 60, 120}, "fps"));
        g.addView(text("Applied on next launch. Resolution never exceeds the current surface. "
                + "Vsync remains enabled; a frame cap is not an FPS guarantee.",
            14, muted));
        LinearLayout a = card(c);
        a.addView(text("Runtime status", 21, ink));
        a.addView(text("Guest CPU: not integrated\nEAGL / Core Animation: not implemented\nTouch "
                + "overlay: unavailable until guest input exists\nDiagnostics: always "
                + "enabled in foundation builds",
            15, muted));
        button(a, "Reset settings", () -> {
            prefs.edit().clear().apply();
            home();
        });
    }
    private void choose(String title, String[] labels, int[] values, String key) {
        new AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(labels,
                (d, which) -> {
                    prefs.edit().putInt(key, values[which]).apply();
                    home();
                })
            .setNegativeButton("Cancel", null)
            .show();
    }
    private void diagnostics(LinearLayout c) {
        c.addView(text("An honest view of the runtime", 23, ink));
        c.addView(text(Diagnostics.export(), 13, muted));
        button(c, "Export diagnostic log",
            ()
                -> startActivityForResult(
                    new Intent(Intent.ACTION_CREATE_DOCUMENT)
                        .setType("text/plain")
                        .addCategory(Intent.CATEGORY_OPENABLE)
                        .putExtra(Intent.EXTRA_TITLE, "PodNative-diagnostics.txt"),
                    EXPORT));
        c.addView(text("Native shader and EGL errors are also available through adb logcat -s "
                + "PodNative ANGLE. Inspect logs before sharing; never attach game assets.",
            14, muted));
    }
    private void launch() {
        LinearLayout root = column();
        root.setBackgroundColor(Color.BLACK);
        button(root, "Stop test & return to library", this::stop);
        liveStatus = text("Initializing real ANGLE…", 12, ink);
        root.addView(liveStatus);
        prototype = new PrototypeView(
            this, prefs.getInt("fps", 60), prefs.getInt("resolution", 1080), s -> {
                if (liveStatus != null)
                    liveStatus.setText(s);
            });
        root.addView(prototype, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            controller.hide(WindowInsets.Type.systemBars());
            controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
    private void stop() {
        resumeTest = false;
        if (prototype != null) {
            prototype.halt();
            prototype = null;
        }
        liveStatus = null;
        home();
    }
    @Override
    protected void onPause() {
        boolean wasRunning = prototype != null;
        if (wasRunning)
            stop();
        resumeTest = wasRunning;
        Diagnostics.record("LIFECYCLE", "paused; surface resources released");
        super.onPause();
    }
    @Override
    protected void onResume() {
        super.onResume();
        if (resumeTest) {
            resumeTest = false;
            launch();
        }
    }
    @Override
    protected void onSaveInstanceState(Bundle out) {
        out.putBoolean("prototype", prototype != null || resumeTest);
        super.onSaveInstanceState(out);
    }
    @Override
    public void onBackPressed() {
        if (prototype != null)
            stop();
        else
            super.onBackPressed();
    }
    private void inspect(File file) {
        new Thread(() -> {
            String result;
            try {
                result = GameInspector.inspect(file);
            } catch (Exception | LinkageError e) {
                result = "Inspection failed: " + e.getMessage();
            }
            Diagnostics.record("MACHO_INSPECTION", result);
            String message = result;
            runOnUiThread(() -> {
                if (!isFinishing() && !isDestroyed())
                    new AlertDialog.Builder(this)
                        .setTitle("Executable inspection — not game launch")
                        .setMessage(message)
                        .setPositiveButton("Done", null)
                        .show();
            });
        }, "PodNative-inspect").start();
    }
    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        android.net.Uri uri = data.getData();
        new Thread(() -> {
            File temp = null;
            try {
                if (request == EXPORT) {
                    try (OutputStream out = getContentResolver().openOutputStream(uri)) {
                        if (out == null)
                            throw new java.io.IOException("Provider did not open destination");
                        out.write(Diagnostics.export().getBytes(StandardCharsets.UTF_8));
                    }
                } else if (request == IMPORT) {
                    File dir = new File(getFilesDir(), "imports");
                    if (!dir.isDirectory() && !dir.mkdirs())
                        throw new java.io.IOException("Cannot create private import storage");
                    File[] existing = dir.listFiles();
                    if (existing != null && existing.length >= 8)
                        throw new java.io.IOException(
                            "8-import limit reached. Remove local imports first.");
                    temp = File.createTempFile("import-", ".part", dir);
                    try (InputStream in = getContentResolver().openInputStream(uri);
                        FileOutputStream out = new FileOutputStream(temp)) {
                        if (in == null)
                            throw new java.io.IOException("Provider did not open source");
                        byte[] buffer = new byte[65536];
                        int n;
                        long total = 0;
                        while ((n = in.read(buffer)) != -1) {
                            total += n;
                            if (total > 1024L * 1024 * 1024)
                                throw new java.io.IOException("Import exceeds 1 GiB safety limit");
                            out.write(buffer, 0, n);
                        }
                    }
                    String report = GameInspector.inspect(temp);
                    Diagnostics.record("IMPORT", report);
                    if (!report.startsWith("INSPECTED_ARM32:"))
                        throw new java.io.IOException(report);
                    File stored = new File(dir, "ARM32-" + System.currentTimeMillis() + ".bin");
                    if (!temp.renameTo(stored))
                        throw new java.io.IOException("Cannot finalize import");
                    temp = null;
                    runOnUiThread(() -> {
                        if (!isDestroyed()) {
                            home();
                            inspect(stored);
                        }
                    });
                }
            } catch (Exception | LinkageError e) {
                Diagnostics.record("STORAGE_ERROR", e.toString());
                runOnUiThread(() -> {
                    if (!isFinishing() && !isDestroyed())
                        new AlertDialog.Builder(this)
                            .setTitle("Action could not complete")
                            .setMessage(e.getMessage())
                            .setPositiveButton("OK", null)
                            .show();
                });
            } finally {
                if (temp != null && !temp.delete())
                    Diagnostics.record("IMPORT", "temporary file cleanup failed");
            }
        }, "PodNative-storage").start();
    }
}
