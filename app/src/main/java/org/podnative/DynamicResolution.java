package org.podnative;

/**
 * Frame-sample controller for the Android presentation surface. Off unless explicitly enabled.
 * The game may render into its own offscreen buffers, which this controller cannot resize.
 */
final class DynamicResolution {
    private final int minimum;
    private final double displayCap;
    private int percent = 100;
    private int slowSamples, fastSamples, highRateSamples;
    private long lastChange;
    private boolean highRate;
    private double baseline;
    private int unsuccessfulSteps;
    private long retryAfter;

    DynamicResolution(int minimumPercent, double cap) {
        minimum = Math.max(50, Math.min(100, minimumPercent));
        displayCap = Double.isFinite(cap) && cap > 0 ? cap : 60;
    }

    int percent() {
        return percent;
    }
    double target() {
        return highRate ? displayCap : Math.min(60, displayCap);
    }

    /** Consume a fresh measured presentation FPS sample, not a guessed cap. */
    int sample(double fps, long nowMs) {
        if (!Double.isFinite(fps) || fps <= 0 || minimum == 100 || nowMs < retryAfter)
            return percent;
        // Do not treat a game locked at 60 as if it were capable of 120.
        if (displayCap > 65 && fps > 63) {
            if (++highRateSamples >= 3)
                highRate = true;
        } else {
            highRateSamples = 0;
        }
        if (nowMs - lastChange < 3000)
            return percent;
        double target = target();
        if (fps < target * 0.90) {
            fastSamples = 0;
            if (++slowSamples >= 3 && percent > minimum) {
                if (baseline > 0 && fps < baseline + 2.0)
                    unsuccessfulSteps++;
                else
                    unsuccessfulSteps = 0;
                // CPU-limited or deliberately low-FPS games don't benefit from
                // a smaller surface. Restore quality and retry later instead.
                if (unsuccessfulSteps >= 2) {
                    percent = 100;
                    baseline = 0;
                    unsuccessfulSteps = 0;
                    retryAfter = nowMs + 20000;
                } else {
                    baseline = fps;
                    percent = Math.max(minimum, percent - 10);
                }
                lastChange = nowMs;
                slowSamples = 0;
            }
        } else if (fps >= target * 0.98) {
            slowSamples = 0;
            if (++fastSamples >= 10 && percent < 100) {
                percent = Math.min(100, percent + 5);
                baseline = 0;
                unsuccessfulSteps = 0;
                lastChange = nowMs;
                fastSamples = 0;
            }
        } else {
            slowSamples = fastSamples = 0;
        }
        return percent;
    }
}
