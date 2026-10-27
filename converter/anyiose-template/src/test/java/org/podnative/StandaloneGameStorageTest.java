package org.podnative;

import static org.junit.Assert.*;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class StandaloneGameStorageTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private static final String PACKAGE = "dev.anyiose.game.p123";
    private static final byte[] GAME = "authorized IPA ZIP fixture".getBytes(java.nio.charset.StandardCharsets.UTF_8);

    private static String sha(byte[] value) throws Exception {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(value))
            hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return hex.toString();
    }

    @Test public void createsVerifiedPrivateGameAndRealExpansionMirror() throws Exception {
        File data = temporary.newFolder("private");
        File obb = new File(temporary.getRoot(), "Android/obb/" + PACKAGE);
        AtomicInteger opens = new AtomicInteger();
        StandaloneGameStorage.AssetSource asset = () -> {
            opens.incrementAndGet();
            return new ByteArrayInputStream(GAME);
        };
        File privateGame = StandaloneGameStorage.prepare(data, obb, PACKAGE, GAME.length, sha(GAME), asset);
        assertEquals(new File(data, "game.ipa"), privateGame);
        File expansion = new File(obb, "main.1." + PACKAGE + ".obb");
        assertArrayEquals(GAME, Files.readAllBytes(privateGame.toPath()));
        assertArrayEquals(GAME, Files.readAllBytes(expansion.toPath()));
        assertEquals(1, opens.get());

        // A repeated launch reuses the files without reopening the APK asset.
        StandaloneGameStorage.prepare(data, obb, PACKAGE, GAME.length, sha(GAME), asset);
        assertEquals(1, opens.get());
        // A corrupt mirror is repaired from the already-verified private IPA.
        Files.write(expansion.toPath(), new byte[] {1, 2, 3});
        StandaloneGameStorage.prepare(data, obb, PACKAGE, GAME.length, sha(GAME), asset);
        assertArrayEquals(GAME, Files.readAllBytes(expansion.toPath()));
        assertEquals(1, opens.get());
    }

    @Test public void unavailableExpansionStorageKeepsPrivateGameAndSaves() throws Exception {
        File data = temporary.newFolder("private-fallback");
        File save = new File(data, "save.dat");
        Files.write(save.toPath(), new byte[] {42});
        File unavailable = temporary.newFile("not-a-directory");
        File privateGame = StandaloneGameStorage.prepare(data, unavailable, PACKAGE, GAME.length,
                sha(GAME), () -> new ByteArrayInputStream(GAME));
        assertArrayEquals(GAME, Files.readAllBytes(privateGame.toPath()));
        assertArrayEquals(new byte[] {42}, Files.readAllBytes(save.toPath()));
    }

    @Test public void brokenAssetDoesNotReplaceAnExistingFileOrLeavePendingData() throws Exception {
        File data = temporary.newFolder("broken");
        File old = new File(data, "game.ipa");
        Files.write(old.toPath(), new byte[] {9});
        assertThrows(java.io.IOException.class, () -> StandaloneGameStorage.prepare(data, null, PACKAGE,
                GAME.length, sha(GAME), () -> new ByteArrayInputStream(new byte[] {1, 2})));
        assertArrayEquals(new byte[] {9}, Files.readAllBytes(old.toPath()));
        assertFalse(new File(data, ".game.ipa.pending").exists());
    }

    @Test public void corruptPrivateCopyIsReplacedFromTheVerifiedAsset() throws Exception {
        File data = temporary.newFolder("repair-private");
        Files.write(new File(data, "game.ipa").toPath(), new byte[] {7});
        AtomicInteger opens = new AtomicInteger();
        File game = StandaloneGameStorage.prepare(data, null, PACKAGE, GAME.length, sha(GAME), () -> {
            opens.incrementAndGet();
            return new ByteArrayInputStream(GAME);
        });
        assertEquals(1, opens.get());
        assertArrayEquals(GAME, Files.readAllBytes(game.toPath()));
    }

    @Test public void refusesSymlinkedExpansionWithoutChangingItsTarget() throws Exception {
        File data = temporary.newFolder("symlink-private");
        File obb = temporary.newFolder("symlink-obb");
        File victim = temporary.newFile("victim");
        Files.write(victim.toPath(), new byte[] {42});
        try {
            Files.createSymbolicLink(new File(obb, "main.1." + PACKAGE + ".obb").toPath(), victim.toPath());
        } catch (UnsupportedOperationException | java.io.IOException | SecurityException noLinks) {
            org.junit.Assume.assumeNoException(noLinks);
        }
        File game = StandaloneGameStorage.prepare(data, obb, PACKAGE, GAME.length, sha(GAME),
                () -> new ByteArrayInputStream(GAME));
        assertArrayEquals(GAME, Files.readAllBytes(game.toPath()));
        assertArrayEquals(new byte[] {42}, Files.readAllBytes(victim.toPath()));
    }

    @Test public void refusesAnObbDirectorySymlink() throws Exception {
        File data = temporary.newFolder("link-parent-private");
        File actual = temporary.newFolder("real-obb");
        File link = new File(temporary.getRoot(), "linked-obb");
        try {
            Files.createSymbolicLink(link.toPath(), actual.toPath());
        } catch (UnsupportedOperationException | java.io.IOException | SecurityException noLinks) {
            org.junit.Assume.assumeNoException(noLinks);
        }
        File game = StandaloneGameStorage.prepare(data, link, PACKAGE, GAME.length, sha(GAME),
                () -> new ByteArrayInputStream(GAME));
        assertArrayEquals(GAME, Files.readAllBytes(game.toPath()));
        assertEquals(0, actual.list().length);
    }

    @Test public void invalidNamesAndDigestsAreRejected() throws Exception {
        assertThrows(IllegalArgumentException.class, () -> StandaloneGameStorage.obbName("../bad"));
        File data = temporary.newFolder("bad-identity");
        assertThrows(java.io.IOException.class, () -> StandaloneGameStorage.prepare(data, null, PACKAGE,
                GAME.length, "not-a-hash", () -> new ByteArrayInputStream(GAME)));
    }
}
