package dev.radek.conventor

import org.junit.Assert.assertEquals
import org.junit.Test

class GridSizingTest {
    @Test fun sharedGridSizingAdaptsToWidthAndLargerText() {
        assertEquals(1, SharedTheme.gridColumns(280, 1f))
        assertEquals(2, SharedTheme.gridColumns(320, 1f))
        assertEquals(3, SharedTheme.gridColumns(400, 1f))
        assertEquals(2, SharedTheme.gridColumns(400, 1.5f))
        assertEquals(4, SharedTheme.gridColumns(800, 1f))
        assertEquals(3, SharedTheme.gridColumns(800, 1.5f))
        assertEquals(180, SharedTheme.artworkSizeDp(1200, 4))
    }
}
