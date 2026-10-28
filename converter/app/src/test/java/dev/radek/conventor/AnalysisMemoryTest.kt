package dev.radek.conventor

import org.junit.Assert.*
import org.junit.Test

class AnalysisMemoryTest {
    @Test fun enforcesTheNativeExecutableLimitBeforeAllocating() {
        val limit = AnalysisMemory.MAX_EXECUTABLE_BYTES
        AnalysisMemory.check(limit)
        assertThrows(IllegalArgumentException::class.java) { AnalysisMemory.check(limit + 1) }
        assertThrows(IllegalArgumentException::class.java) { AnalysisMemory.check(0) }
    }
}
