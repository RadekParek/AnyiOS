package org.podnative;

import android.app.Activity;
import android.content.Context;
import android.graphics.Point;
import android.os.Build;
import android.view.Display;
import android.view.WindowManager;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Helpers for requesting a display mode and passing a separate emulator frame cap. */
final class DisplayFrameRate {
    static final int AUTO = 0;
    private static final float FALLBACK_HZ = 60.0f;

    private DisplayFrameRate() {}

    static float highestSupportedRefreshRate(Context context) {
        Display display = displayFor(context);
        if (display == null)
            return FALLBACK_HZ;
        Display.Mode current = display.getMode();
        float highest = display.getRefreshRate();
        for (Display.Mode mode : modes(display)) {
            if (current == null || sameResolution(mode, current))
                highest = Math.max(highest, mode.getRefreshRate());
        }
        return highest > 0.0f ? highest : FALLBACK_HZ;
    }

    /**
     * Return the actual pacing cap. AUTO follows the fastest mode at the current
     * display resolution; an explicit cap is clamped so it cannot exceed it.
     */
    static float resolveCap(Context context, int requestedFps) {
        float highest = highestSupportedRefreshRate(context);
        return requestedFps <= AUTO ? highest : Math.min(requestedFps, highest);
    }

    /** Request the best matching mode without changing display resolution when possible. */
    static void requestPreferredMode(Activity activity, int requestedFps) {
        Display display = displayFor(activity);
        if (display == null)
            return;
        Display.Mode current = display.getMode();
        List<Display.Mode> candidates = new ArrayList<>();
        for (Display.Mode mode : modes(display)) {
            if (current == null || sameResolution(mode, current))
                candidates.add(mode);
        }
        if (candidates.isEmpty()) {
            for (Display.Mode mode : modes(display)) candidates.add(mode);
        }
        if (candidates.isEmpty())
            return;

        float target = resolveCap(activity, requestedFps);
        Display.Mode selected = null;
        float selectedRate = -1.0f;
        for (Display.Mode mode : candidates) {
            float rate = mode.getRefreshRate();
            if (rate <= target + 0.05f && rate > selectedRate) {
                selected = mode;
                selectedRate = rate;
            }
        }
        if (selected == null) {
            selected = candidates.get(0);
            for (Display.Mode mode : candidates)
                if (mode.getRefreshRate() < selected.getRefreshRate())
                    selected = mode;
        }

        WindowManager.LayoutParams attributes = activity.getWindow().getAttributes();
        attributes.preferredDisplayModeId = selected.getModeId();
        activity.getWindow().setAttributes(attributes);
    }

    /** Current display size capped to the user's short-edge render setting. */
    static int[] renderSize(Context context, int shortEdgeLimit) {
        Display display = displayFor(context);
        if (display == null)
            return new int[] {0, 0};
        Point size = new Point();
        display.getRealSize(size);
        return SurfaceResolution.dimensions(size.x, size.y, shortEdgeLimit, 100);
    }

    static String formatHz(float rate) {
        if (Math.abs(rate - Math.round(rate)) < 0.05f)
            return String.format(Locale.ROOT, "%.0f", rate);
        return String.format(Locale.ROOT, "%.1f", rate);
    }

    static String formatOption(float rate) {
        return String.format(Locale.ROOT, "%.3f", rate)
            .replaceAll("0+$", "")
            .replaceAll("\\.$", "");
    }

    private static Display displayFor(Context context) {
        if (Build.VERSION.SDK_INT >= 30 && context instanceof Activity) {
            Display activityDisplay = ((Activity) context).getDisplay();
            if (activityDisplay != null)
                return activityDisplay;
        }
        WindowManager manager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        return manager == null ? null : manager.getDefaultDisplay();
    }

    private static Display.Mode[] modes(Display display) {
        Display.Mode[] modes = display.getSupportedModes();
        return modes == null ? new Display.Mode[0] : modes;
    }

    private static boolean sameResolution(Display.Mode first, Display.Mode second) {
        return first.getPhysicalWidth() == second.getPhysicalWidth()
            && first.getPhysicalHeight() == second.getPhysicalHeight();
    }
}
