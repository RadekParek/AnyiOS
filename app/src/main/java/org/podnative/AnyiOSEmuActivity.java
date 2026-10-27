package org.podnative;

import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Debug;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RelativeLayout;
import android.widget.TextView;
import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Locale;

/** SDL host activity for AnyiOSEMU's pinned ARM32 iPhone OS runtime. */
public final class AnyiOSEmuActivity extends org.anyiose.android.MainActivity {
    public static final String EXTRA_GAME_ID = "org.podnative.anyiose.GAME_ID";
    public static final String EXTRA_GAME_TITLE = "org.podnative.anyiose.GAME_TITLE";
    public static final String EXTRA_SOURCE_PATH = "org.podnative.anyiose.SOURCE_PATH";
    public static final String EXTRA_DATA_PATH = "org.podnative.anyiose.DATA_PATH";
    public static final String EXTRA_SHOW_CONSOLE = "org.podnative.anyiose.SHOW_CONSOLE";
    public static final String EXTRA_GAME_REPORT = "org.podnative.anyiose.GAME_REPORT";

    /** The active DocumentsProvider exposes this game's isolated runtime data. */
    public static volatile String userDataPath;

    private static final int MAX_REPORT_BYTES = 64 * 1024;
    private final Handler hudHandler = new Handler(Looper.getMainLooper());
    private String gameId = "";
    private String gameTitle = "Imported game";
    private String sourcePath = "";
    private String dataPath = "";
    private long startedAt;
    private int fpsCapPreference = DisplayFrameRate.AUTO;
    private float effectiveFpsCap = 60.0f;
    private boolean performanceHudEnabled;
    private boolean boostDisplayLink;
    private int gameDisplayProfile;
    private int dynamicMinimum;
    private DynamicResolution dynamicResolution;
    private SurfaceResolution surfaceResolution;
    private boolean maxClocksEnabled;
    private boolean nativeGlesEnabled;
    private boolean forceCompositionEnabled;
    private boolean verboseGlesEnabled;
    private TextView performanceHud;
    private Runnable performanceHudSampler;
    private long previousHudWallMs;
    private long previousProcessCpuMs;
    private PresentationFpsReader fpsReader;
    private long lastMeasuredFpsAtMs;
    private double lastMeasuredFps = Double.NaN;

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
        android.content.SharedPreferences settings = getSharedPreferences("settings", MODE_PRIVATE);
        fpsCapPreference = settings.getInt("fps", DisplayFrameRate.AUTO);
        performanceHudEnabled = settings.getBoolean("performanceHud", false);
        boostDisplayLink = settings.getBoolean("boostDisplayLink", false);
        gameDisplayProfile = settings.getInt("gameDisplayProfile", GameDisplayProfile.AUTO);
        dynamicMinimum = settings.getInt("dynamicResolution", 100);
        maxClocksEnabled = settings.getBoolean("maxClocks", false);
        nativeGlesEnabled = settings.getBoolean("nativeGles", false);
        forceCompositionEnabled = settings.getBoolean("forceComposition", false);
        verboseGlesEnabled = settings.getBoolean("verboseLogging", false);

