package org.podnative;

import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Best-effort device-wide GPU busy counters; never estimate usage from FPS or memory. */
final class GpuUsageReader {
    private static final long RETRY_MS = 15000;
    private static final long BUSY_HOLD_MS = 5000;
    private final File sys, proc;
    private Source active;
    private long lastDiscovery = Long.MIN_VALUE;
    private double lastBusy = Double.NaN;
    private long lastBusyAt;

    private static final class Source {
        final File file;
        final boolean busyPair;
        Source(File file, boolean busyPair) {
            this.file = file;
            this.busyPair = busyPair;
        }
    }

    GpuUsageReader() {
        this(new File("/sys"), new File("/proc"));
    }

    GpuUsageReader(File sys, File proc) {
        this.sys = sys;
        this.proc = proc;
    }

    /** Returns a global percentage, or NaN if Android does not expose a usable counter. */
    double sample(long nowMs) {
        if (active == null) {
            if (lastDiscovery != Long.MIN_VALUE && nowMs >= lastDiscovery
                && nowMs - lastDiscovery < RETRY_MS)
                return Double.NaN;
            lastDiscovery = nowMs;
            Source pendingKgsl = null;
            for (Source source : candidates()) {
                String text = read(source.file);
                if (text == null || !isValid(source, text))
                    continue;
                if (source.busyPair && Double.isNaN(parseBusyPair(text))) {
                    // 0 0 is not a reading. Prefer any other usable source,
                    // but keep polling this one if nothing else is available.
                    if (pendingKgsl == null)
                        pendingKgsl = source;
                    continue;
                }
                active = source;
                return parseSample(source, text, nowMs);
            }
            active = pendingKgsl;
            return Double.NaN;
        }
        String text = read(active.file);
        if (text == null || !isValid(active, text)) {
            active = null;
            lastBusy = Double.NaN;
            return Double.NaN;
        }
        return parseSample(active, text, nowMs);
    }

    private double parseSample(Source source, String text, long nowMs) {
        if (!source.busyPair)
            return parsePercent(text);
        double value = parseBusyPair(text);
        if (!Double.isNaN(value)) {
            lastBusy = value;
            lastBusyAt = nowMs;
            return value;
        }
        // KGSL can expose "0 0" while its counters reset between windows. Do
        // not treat an empty window as zero usage. Briefly retain the last real
        // window, then report unavailable until the next published sample.
        if (Double.isNaN(lastBusy))
            return Double.NaN;
        if (nowMs >= lastBusyAt && nowMs - lastBusyAt <= BUSY_HOLD_MS)
            return lastBusy;
        return Double.NaN;
    }

    private static boolean isValid(Source source, String text) {
        if (!source.busyPair)
            return !Double.isNaN(parsePercent(text));
        if (!Double.isNaN(parseBusyPair(text)))
            return true;
        return text.trim().matches("0\\s+0");
    }

    static double parseBusyPair(String text) {
        String[] fields = text.trim().split("\\s+");
        if (fields.length != 2)
            return Double.NaN;
        try {
            long busy = Long.parseLong(fields[0]), total = Long.parseLong(fields[1]);
            if (busy < 0 || total <= 0 || busy > total)
                return Double.NaN;
            return busy * 100.0 / total;
        } catch (NumberFormatException e) {
            return Double.NaN;
        }
    }

    static double parsePercent(String text) {
        // Known percentage and devfreq "load@frequency" formats. Values with
        // unknown units/ranges are rejected rather than guessed.
        String value = text.trim();
        if (!value.matches("[0-9]{1,3}(?:\\s*%|@[0-9]+)?"))
            return Double.NaN;
        int end = value.indexOf('@');
        if (end >= 0)
            value = value.substring(0, end);
        value = value.replace("%", "").trim();
        int percent = Integer.parseInt(value);
        return percent <= 100 ? percent : Double.NaN;
    }

    private List<Source> candidates() {
        List<Source> result = new ArrayList<>();
        add(result, sys, "class/kgsl/kgsl-3d0/gpubusy", true);
        add(result, sys, "class/kgsl/kgsl-3d0/gpu_busy_percentage", false);
        add(result, sys, "class/kgsl/kgsl-3d0/devfreq/gpu_load", false);
        add(result, sys, "class/kgsl/kgsl-3d0/gpuload", false);
        add(result, sys, "kernel/gpu/gpu_busy", false);
        add(result, sys, "class/misc/sgpu/device/gpu_busy_percent", false);
        add(result, sys, "class/misc/mali0/device/utilization", false);
        add(result, proc, "ged/hal/loading", false);
        add(result, proc, "ged/hal/gpu_loading", false);
        File[] devfreq = new File(sys, "class/devfreq").listFiles();
        if (devfreq != null)
            for (File node : devfreq) {
                String name = node.getName().toLowerCase(Locale.ROOT);
                if (!name.matches(".*(gpu|mali|kgsl|g3d|sgpu|adreno|xclipse|pvr).*"))
                    continue;
                add(result, node, "load", false);
                add(result, node, "gpu_load", false);
            }
        return result;
    }

    private static void add(List<Source> sources, File base, String path, boolean busyPair) {
        sources.add(new Source(new File(base, path), busyPair));
    }

    private static String read(File file) {
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] bytes = new byte[256];
            int length = input.read(bytes);
            return length > 0 ? new String(bytes, 0, length, StandardCharsets.US_ASCII) : null;
        } catch (Exception e) {
            // Many production Android kernels forbid unprivileged access to GPU counters.
            return null;
        }
    }
}
