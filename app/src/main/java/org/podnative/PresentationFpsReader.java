package org.podnative;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Incrementally reads completed presentation samples from the append-only runtime log. */
final class PresentationFpsReader {
    private static final int MAX_SCAN_BYTES = 32 * 1024;
    // Covers a sample split across writes without re-reading the full log tail.
    private static final int OVERLAP_BYTES = 128;
    private static final Pattern MARKER =
        Pattern.compile("AnyiOSEMU_PRESENT_FPS:[ \\t]*([0-9]+(?:\\.[0-9]+)?)[ \\t]*\\r?\\n");

    private final File file;
    private long previousLength = -1;
    private long previousModified = -1;
    private long lastMarkerPosition = -1;
    private int lastBytesRead;

    PresentationFpsReader(File file) {
        this.file = file;
    }

    /** The newest previously unseen sample, or NaN if no complete sample was written. */
    double poll() {
        if (!file.isFile()) {
            previousLength = previousModified = lastMarkerPosition = -1;
            lastBytesRead = 0;
            return Double.NaN;
        }
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long length = input.length();
            long modified = file.lastModified();
            if (length == previousLength && modified == previousModified) {
                lastBytesRead = 0;
                return Double.NaN;
            }
            boolean rewritten = previousLength >= 0
                && (length < previousLength
                    || (length == previousLength && modified != previousModified));
            long lastMarker = rewritten ? -1 : lastMarkerPosition;
            long start = Math.max(0, length - MAX_SCAN_BYTES);
            if (!rewritten && previousLength >= 0)
                start = Math.max(start, Math.max(0, previousLength - OVERLAP_BYTES));
            input.seek(start);
            byte[] bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
            // ISO-8859-1 preserves a one-to-one byte/character offset. Markers
            // are ASCII even when earlier game log text contains UTF-8.
            Matcher matcher = MARKER.matcher(new String(bytes, StandardCharsets.ISO_8859_1));
            double latest = Double.NaN;
            while (matcher.find()) {
                long markerPosition = start + matcher.start();
                if (markerPosition > lastMarker) {
                    double fps = Double.parseDouble(matcher.group(1));
                    if (Double.isFinite(fps)) {
                        latest = fps;
                        lastMarker = markerPosition;
                    }
                }
            }
            previousLength = length;
            previousModified = modified;
            lastMarkerPosition = lastMarker;
            lastBytesRead = bytes.length;
            return latest;
        } catch (Exception ignored) {
            // The file can be truncated or recreated while being read. Retry on
            // the next sample rather than letting telemetry interrupt gameplay.
            return Double.NaN;
        }
    }

    int lastBytesRead() {
        return lastBytesRead;
    }
}
