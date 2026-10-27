package org.podnative;

import static org.junit.Assert.*;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.junit.Test;

public class PresentationFpsReaderTest {
    private static void write(File file, String text, boolean append) throws Exception {
        try (FileOutputStream output = new FileOutputStream(file, append)) {
            output.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void append(File file, String text) throws Exception {
        write(file, text, true);
    }

    @Test
    public void consumesOnlyNewCompleteSamplesEvenWithUtf8Text() throws Exception {
        File log = File.createTempFile("presentation-fps", ".log");
        try {
            PresentationFpsReader reader = new PresentationFpsReader(log);
            assertTrue(Double.isNaN(reader.poll()));
            append(log, "游戏 🎮\nAnyiOSEMU_PRESENT_FPS: 48.5\n");
            assertEquals(48.5, reader.poll(), 0.0);
            assertTrue(Double.isNaN(reader.poll()));
            assertEquals(0, reader.lastBytesRead());
            append(log, "unrelated output\n");
            assertTrue(Double.isNaN(reader.poll()));
            append(log, "AnyiOSEMU_PRESENT_FPS: 55.0\nAnyiOSEMU_PRESENT_FPS: 61.5\n");
            assertEquals(61.5, reader.poll(), 0.0);
            append(log, "AnyiOSEMU_PRESENT_FPS: 7");
            assertTrue(Double.isNaN(reader.poll()));
            append(log, "2.0\n");
            assertEquals(72.0, reader.poll(), 0.0);
        } finally {
            log.delete();
        }
    }

    @Test
    public void handlesTruncationSameSizeRewriteAndMissingFile() throws Exception {
        File log = File.createTempFile("presentation-fps", ".log");
        try {
            PresentationFpsReader reader = new PresentationFpsReader(log);
            write(log, "older output\nAnyiOSEMU_PRESENT_FPS: 48.0\n", false);
            assertEquals(48.0, reader.poll(), 0.0);
            write(log, "AnyiOSEMU_PRESENT_FPS: 10.0\n", false);
            assertEquals(10.0, reader.poll(), 0.0);
            long modified = log.lastModified();
            write(log, "AnyiOSEMU_PRESENT_FPS: 20.0\n", false);
            assertTrue(log.setLastModified(modified + 2000));
            assertEquals(20.0, reader.poll(), 0.0);
            assertTrue(log.delete());
            assertTrue(Double.isNaN(reader.poll()));
            write(log, "AnyiOSEMU_PRESENT_FPS: 30.0\n", false);
            assertEquals(30.0, reader.poll(), 0.0);
        } finally {
            log.delete();
        }
    }

    @Test
    public void boundsInitialScanAndReadsOnlyNewTail() throws Exception {
        File log = File.createTempFile("presentation-fps", ".log");
        try {
            StringBuilder noise = new StringBuilder();
            for (int i = 0; i < 40_000; i++) noise.append('x');
            write(log, noise + "\nAnyiOSEMU_PRESENT_FPS: 60.0\n", false);
            PresentationFpsReader reader = new PresentationFpsReader(log);
            assertEquals(60.0, reader.poll(), 0.0);
            assertTrue(reader.lastBytesRead() <= 32 * 1024);
            append(log, "more output\n");
            assertTrue(Double.isNaN(reader.poll()));
            assertTrue(reader.lastBytesRead() <= 128 + "more output\n".length());
            append(log, "AnyiOSEMU_PRESENT_FPS: 59.0\r\n");
            assertEquals(59.0, reader.poll(), 0.0);
        } finally {
            log.delete();
        }
    }
}
