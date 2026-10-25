package org.podnative;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.TextView;
import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/** SDL host activity for AnyiOSEMU's pinned ARM32 iPhone OS runtime. */
public final class AnyiOSEmuActivity extends org.touchhle.android.MainActivity {
    public static final String EXTRA_GAME_ID = "org.podnative.anyiose.GAME_ID";
    public static final String EXTRA_GAME_TITLE = "org.podnative.anyiose.GAME_TITLE";
    public static final String EXTRA_SOURCE_PATH = "org.podnative.anyiose.SOURCE_PATH";
    public static final String EXTRA_DATA_PATH = "org.podnative.anyiose.DATA_PATH";
    public static final String EXTRA_SHOW_CONSOLE = "org.podnative.anyiose.SHOW_CONSOLE";
    public static final String EXTRA_GAME_REPORT = "org.podnative.anyiose.GAME_REPORT";

    /** The active DocumentsProvider exposes this game's isolated runtime data. */
    public static volatile String userDataPath;

    private static final int MAX_REPORT_BYTES = 64 * 1024;
    private String gameId = "";
    private String gameTitle = "Imported game";
    private String sourcePath = "";
    private String dataPath = "";
    private long startedAt;

    @Override
    protected void onCreate(Bundle state) {
        Intent intent = getIntent();
        gameId = intent == null ? "" : intent.getStringExtra(EXTRA_GAME_ID);
        gameTitle = intent == null ? "Imported game" : intent.getStringExtra(EXTRA_GAME_TITLE);
        sourcePath = intent == null ? "" : intent.getStringExtra(EXTRA_SOURCE_PATH);
        dataPath = intent == null ? "" : intent.getStringExtra(EXTRA_DATA_PATH);
        if (gameTitle == null || gameTitle.isEmpty())
            gameTitle = "Imported game";
        if (sourcePath == null)
            sourcePath = "";
        if (dataPath == null)
            dataPath = "";
        startedAt = System.currentTimeMillis();

        File dataDirectory = new File(dataPath);
        boolean dataDirectoryReady = dataDirectory.isDirectory() || dataDirectory.mkdirs();
        try {
            File canonicalData = dataDirectory.getCanonicalFile();
            File privateFiles = getFilesDir().getCanonicalFile();
            userDataPath = dataDirectoryReady && canonicalData.isDirectory()
                    && !canonicalData.equals(privateFiles)
                    && canonicalData.toPath().startsWith(privateFiles.toPath())
                ? canonicalData.getPath()
                : null;
        } catch (Exception e) {
            userDataPath = null;
        }
        super.onCreate(state);
        getSharedPreferences("anyiose_runtime", MODE_PRIVATE)
            .edit()
            .putString("last_data_path", userDataPath)
            .apply();
        setTitle("AnyiOSEMU · " + gameTitle);
        if (intent != null && intent.getBooleanExtra(EXTRA_SHOW_CONSOLE, false))
            showSessionBadge();
    }

    @Override
    protected String[] getArguments() {
        if (sourcePath.isEmpty() || userDataPath == null)
            return new String[] {"--help"};
        ArrayList<String> arguments = new ArrayList<>();
        arguments.add("--anyiose-data-dir=" + userDataPath);
        arguments.add(sourcePath);
        arguments.add("--device-family=auto");
        int fps = getSharedPreferences("settings", MODE_PRIVATE).getInt("fps", 60);
        if (fps != 30 && fps != 60 && fps != 120)
            fps = 60;
        arguments.add("--fps-limit=" + fps);
        boolean nativeGles =
            getSharedPreferences("settings", MODE_PRIVATE).getBoolean("nativeGles", false);
        arguments.add(nativeGles ? "--gles-native" : "--no-gles-native");
        boolean forceComposition =
            getSharedPreferences("settings", MODE_PRIVATE).getBoolean("forceComposition", false);
        arguments.add(forceComposition ? "--force-composition" : "--no-force-composition");
        if (getSharedPreferences("settings", MODE_PRIVATE).getBoolean("verboseLogging", false))
            arguments.add("--verbose-gles");
        return arguments.toArray(new String[0]);
    }

