package org.podnative;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Provision a verified private IPA for the game and an optional Android OBB
 * mirror. The signed APK still contains the complete source; shared storage is
 * never trusted for executable code and is not required for the game or saves.
 * Android expansion files may be arbitrary formats and use the standard name:
 * https://developer.android.com/google/play/expansion-files
 */
final class StandaloneGameStorage {
    interface AssetSource { InputStream open() throws IOException; }

    private StandaloneGameStorage() {}

    static String obbName(String packageName) {
        if (packageName == null || !packageName.matches("[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+"))
            throw new IllegalArgumentException("Invalid Android package name");
        // The standalone template's android:versionCode is 1.
        return "main.1." + packageName + ".obb";
    }

    static File prepare(File privateDirectory, File obbDirectory, String packageName,
            long expectedBytes, String expectedSha256, AssetSource source) throws Exception {
        if (expectedBytes <= 0 || expectedSha256 == null || !expectedSha256.matches("[0-9a-f]{64}"))
            throw new IOException("Invalid game identity");
        File privateGame = new File(privateDirectory, "game.ipa");
        if (!validFile(privateGame, expectedBytes, expectedSha256))
            writeVerified(privateGame, expectedBytes, expectedSha256, source);
        if (obbDirectory != null) {
            try {
                if (Files.isSymbolicLink(obbDirectory.toPath()))
                    throw new IOException("Unsafe OBB directory");
                if (!obbDirectory.isDirectory() && !obbDirectory.mkdirs() && !obbDirectory.isDirectory())
                    throw new IOException("Cannot create the app's OBB directory");
                File expansion = new File(obbDirectory, obbName(packageName));
                if (!expansion.exists() && !Files.isSymbolicLink(expansion.toPath())) {
                    // The bundle ID can also belong to another installed app or
                    // runtime variant. Never replace an existing OBB: it may be
                    // their data. The signed, private IPA remains authoritative.
                    writeVerified(expansion, expectedBytes, expectedSha256,
                            () -> new FileInputStream(privateGame), false);
                }
            } catch (IOException | RuntimeException unavailable) {
                // Shared storage can be absent, read-only, full or restricted.
            }
        }
        return privateGame;
    }

    private static boolean validFile(File file, long bytes, String digest) throws Exception {
        return !Files.isSymbolicLink(file.toPath()) && file.isFile() && file.length() == bytes
                && sha256(file).equals(digest);
    }

    private static void writeVerified(File target, long expectedBytes, String expectedSha256,
            AssetSource source) throws Exception {
        writeVerified(target, expectedBytes, expectedSha256, source, true);
    }

    private static void writeVerified(File target, long expectedBytes, String expectedSha256,
            AssetSource source, boolean replace) throws Exception {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()))
            throw new IOException("Game storage directory is unavailable");
        if (Files.isSymbolicLink(target.toPath())
                || !parent.getCanonicalFile().equals(target.getCanonicalFile().getParentFile()))
            throw new IOException("Unsafe game storage path");
        File pending = new File(parent, "." + target.getName() + ".pending");
        if (Files.isSymbolicLink(pending.toPath())) throw new IOException("Unsafe pending game path");
        if (pending.exists() && !pending.delete()) throw new IOException("Cannot remove incomplete game file");
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long total = 0;
            try (InputStream input = source.open();
                 FileChannel channel = FileChannel.open(pending.toPath(), StandardOpenOption.CREATE_NEW,
                         StandardOpenOption.WRITE);
                 java.io.OutputStream output = Channels.newOutputStream(channel)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    total += count;
                    if (total > expectedBytes) throw new IOException("Game payload is too large");
                    digest.update(buffer, 0, count);
                    output.write(buffer, 0, count);
                }
                output.flush();
                channel.force(true);
            }
            if (total != expectedBytes || !hex(digest.digest()).equals(expectedSha256))
                throw new IOException("Game payload failed its integrity check");
            if (!replace) {
                // No REPLACE_EXISTING (nor ATOMIC_MOVE, whose replacement
                // semantics vary by provider): a competing file wins safely.
                Files.move(pending.toPath(), target.toPath());
            } else {
                try {
                    Files.move(pending.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                            StandardCopyOption.REPLACE_EXISTING);
                } catch (AtomicMoveNotSupportedException unsupported) {
                    Files.move(pending.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
            }
        } finally {
            pending.delete();
        }
    }

    private static String sha256(File file) throws IOException, NoSuchAlgorithmException {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return hex(digest.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(String.format(Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
}
