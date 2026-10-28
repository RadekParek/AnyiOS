package org.podnative;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.SeekableByteChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** The runtime's per-game file, including output retained after an interrupted session. */
final class LatestGameLog {
    static final int PREVIEW_BYTES = 512 * 1024;
    private static final int SUMMARY_CHARS = 800;
    private static final String FILE_NAME = "AnyiOSEMU_log.txt";
    private static final String BLOCKER_FILE_NAME = "AnyiOSEMU_last_blocker.txt";
    private static final Pattern EXIT_STATUS =
        Pattern.compile("AnyiOSEMU finished with status (-?\\d+)");
    private static final Pattern ANDROID_PRIVATE_PATH =
        Pattern.compile("(?i)(?:/data/(?:user(?:_de)?/[0-9]+|data)/|/storage/emulated/[0-9]+/)"
            + "[^\\s\\\"'<>]+");

    static final class Blocker {
        final String message;
        final long recordedAt;
        final boolean previousSession;
        Blocker(String message, long recordedAt, boolean previousSession) {
            this.message = message;
            this.recordedAt = recordedAt;
            this.previousSession = previousSession;
        }
    }

    private LatestGameLog() {}

    private static File checkedFile(File dataDirectory, String name) throws IOException {
        File parent = dataDirectory.getParentFile();
        if (Files.isSymbolicLink(dataDirectory.toPath())
            || (parent != null && Files.isSymbolicLink(parent.toPath())))
            throw new IOException("The game data directory is not a private directory");
        File file = new File(dataDirectory, name);
        if (Files.isSymbolicLink(file.toPath()) || (file.exists() && !file.isFile()))
            throw new IOException("The game log path is not a regular file");
        return file;
    }

    private static File file(File dataDirectory) throws IOException {
        return checkedFile(dataDirectory, FILE_NAME);
    }

    /** Archive an actual error before clearing the prior run; never fabricate a blocker. */
    static void prepare(File dataDirectory) throws IOException {
        if (!dataDirectory.isDirectory())
            throw new IOException("The private game data directory is unavailable");
        File log = file(dataDirectory);
        // A sidecar write failure should not prevent a game from launching. The
        // current log is still available until the runtime resets it.
        try {
            Blocker last = currentBlocker(dataDirectory, log);
            if (last != null)
                saveBlocker(dataDirectory, last);
        } catch (IOException | SecurityException ignored) {
            // The normal log preparation below still reports its own errors.
        }
        try (OutputStream ignored = Files.newOutputStream(log.toPath(), StandardOpenOption.CREATE,
                 StandardOpenOption.WRITE, StandardOpenOption.TRUNCATE_EXISTING,
                 LinkOption.NOFOLLOW_LINKS)) {
            // A zero-length file distinguishes a failed startup from a previous game's log.
        }
    }

    static long lastLogTime(File dataDirectory) throws IOException {
        File log = file(dataDirectory);
        return log.isFile() && log.length() > 0 ? log.lastModified() : 0L;
    }

