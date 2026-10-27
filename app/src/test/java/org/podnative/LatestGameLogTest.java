package org.podnative;

import static org.junit.Assert.*;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class LatestGameLogTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void startsFreshAndReadsTheRealFileEvenWithoutAnActivityResult() throws Exception {
        File data = temporary.newFolder("data");
        File log = new File(data, "AnyiOSEMU_log.txt");
        Files.write(log.toPath(), "previous session".getBytes(StandardCharsets.UTF_8));
        LatestGameLog.prepare(data);
        assertTrue(LatestGameLog.preview(data, 1024).contains("No log was recorded"));
        Files.write(log.toPath(), "guest printf\ncrash info\n".getBytes(StandardCharsets.UTF_8));
        assertEquals("guest printf\ncrash info\n", LatestGameLog.preview(data, 1024));
        ByteArrayOutputStream exported = new ByteArrayOutputStream();
        LatestGameLog.copy(data, exported);
        assertEquals("guest printf\ncrash info\n", exported.toString(StandardCharsets.UTF_8));
    }

    @Test
    public void previewsOnlyTheTailButExportsTheCompleteUtf8Log() throws Exception {
        File data = temporary.newFolder("utf8");
        Files.write(new File(data, "AnyiOSEMU_log.txt").toPath(),
            "old text ☃ newest\n".getBytes(StandardCharsets.UTF_8));
        String tail = LatestGameLog.preview(data, 10);
        assertTrue(tail.contains("Earlier log output omitted"));
        assertTrue(tail.endsWith("newest\n"));
        assertFalse(tail.contains("\ufffd"));
        ByteArrayOutputStream exported = new ByteArrayOutputStream();
        LatestGameLog.copy(data, exported);
        assertEquals("old text ☃ newest\n", exported.toString(StandardCharsets.UTF_8));
    }

    @Test
    public void engineEvidenceUsesOnlyTheCurrentBoundedLogTail() throws Exception {
        File data = temporary.newFolder("engine");
        File log = new File(data, "AnyiOSEMU_log.txt");
        byte[] padding = new byte[LatestGameLog.PREVIEW_BYTES];
        java.util.Arrays.fill(padding, (byte) 'x');
        try (java.io.FileOutputStream output = new java.io.FileOutputStream(log)) {
            output.write(("old " + GameEngineDetector.OCTARINE_SOURCE + "\n")
                .getBytes(StandardCharsets.UTF_8));
            output.write(padding);
        }
        GameEngineDetector detector = new GameEngineDetector();
        detector.observeRuntimeLog(LatestGameLog.preview(data, LatestGameLog.PREVIEW_BYTES));
        assertEquals("Unknown", detector.result());

        Files.write(log.toPath(), ("/Users/codeglue/" + GameEngineDetector.OCTARINE_SOURCE)
            .getBytes(StandardCharsets.UTF_8));
        detector.observeRuntimeLog(LatestGameLog.preview(data, LatestGameLog.PREVIEW_BYTES));
        assertEquals("Octarine, Unity (detected)",
            GameEngineDetector.merge("Unity (detected)", detector.result()));
        assertEquals("Octarine (detected)",
            GameEngineDetector.merge(detector.result(), detector.result()));
    }

    @Test
    public void missingAndEmptyLogsAreNotConfusedWithPreviousReports() throws Exception {
        File data = temporary.newFolder("empty");
        assertTrue(LatestGameLog.preview(data, 100).contains("No log was recorded"));
        LatestGameLog.prepare(data);
        assertThrows(java.io.IOException.class,
            () -> LatestGameLog.copy(data, new ByteArrayOutputStream()));
    }
}
