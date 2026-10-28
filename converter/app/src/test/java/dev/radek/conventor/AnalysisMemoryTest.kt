package dev.radek.conventor

import org.junit.Assert.*
import org.junit.Test

class AnalysisMemoryTest {
    @Test fun enforcesTheNativeExecutableLimitBeforeAllocating() {
        val limit = AnalysisMemory.MAX_EXECUTABLE_BYTES
        AnalysisMemory.check(limit, automatic = false)
        assertThrows(IllegalArgumentException::class.java) { AnalysisMemory.check(limit + 1, false) }
        assertThrows(IllegalArgumentException::class.java) { AnalysisMemory.check(0, false) }
    }

    @Test fun automaticAnalysisReservesHeapForTheRestOfTheApp() {
        val mib = 1024L * 1024
        AnalysisMemory.check(40 * mib, automatic = true, maxHeap = 128 * mib, usedHeap = 20 * mib)
        val failure = assertThrows(IllegalArgumentException::class.java) {
            AnalysisMemory.check(50 * mib, automatic = true, maxHeap = 128 * mib, usedHeap = 20 * mib)
        }
        assertTrue(failure.message!!.contains("imported game was kept"))
        // Manual analysis preserves the prior behavior within the native limit.
        AnalysisMemory.check(50 * mib, automatic = false, maxHeap = 128 * mib, usedHeap = 20 * mib)
    }
}
