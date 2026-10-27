package org.podnative;

import android.content.Context;
import android.view.Display;
import java.util.ArrayList;
import java.util.List;

/** Presentation-buffer choices derived from the current Android display mode. */
final class DisplayResolution {
    static final int NATIVE = 0;
    static final String PREFERENCE = "surfaceShortEdge";
    private static final int[] SHORT_EDGES = {720, 1080, 1440};

    private DisplayResolution() {}

    static int[] displaySize(Context context) {
        Display display = DisplayFrameRate.displayFor(context);
        Display.Mode mode = display == null ? null : display.getMode();
        if (mode == null)
            return new int[] {0, 0};
        // List dimensions landscape-first, independent of the settings screen's rotation.
        return new int[] {Math.max(mode.getPhysicalWidth(), mode.getPhysicalHeight()),
            Math.min(mode.getPhysicalWidth(), mode.getPhysicalHeight())};
    }

    static int[] choices(int width, int height, int selected) {
        int shortEdge = Math.min(width, height);
        List<Integer> choices = new ArrayList<>();
        choices.add(NATIVE);
        if (shortEdge > 0)
            for (int edge : SHORT_EDGES)
                if (edge < shortEdge)
                    choices.add(edge);
        // Keep a previously selected option visible if the device mode changed.
        if (selected > 0 && !choices.contains(selected))
            choices.add(selected);
        int[] result = new int[choices.size()];
        for (int i = 0; i < result.length; i++) result[i] = choices.get(i);
        return result;
    }

    static String label(int width, int height, int shortEdge) {
        if (width <= 0 || height <= 0)
            return shortEdge <= 0 ? "Native" : "Up to " + shortEdge + " px short edge";
        int[] dimensions = SurfaceResolution.dimensions(width, height, shortEdge, 100);
        String pixels = dimensions[0] + "×" + dimensions[1];
        if (shortEdge <= 0)
            return pixels + " · Native";
        if (shortEdge >= Math.min(width, height))
            return pixels + " · limited by display";
        return pixels;
    }
}
