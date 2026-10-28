package dev.radek.conventor

import org.junit.Assert.*
import org.junit.Test

class ArtworkSizingTest {
    @Test fun targetAndDecodedSizeAreBoundedWithoutUpscaling() {
        assertEquals(512, ArtworkSizing.target(900))
        assertEquals(1, ArtworkSizing.target(0))
        assertEquals(4, ArtworkSizing.sample(2048, 2048, 300))
        assertEquals(300 to 300, ArtworkSizing.scaled(512, 512, 300))
        assertEquals(64 to 32, ArtworkSizing.scaled(64, 32, 300))
    }

    @Test fun wideOrTallArtworkCannotRetainAnUnboundedEdge() {
        assertEquals(8, ArtworkSizing.sample(4096, 256, 400))
        assertEquals(400 to 25, ArtworkSizing.scaled(4096, 256, 400))
        assertEquals(25 to 400, ArtworkSizing.scaled(256, 4096, 400))
        assertThrows(IllegalArgumentException::class.java) { ArtworkSizing.scaled(0, 50, 100) }
    }
}
