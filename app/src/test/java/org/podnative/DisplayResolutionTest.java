package org.podnative;

import static org.junit.Assert.*;
import org.junit.Test;

public class DisplayResolutionTest {
    @Test
    public void choicesUseActualDisplayPixelsAndKeepNativeAsDefault() {
        assertArrayEquals(new int[] {0, 720, 1080}, DisplayResolution.choices(3088, 1440, 0));
        assertEquals("3088×1440 · Native", DisplayResolution.label(3088, 1440, 0));
        assertEquals("2316×1080", DisplayResolution.label(3088, 1440, 1080));
        assertEquals("1544×720", DisplayResolution.label(3088, 1440, 720));
    }

    @Test
    public void displayModeChangesDoNotHidePreviouslySelectedSettingsOrUpscale() {
        assertArrayEquals(new int[] {0, 720, 1440}, DisplayResolution.choices(2400, 1080, 1440));
        assertEquals("2400×1080 · limited by display", DisplayResolution.label(2400, 1080, 1440));
        assertEquals("Native", DisplayResolution.label(0, 0, 0));
        assertEquals("Up to 1080 px short edge", DisplayResolution.label(0, 0, 1080));
    }
}