    private void showSessionBadge() {
        View content = findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup))
            return;
        ViewGroup outer = (ViewGroup) content;
        if (outer.getChildCount() == 0 || !(outer.getChildAt(0) instanceof ViewGroup))
            return;
        ViewGroup surface = (ViewGroup) outer.getChildAt(0);
        TextView badge = new TextView(this);
        badge.setText("AnyiOSEMU  ·  " + gameTitle + "\nARM32 guest session — Back exits");
        badge.setTextColor(Color.WHITE);
        badge.setTextSize(12);
        badge.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        int padding = Math.round(14 * getResources().getDisplayMetrics().density);
        badge.setPadding(padding, padding, padding, padding);
        badge.setBackgroundColor(0xCC10141D);
        badge.setClickable(false);
        badge.setFocusable(false);
        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        surface.addView(badge, params);
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (!isFinishing() && !isDestroyed())
                badge.animate()
                    .alpha(0f)
                    .setDuration(250)
                    .withEndAction(() -> {
                        if (badge.getParent() instanceof ViewGroup)
                            ((ViewGroup) badge.getParent()).removeView(badge);
                    })
                    .start();
        }, 5000);
    }

    @Override
    protected void onDestroy() {
        // SDL joins its native thread during super.onDestroy(); capture the log only after
        // that shutdown has finished so the final runtime status is not cut off.
        super.onDestroy();
        if (isFinishing() && !gameId.isEmpty()) {
            Intent result = new Intent();
            result.putExtra(EXTRA_GAME_ID, gameId);
            result.putExtra(EXTRA_GAME_REPORT, sessionReport());
            setResult(RESULT_OK, result);
        }
    }

    private String sessionReport() {
        StringBuilder report = new StringBuilder();
        android.content.SharedPreferences settings = getSharedPreferences("settings", MODE_PRIVATE);
        int fps = settings.getInt("fps", 60);
        if (fps != 30 && fps != 60 && fps != 120)
            fps = 60;
        report.append("AnyiOSEMU session\nGame=")
            .append(gameTitle)
            .append("\nStarted=")
            .append(new java.util.Date(startedAt))
            .append("\nBuild=")
            .append(BuildConfig.SOURCE_REVISION)
            .append("\nAnyiOSEMU engine=")
            .append(BuildConfig.ANYIOSEMU_ENGINE_REVISION)
            .append("\nNative guest: ARM32; universal ARM32+ARM64 bundles select ARM32"
                + "; ARM64-only bundles are unsupported")
            .append("\nFPS limit=")
            .append(fps)
            .append("\nGraphics driver=")
            .append(settings.getBoolean("nativeGles", false) ? "device-native GLES"
                                                             : "bundled ANGLE by default")
            .append("\nForce Composition=")
            .append(settings.getBoolean("forceComposition", false))
            .append("\nVerbose GLES logging=")
            .append(settings.getBoolean("verboseLogging", false))
            .append("\n\n");
        File log = new File(dataPath, "AnyiOSEMU_log.txt");
        try (RandomAccessFile input = new RandomAccessFile(log, "r")) {
            long length = input.length();
            long start = Math.max(0, length - MAX_REPORT_BYTES);
            input.seek(start);
            byte[] bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
            String text = new String(bytes, StandardCharsets.UTF_8);
            if (start > 0)
                report.append("[Earlier log output omitted]\n");
            if (!dataPath.isEmpty())
                text = text.replace(dataPath, "<private per-game data>");
            if (!sourcePath.isEmpty())
                text = text.replace(sourcePath, "<private game source>");
            report.append(text);
        } catch (Exception e) {
            report.append("Runtime log unavailable: ").append(e.getMessage()).append('\n');
        }
        if (report.length() > MAX_REPORT_BYTES + 2048)
            report.setLength(MAX_REPORT_BYTES + 2048);
        if (report.indexOf("AnyiOSEMU finished with status 0") >= 0)
            report.append(
                "\nSession ended with exit code 0. This is not proof of visible rendering or compatibility.\n");
        else if (report.indexOf("AnyiOSEMU errored:") >= 0 || report.indexOf("PANIC in thread") >= 0
            || report.indexOf("FATAL: native host crash") >= 0
            || report.indexOf("FATAL host exception") >= 0 || report.indexOf("FATAL SIGNAL") >= 0
            || report.toString().matches("(?s).*AnyiOSEMU finished with status (?!0\\b)[0-9]+.*"))
            report.append(
                "\nThe runtime recorded an error; see the log above for the first blocker.\n");
        else
            report.append(
                "\nThe session ended without a clean exit-status line. A native crash or forced stop may have interrupted logging.\n");
        return report.toString();
    }
}