        File dataDirectory = new File(dataPath);
        fpsReader = new PresentationFpsReader(new File(dataDirectory, "AnyiOSEMU_log.txt"));
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
        DisplayFrameRate.requestPreferredMode(this, fpsCapPreference);
        effectiveFpsCap = DisplayFrameRate.resolveCap(this, fpsCapPreference);
        if (mSurface != null && dynamicMinimum < 100) {
            surfaceResolution = new SurfaceResolution(mSurface);
            dynamicResolution = new DynamicResolution(dynamicMinimum, effectiveFpsCap);
        }
        getSharedPreferences("anyiose_runtime", MODE_PRIVATE)
            .edit()
            .putString("last_data_path", userDataPath)
            .apply();
        setTitle("AnyiOSEMU · " + gameTitle);
        if (intent != null && intent.getBooleanExtra(EXTRA_SHOW_CONSOLE, false))
            showSessionBadge();
        if (performanceHudEnabled)
            showPerformanceHud();
        if (performanceHudEnabled || dynamicResolution != null)
            createPerformanceSampler();
    }

    @Override
    protected String[] getArguments() {
        if (sourcePath.isEmpty() || userDataPath == null)
            return new String[] {"--help"};
        ArrayList<String> arguments = new ArrayList<>();
        arguments.add("--anyiose-data-dir=" + userDataPath);
        arguments.add(sourcePath);
        // Keep the guest's logical layout independent of the Android surface buffer.
        // With the default profile this matches the standalone launch path.
        arguments.add(GameDisplayProfile.argument(gameDisplayProfile));
        float fpsCap = DisplayFrameRate.resolveCap(this, fpsCapPreference);
        effectiveFpsCap = fpsCap;
        arguments.add("--fps-limit=" + DisplayFrameRate.formatOption(fpsCap));
        arguments.add(boostDisplayLink ? "--boost-display-link" : "--no-boost-display-link");
        arguments.add(maxClocksEnabled ? "--perf-hints" : "--no-perf-hints");
        arguments.add(maxClocksEnabled ? "--affinity=big" : "--affinity=off");
        if (performanceHudEnabled || dynamicMinimum < 100)
            arguments.add("--print-fps");
        arguments.add(nativeGlesEnabled ? "--gles-native" : "--no-gles-native");
        arguments.add(forceCompositionEnabled ? "--force-composition" : "--no-force-composition");
        if (verboseGlesEnabled)
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

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void showPerformanceHud() {
        View content = findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup))
            return;
        ViewGroup outer = (ViewGroup) content;
        if (outer.getChildCount() == 0 || !(outer.getChildAt(0) instanceof ViewGroup))
            return;
        ViewGroup surface = (ViewGroup) outer.getChildAt(0);
        performanceHud = new TextView(this);
        performanceHud.setText("AnyiOSEMU HUD · sampling…");
        performanceHud.setTextColor(Color.WHITE);
        performanceHud.setTextSize(11);
        performanceHud.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        performanceHud.setPadding(dp(11), dp(9), dp(11), dp(9));
        performanceHud.setMaxWidth(dp(340));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xD910141D);
        background.setCornerRadius(dp(12));
        background.setStroke(dp(1), 0x664D6478);
        performanceHud.setBackground(background);
        performanceHud.setClickable(false);
        performanceHud.setFocusable(false);
        performanceHud.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        params.addRule(RelativeLayout.ALIGN_PARENT_END);
        params.topMargin =
            getSharedPreferences("settings", MODE_PRIVATE).getBoolean("showConsole", false)
            ? dp(66)
            : dp(10);
        params.rightMargin = dp(10);
        surface.addView(performanceHud, params);
    }

    private void createPerformanceSampler() {
        performanceHudSampler = new Runnable() {
            @Override
            public void run() {
                if (isFinishing() || isDestroyed())
                    return;
                updatePerformanceHud();
                hudHandler.postDelayed(this, 1000);
            }
        };
    }

    private void updatePerformanceHud() {
        long nowWall = SystemClock.elapsedRealtime();
        double sample = fpsReader.poll();
        if (!Double.isNaN(sample)) {
            lastMeasuredFps = sample;
            lastMeasuredFpsAtMs = nowWall;
            if (dynamicResolution != null && surfaceResolution != null)
                surfaceResolution.setPercent(dynamicResolution.sample(sample, nowWall));
        }
        // Dynamic resolution only needs FPS. Collecting PSS and CPU data when
        // the HUD is hidden causes unnecessary work on the Android UI thread.
        if (performanceHud == null)
            return;

        long nowCpu = Process.getElapsedCpuTime();
        String cpu = "—";
        if (previousHudWallMs > 0 && nowWall > previousHudWallMs) {
            double percent =
                (nowCpu - previousProcessCpuMs) * 100.0 / (nowWall - previousHudWallMs);
            cpu = String.format(Locale.ROOT, "%.0f%%", Math.max(0.0, percent));
        }
        previousHudWallMs = nowWall;
        previousProcessCpuMs = nowCpu;

        Debug.MemoryInfo memory = new Debug.MemoryInfo();
        Debug.getMemoryInfo(memory);
        long pssMegabytes = Math.max(0, memory.getTotalPss()) / 1024L;
        long graphicsKilobytes = 0;
        try {
            graphicsKilobytes = Long.parseLong(memory.getMemoryStats().get("summary.graphics"));
        } catch (Exception ignored) {
            // Some Android versions do not report a graphics PSS category.
        }
        String graphics = graphicsKilobytes > 0 ? (graphicsKilobytes / 1024L) + " MB" : "n/a";
        String fps = !Double.isNaN(lastMeasuredFps) && nowWall - lastMeasuredFpsAtMs <= 3000
            ? String.format(Locale.ROOT, "%.1f", lastMeasuredFps)
            : "—";
        performanceHud.setText("FPS " + fps + " · cap " + DisplayFrameRate.formatHz(effectiveFpsCap)
            + " Hz"
            + (dynamicResolution == null ? "" : " · surface " + dynamicResolution.percent() + "%")
            + "\n"
            + "CPU " + cpu + " (process, one-core equivalent) · RAM " + pssMegabytes
            + " MB PSS\nGPU utilization n/a · graphics PSS (within RAM total) " + graphics);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (performanceHudSampler != null) {
            hudHandler.removeCallbacks(performanceHudSampler);
            hudHandler.post(performanceHudSampler);
        }
    }

    @Override
    protected void onPause() {
        if (performanceHudSampler != null)
            hudHandler.removeCallbacks(performanceHudSampler);
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (performanceHudSampler != null)
            hudHandler.removeCallbacks(performanceHudSampler);
        if (surfaceResolution != null)
            surfaceResolution.close();
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
        int requestedFps = fpsCapPreference;
        long reportTime = SystemClock.elapsedRealtime();
        boolean recentFpsSample = !Double.isNaN(lastMeasuredFps) && lastMeasuredFpsAtMs > 0
            && reportTime - lastMeasuredFpsAtMs <= 3000;
        String measuredFps = recentFpsSample
            ? String.format(Locale.ROOT, "%.1f FPS (last presented-frame sample)", lastMeasuredFps)
            : "not recorded (HUD off, no sample, or last sample older than three seconds)";
        report.append("AnyiOSEMU session\nGame=")
            .append(gameTitle)
            .append("\nStarted=")
            .append(new java.util.Date(startedAt))
            .append("\nBuild=")
            .append(BuildConfig.SOURCE_REVISION)
            .append("\nAnyiOSEMU engine=")
            .append(BuildConfig.ANYIOSEMU_ENGINE_REVISION)
            .append("\nNative guest: ARM32; universal ARM64+ARM32 bundles select ARM32"
                + "; ARM64-only bundles are unsupported")
            .append("\nFPS cap request=")
            .append(requestedFps <= DisplayFrameRate.AUTO
                    ? "Auto (fastest supported mode at current resolution)"
                    : requestedFps + " FPS")
            .append("\nEffective FPS cap passed to AnyiOSEMU=")
            .append(DisplayFrameRate.formatHz(effectiveFpsCap))
            .append(" Hz\nMeasured presentation FPS=")
            .append(measuredFps)
            .append("\nGame layout=")
            .append(GameDisplayProfile.label(gameDisplayProfile))
            .append("\nPresentation base resolution=display native")
            .append("\nDynamic surface resolution=")
            .append(dynamicResolution == null ? "off" : "on, minimum " + dynamicMinimum + "%")
            .append(" (presentation surface only; offscreen game buffers may not scale)")
            .append("\nMaximize FPS (60 FPS display-link boost)=")
            .append(boostDisplayLink)
            .append("\nPerformance HUD=")
            .append(performanceHudEnabled)
            .append("\nMax clocks request=")
            .append(maxClocksEnabled)
            .append(" (best-effort Android performance hints/CPU affinity; clocks are not forced)")
            .append("\nGraphics driver=")
            .append(nativeGlesEnabled ? "device-native GLES" : "bundled ANGLE by default")
            .append("\nForce Composition=")
            .append(forceCompositionEnabled)
            .append("\nVerbose GLES logging=")
            .append(verboseGlesEnabled)
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
