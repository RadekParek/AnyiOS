package org.podnative;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** The runtime's per-game file, including output retained after an interrupted session. */
final class LatestGameLog {
    static final int PREVIEW_BYTES = 512 * 1024;
    private static final String FILE_NAME = "AnyiOSEMU_log.txt";

    private LatestGameLog() {}

    private static File file(File dataDirectory) throws IOException {
        File log = new File(dataDirectory, FILE_NAME);
        if (Files.isSymbolicLink(log.toPath()) || (log.exists() && !log.isFile()))
            throw new IOException("The game log path is not a regular file");
        return log;
    }

    /** Clear the prior run before starting the activity, even if startup later fails. */
    static void prepare(File dataDirectory) throws IOException {
        if (!dataDirectory.isDirectory())
            throw new IOException("The private game data directory is unavailable");
        try (FileOutputStream ignored = new FileOutputStream(file(dataDirectory), false)) {
            // A zero-length file distinguishes a failed startup from a previous game's log.
        }
    }

    static String preview(File dataDirectory, int limit) throws IOException {
        if (limit <= 0)
            throw new IllegalArgumentException("Log preview limit must be positive");
        File log = file(dataDirectory);
        if (!log.isFile() || log.length() == 0)
            return "No log was recorded for this game's most recent launch.";
        try (RandomAccessFile input = new RandomAccessFile(log, "r")) {
            long length = input.length();
            long start = Math.max(0L, length - limit);
            input.seek(start);
            byte[] bytes = new byte[(int) (length - start)];
            input.readFully(bytes);
            int offset = 0;
            // If the bounded tail starts inside a UTF-8 code point, drop its continuation bytes.
            if (start > 0)
                while (offset < bytes.length && (bytes[offset] & 0xc0) == 0x80) offset++;
            return (start > 0 ? "[Earlier log output omitted. Export to read the full log.]\n" : "")
                + new String(bytes, offset, bytes.length - offset, StandardCharsets.UTF_8);
        }
    }

    static void copy(File dataDirectory, OutputStream output) throws IOException {
        File log = file(dataDirectory);
        if (!log.isFile() || log.length() == 0)
            throw new IOException("No log was recorded for this game's most recent launch");
        try (FileInputStream input = new FileInputStream(log)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
        }
    }
}
