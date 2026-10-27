package org.podnative;

/** Process CPU time normalized to the capacity of all processors available to the app. */
final class CpuUsage {
    private CpuUsage() {}

    static double percent(long previousCpuMs, long currentCpuMs, long previousWallMs,
        long currentWallMs, int processors) {
        if (previousCpuMs < 0 || currentCpuMs < previousCpuMs || previousWallMs <= 0
            || currentWallMs <= previousWallMs || processors <= 0)
            return Double.NaN;
        double value = (currentCpuMs - previousCpuMs) * 100.0
            / ((currentWallMs - previousWallMs) * (double) processors);
        return Math.max(0.0, Math.min(100.0, value));
    }
}
