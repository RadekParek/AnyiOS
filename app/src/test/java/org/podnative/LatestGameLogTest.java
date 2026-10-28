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

        Files.write(log.toPath(),
            ("/Users/codeglue/" + GameEngineDetector.OCTARINE_SOURCE)
                .getBytes(StandardCharsets.UTF_8));
        detector.observeRuntimeLog(LatestGameLog.preview(data, LatestGameLog.PREVIEW_BYTES));
        assertEquals("Octarine, Unity (detected)",
            GameEngineDetector.merge("Unity (detected)", detector.result()));
        assertEquals(
            "Octarine (detected)", GameEngineDetector.merge(detector.result(), detector.result()));
    }

    @Test
    public void keepsLastRealBlockerAcrossGameRestartsWithoutInventingOne() throws Exception {
        File data = temporary.newFolder("persisted");
        File log = new File(data, "AnyiOSEMU_log.txt");
        assertNull(LatestGameLog.lastBlocker(data));
        Files.write(log.toPath(),
            ("normal guest output\nAnyiOSEMU finished with status 0\n")
                .getBytes(StandardCharsets.UTF_8));
        assertNull(LatestGameLog.lastBlocker(data));
        LatestGameLog.prepare(data);
        assertNull(LatestGameLog.lastBlocker(data));

        String panic = "AnyiOSEMU: PANIC in thread \"main\" at mem.rs:7: unimplemented API";
        String fatal = "AnyiOSEMU: FATAL: native host crash: SIGABRT at address 0x123";
        Files.write(log.toPath(),
            (panic + "\nbacktrace\n" + fatal + "\n").getBytes(StandardCharsets.UTF_8));
        LatestGameLog.Blocker current = LatestGameLog.lastBlocker(data);
        assertNotNull(current);
        assertFalse(current.previousSession);
        assertTrue(current.message.contains("unimplemented API"));
        assertTrue(current.message.contains("SIGABRT"));
        LatestGameLog.prepare(data);
        LatestGameLog.Blocker archived = LatestGameLog.lastBlocker(data);
        assertNotNull(archived);
        assertTrue(archived.previousSession);
        assertEquals(current.message, archived.message);
        assertEquals(current.recordedAt, archived.recordedAt);

        Files.write(log.toPath(),
            "A later normal session\nAnyiOSEMU finished with status 0\n".getBytes(
                StandardCharsets.UTF_8));
        LatestGameLog.prepare(data);
        assertEquals(archived.message, LatestGameLog.lastBlocker(data).message);
        Files.write(log.toPath(),
            "AnyiOSEMU errored: MissingFramework(QuartzCore)\n".getBytes(StandardCharsets.UTF_8));
        assertFalse(LatestGameLog.lastBlocker(data).previousSession);
        assertTrue(LatestGameLog.lastBlocker(data).message.contains("MissingFramework"));
    }

    @Test
    public void interruptedOrCancelledSessionsAreNotReportedAsErrors() throws Exception {
        File data = temporary.newFolder("interrupted");
        File log = new File(data, "AnyiOSEMU_log.txt");
        Files.write(log.toPath(),
            ("GUEST_BLOCKER: kind=Cancelled subject=user exited\n"
                + "the process was interrupted without a crash marker\n")
                .getBytes(StandardCharsets.UTF_8));
        assertNull(LatestGameLog.lastBlocker(data));
        LatestGameLog.prepare(data);
        assertNull(LatestGameLog.lastBlocker(data));
        Files.write(
            log.toPath(), "AnyiOSEMU finished with status 12\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(LatestGameLog.lastBlocker(data).message.contains("status 12"));
    }

    @Test
    public void blockerSummaryIsBoundedAndRedactsAndroidPrivatePaths() throws Exception {
        File data = temporary.newFolder("redact");
        File log = new File(data, "AnyiOSEMU_log.txt");
        String path = "/data/user/0/org.podnative/files/games/game-private/source.bin";
        Files.write(log.toPath(),
            ("AnyiOSEMU errored: " + path + " "
                + "x".repeat(4000) + "\n")
                .getBytes(StandardCharsets.UTF_8));
        LatestGameLog.Blocker blocker = LatestGameLog.lastBlocker(data);
        assertTrue(blocker.message.contains("<private path>"));
        assertFalse(blocker.message.contains(path));
        assertTrue(blocker.message.length() <= 801);
        LatestGameLog.prepare(data);
        assertEquals(blocker.message, LatestGameLog.lastBlocker(data).message);
    }

    @Test
    public void blockerScanningUsesOnlyCompleteLinesInABoundedTail() throws Exception {
        File data = temporary.newFolder("bounded");
        File log = new File(data, "AnyiOSEMU_log.txt");
        byte[] padding = new byte[LatestGameLog.PREVIEW_BYTES];
        java.util.Arrays.fill(padding, (byte) 'x');
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(log)) {
            out.write("AnyiOSEMU errored: old\n".getBytes(StandardCharsets.UTF_8));
            out.write(padding);
            out.write("\nAnyiOSEMU errored: newest\n".getBytes(StandardCharsets.UTF_8));
        }
        assertEquals("AnyiOSEMU errored: newest", LatestGameLog.lastBlocker(data).message);
    }

    @Test
    public void blockersStayIsolatedPerGameAndOversizedSidecarsAreIgnored() throws Exception {
        File first = temporary.newFolder("first-game");
        File second = temporary.newFolder("second-game");
        Files.write(new File(first, "AnyiOSEMU_log.txt").toPath(),
            "AnyiOSEMU errored: First game failure\n".getBytes(StandardCharsets.UTF_8));
        LatestGameLog.prepare(first);
        assertTrue(LatestGameLog.lastBlocker(first).message.contains("First game failure"));
        assertNull(LatestGameLog.lastBlocker(second));
        Files.write(new File(second, "AnyiOSEMU_last_blocker.txt").toPath(), new byte[4097]);
        assertNull(LatestGameLog.lastBlocker(second));
    }

    @Test
    public void logAndBlockerSymlinksAreNotFollowed() throws Exception {
        File data = temporary.newFolder("symlinks");
        File other = temporary.newFile("untouched.txt");
        Files.write(other.toPath(), "keep me".getBytes(StandardCharsets.UTF_8));
        File log = new File(data, "AnyiOSEMU_log.txt");
        try {
            Files.createSymbolicLink(log.toPath(), other.toPath());
        } catch (
            UnsupportedOperationException | SecurityException | java.io.IOException unsupported) {
            return; // Some host filesystems do not allow symlink creation.
        }
        assertThrows(java.io.IOException.class, () -> LatestGameLog.prepare(data));
        assertThrows(java.io.IOException.class, () -> LatestGameLog.preview(data, 100));
        assertThrows(
            java.io.IOException.class, () -> LatestGameLog.copy(data, new ByteArrayOutputStream()));
        assertEquals("keep me", Files.readString(other.toPath()));
        Files.delete(log.toPath());
        File sidecar = new File(data, "AnyiOSEMU_last_blocker.txt");
        Files.createSymbolicLink(sidecar.toPath(), other.toPath());
        assertThrows(java.io.IOException.class, () -> LatestGameLog.lastBlocker(data));

        File target = temporary.newFolder("parent-target");
        File realData = new File(target, "AnyiOSEMU");
        assertTrue(realData.mkdir());
        Files.write(new File(realData, "AnyiOSEMU_log.txt").toPath(),
            "AnyiOSEMU errored: private log\n".getBytes(StandardCharsets.UTF_8));
        File parentLink = new File(data, "linked-sandbox");
        Files.createSymbolicLink(parentLink.toPath(), target.toPath());
        File throughParent = new File(parentLink, "AnyiOSEMU");
        assertThrows(java.io.IOException.class, () -> LatestGameLog.lastBlocker(throughParent));
        assertThrows(java.io.IOException.class, () -> LatestGameLog.prepare(throughParent));
    }

    @Test
    public void missingAndEmptyLogsAreNotConfusedWithPreviousReports() throws Exception {
        File data = temporary.newFolder("empty");
        assertTrue(LatestGameLog.preview(data, 100).contains("No log was recorded"));
        LatestGameLog.prepare(data);
        assertThrows(
            java.io.IOException.class, () -> LatestGameLog.copy(data, new ByteArrayOutputStream()));
    }
}
