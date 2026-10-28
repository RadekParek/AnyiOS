package dev.radek.conventor

/** Reject executable allocations the native analyzer cannot support. */
internal object AnalysisMemory {
    // Must match kMaxExecutableBytes in converter/native/src/jni.cpp.
    const val MAX_EXECUTABLE_BYTES = 256L * 1024 * 1024

    fun check(bytes: Long) {
        require(bytes in 1..MAX_EXECUTABLE_BYTES) {
            "Executable is empty or exceeds the on-device 256 MiB analysis limit; use the host analyzer for larger files"
        }
    }
}
