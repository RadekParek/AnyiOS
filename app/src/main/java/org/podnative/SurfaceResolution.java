package org.podnative;

import android.view.SurfaceView;
import android.view.View;

/** Resizes SDL's Android presentation buffer, not the view or touch coordinate space. */
final class SurfaceResolution implements View.OnLayoutChangeListener {
    private final SurfaceView surface;
    private final int shortEdgeLimit;
    private int percent = 100;
    private int lastWidth, lastHeight;
    private boolean fixedSize;

    SurfaceResolution(SurfaceView surface, int shortEdgeLimit) {
        this.surface = surface;
        this.shortEdgeLimit = shortEdgeLimit;
        surface.addOnLayoutChangeListener(this);
        surface.post(this::apply);
    }

    void setPercent(int percent) {
        this.percent = Math.max(50, Math.min(100, percent));
        apply();
    }

    void close() {
        surface.removeOnLayoutChangeListener(this);
    }

    static int[] dimensions(int width, int height, int shortEdgeLimit, int percent) {
        if (width <= 0 || height <= 0)
            return new int[] {0, 0};
        double base = shortEdgeLimit > 0
            ? Math.min(1.0, shortEdgeLimit / (double) Math.min(width, height))
            : 1.0;
        double scale = base * Math.max(50, Math.min(100, percent)) / 100.0;
        return new int[] {Math.max(1, (int) Math.round(width * scale)),
            Math.max(1, (int) Math.round(height * scale))};
    }

    private void apply() {
        int width = surface.getWidth(), height = surface.getHeight();
        int[] size = dimensions(width, height, shortEdgeLimit, percent);
        if (size[0] == 0 || size[1] == 0)
            return;
        if (size[0] == width && size[1] == height) {
            // Let Android own the native-size buffer instead of fixing an
            // identical size, especially during the first orientation change.
            if (fixedSize) {
                surface.getHolder().setSizeFromLayout();
                fixedSize = false;
            }
            lastWidth = lastHeight = 0;
            return;
        }
        if (fixedSize && size[0] == lastWidth && size[1] == lastHeight)
            return;
        lastWidth = size[0];
        lastHeight = size[1];
        fixedSize = true;
        surface.getHolder().setFixedSize(lastWidth, lastHeight);
    }

    @Override
    public void onLayoutChange(View v, int left, int top, int right, int bottom, int oldLeft,
        int oldTop, int oldRight, int oldBottom) {
        apply();
    }
}
