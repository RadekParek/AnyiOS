package org.anyiose.android;

import static org.junit.Assert.assertArrayEquals;
import org.junit.Test;

public class OverlayGeometryTest {
    @Test public void reducedDrawableMapsUiBackToTheFullSizedView() {
        OverlayGeometry full = OverlayGeometry.fromDrawable(400, 200, 400, 300, 1600, 900);
        OverlayGeometry half = OverlayGeometry.fromDrawable(200, 100, 200, 150, 800, 450);
        int[] expected = {410, 220, 400, 300};
        assertArrayEquals(expected, full.inView(1600, 900, 10, 20));
        assertArrayEquals(expected, half.inView(1600, 900, 10, 20));
    }

    @Test public void actualDrawableDimensionsKeepOverlayAlignedWhenHolderFrameDiffers() {
        // The holder may report 800x450 even if EGL actually draws at 1600x900.
        // Rust sends the latter together with the bounds; never normalize by
        // the stale holder size or the overlay would be twice as large.
        OverlayGeometry actual = OverlayGeometry.fromDrawable(400, 200, 400, 300, 1600, 900);
        assertArrayEquals(new int[] {400, 200, 400, 300}, actual.inView(1600, 900, 0, 0));
    }

    @Test public void normalizedOverlayFollowsViewResizeAndMatchParent() {
        OverlayGeometry bounds = OverlayGeometry.fromDrawable(200, 100, 200, 150, 800, 450);
        assertArrayEquals(new int[] {200, 100, 200, 150}, bounds.inView(800, 450, 0, 0));
        assertArrayEquals(new int[] {400, 200, 400, 300}, bounds.inView(1600, 900, 0, 0));
        OverlayGeometry match = OverlayGeometry.fromDrawable(0, 0, 0, 0, 800, 450);
        assertArrayEquals(new int[] {0, 0, -1, -1}, match.inView(1600, 900, 0, 0));
    }
}