    /** Most recent explicit runtime error, including a saved earlier session if needed. */
    static Blocker lastBlocker(File dataDirectory) throws IOException {
        Blocker current = currentBlocker(dataDirectory, file(dataDirectory));
        if (current != null)
            return current;
        File saved = checkedFile(dataDirectory, BLOCKER_FILE_NAME);
        if (!saved.isFile())
            return null;
        String content;
        try (SeekableByteChannel input = Files.newByteChannel(
                 saved.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long length = input.size();
            if (length <= 0 || length > 4096)
                return null;
            ByteBuffer bytes = ByteBuffer.allocate((int) length);
            while (bytes.hasRemaining() && input.read(bytes) > 0) { /* bounded snapshot */
            }
            content = new String(bytes.array(), 0, bytes.position(), StandardCharsets.UTF_8);
        }
        int separator = content.indexOf('\n');
        if (separator <= 0 || separator == content.length() - 1)
            return null;
        try {
            long recordedAt = Long.parseLong(content.substring(0, separator));
            String message = sanitize(content.substring(separator + 1), dataDirectory);
            return message.isEmpty() ? null : new Blocker(message, recordedAt, true);
        } catch (NumberFormatException ignored) {
            return null;
        }
    }

    private static Blocker currentBlocker(File dataDirectory, File log) throws IOException {
        if (!log.isFile() || log.length() == 0)
            return null;
        String text = readTail(log, PREVIEW_BYTES, false);
        String candidate = "";
        String lastCause = "";
        int causeLine = -1000;
        int lineNumber = 0;
        for (int start = 0; start < text.length();) {
            int end = text.indexOf('\n', start);
            if (end < 0)
                end = text.length();
            String line = text.substring(start, end).trim();
            start = end + 1;
            lineNumber++;
            boolean panic = line.contains("AnyiOSEMU: PANIC in thread");
            boolean error = line.contains("AnyiOSEMU errored:");
            boolean guestBlocker = line.startsWith("GUEST_BLOCKER:")
                && !line.contains("kind=Cancelled") && !line.contains("kind=Guest exited")
                && !line.contains("kind=Time slice");
            boolean launchBlocker = line.startsWith("LAUNCH_BLOCKED:");
            boolean fatal = line.contains("AnyiOSEMU: FATAL: native host crash")
                || line.contains("AnyiOSEMU: FATAL host exception")
                || line.contains("AnyiOSEMU: FATAL SIGNAL") || line.contains("AnyiOSEMU: fatal SIG")
                || line.contains("AnyiOSEMU: fatal signal");
            Matcher status = EXIT_STATUS.matcher(line);
            boolean unsuccessful = status.find() && !status.group(1).equals("0");
            if (panic || error || guestBlocker || launchBlocker) {
                lastCause = line;
                causeLine = lineNumber;
                candidate = line;
            } else if (fatal || unsuccessful) {
                // A signal/exit code is not a root cause. Keep a nearby recorded
                // panic as well rather than replacing it with the generic signal.
                candidate = !lastCause.isEmpty() && lineNumber - causeLine <= 32
                    ? lastCause + "\n" + line
                    : line;
            }
        }
        String summary = sanitize(candidate, dataDirectory);
        return summary.isEmpty() ? null : new Blocker(summary, log.lastModified(), false);
    }

    private static String sanitize(String text, File dataDirectory) {
        String clean = text.replace(dataDirectory.getAbsolutePath(), "<private game data>")
                           .replace('\0', ' ')
                           .replace('\r', ' ')
                           .trim();
        clean = ANDROID_PRIVATE_PATH.matcher(clean).replaceAll("<private path>");
        if (clean.length() > SUMMARY_CHARS)
            clean = clean.substring(0, SUMMARY_CHARS) + "…";
        return clean;
    }

    private static void saveBlocker(File dataDirectory, Blocker blocker) throws IOException {
        File destination = checkedFile(dataDirectory, BLOCKER_FILE_NAME);
        File temporary = File.createTempFile(".last-blocker-", ".tmp", dataDirectory);
        try {
            byte[] bytes =
                (blocker.recordedAt + "\n" + blocker.message).getBytes(StandardCharsets.UTF_8);
            try (FileChannel output = FileChannel.open(
                     temporary.toPath(), StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
                ByteBuffer buffer = ByteBuffer.wrap(bytes);
                while (buffer.hasRemaining()) output.write(buffer);
                output.force(true);
            }
            try {
                Files.move(temporary.toPath(), destination.toPath(),
                    StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException ignored) {
                Files.move(
                    temporary.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }

    private static String readTail(File log, int limit, boolean describeOmission)
        throws IOException {
        try (SeekableByteChannel input = Files.newByteChannel(
                 log.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long length = input.size();
            long start = Math.max(0L, length - limit);
            input.position(start);
            ByteBuffer bytes = ByteBuffer.allocate((int) (length - start));
            // The runtime can truncate or replace a log while the UI reads it.
            // Keep the bytes actually read instead of allocating again or failing.
            while (bytes.hasRemaining() && input.read(bytes) > 0) { /* bounded snapshot */
            }
            int end = bytes.position();
            byte[] data = bytes.array();
            int offset = 0;
            if (start > 0) {
                // Drop an incomplete first codepoint; blocker detection also
                // drops the partial first line to avoid recognizing a suffix.
                while (offset < end && (data[offset] & 0xc0) == 0x80) offset++;
                if (!describeOmission) {
                    while (offset < end && data[offset] != '\n') offset++;
                    if (offset < end)
                        offset++;
                }
            }
            return (start > 0 && describeOmission
                           ? "[Earlier log output omitted. Export to read the full log.]\n"
                           : "")
                + new String(data, offset, end - offset, StandardCharsets.UTF_8);
        }
    }

    static String preview(File dataDirectory, int limit) throws IOException {
        if (limit <= 0)
            throw new IllegalArgumentException("Log preview limit must be positive");
        File log = file(dataDirectory);
        if (!log.isFile() || log.length() == 0)
            return "No log was recorded for this game's most recent launch.";
        return readTail(log, limit, true);
    }

    static void copy(File dataDirectory, OutputStream output) throws IOException {
        File log = file(dataDirectory);
        if (!log.isFile() || log.length() == 0)
            throw new IOException("No log was recorded for this game's most recent launch");
        try (InputStream input = Files.newInputStream(
                 log.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            byte[] buffer = new byte[65536];
            int read;
            while ((read = input.read(buffer)) != -1) output.write(buffer, 0, read);
        }
    }
}
