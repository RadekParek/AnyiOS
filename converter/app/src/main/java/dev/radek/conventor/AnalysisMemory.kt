package dev.radek.conventor

/** Protect automatic import from allocations the native analyzer cannot safely make. */
internal object AnalysisMemory {
    // Must match kMaxExecutableBytes in converter/native/src/jni.cpp.
    const val MAX_EXECUTABLE_BYTES = 256L * 1024 * 1024
    private const val RESERVED_HEAP_BYTES = 16L * 1024 * 1024

    fun check(
        bytes: Long,
        automatic: Boolean,
        maxHeap: Long = Runtime.getRuntime().maxMemory(),
        usedHeap: Long = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory(),
    ) {
        require(bytes in 1..MAX_EXECUTABLE_BYTES) {
            "Executable is empty or exceeds the on-device 256 MiB analysis limit; use the host analyzer for larger files"
        }
        if (automatic) {
            // readBytes holds the Java array while JNI makes its own native copy.
            // This is a conservative estimate, not a guarantee of available RAM.
            val available = (maxHeap - usedHeap).coerceAtLeast(0)
            require(bytes <= (available - RESERVED_HEAP_BYTES).coerceAtLeast(0) / 2) {
                "Not enough memory for automatic analysis; the imported game was kept. Retry analysis or use the host analyzer"
            }
        }
    }
}
