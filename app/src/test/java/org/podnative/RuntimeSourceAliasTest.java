package org.podnative;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.app.Application;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.concurrent.CancellationException;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class RuntimeSourceAliasTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void copiesIpaForRuntimeWhenHardLinksAreUnavailable() throws Exception {
        File game = temporary.newFolder("game");
        File source = new File(game, "source.bin");
        byte[] original = "synthetic IPA bytes".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        Files.write(source.toPath(), original);
        File alias = new File(game, "AnyiOSEMU_source.ipa");
        File pending = new File(game, ".AnyiOSEMU_source.ipa.pending");
        File marker = new File(game, ".AnyiOSEMU_source.ipa.meta");

        File prepared = GameLibrary.copyRuntimeIpaAlias(source, alias, pending, marker,
            source.length() + 64L * 1024 * 1024, () -> {}, bytes -> {});

        assertEquals(alias, prepared);
        assertArrayEquals(original, Files.readAllBytes(alias.toPath()));
        assertTrue(marker.isFile());
        assertFalse(Files.isSameFile(source.toPath(), alias.toPath()));
        assertFalse(pending.exists());
    }

    @Test
    public void cancellationRemovesThePartialCopyAndPreservesTheOriginal() throws Exception {
        File game = temporary.newFolder("cancelled-copy");
        File source = new File(game, "source.bin");
        byte[] original = new byte[1024];
        Files.write(source.toPath(), original);
        File alias = new File(game, "AnyiOSEMU_source.ipa");
        File pending = new File(game, ".AnyiOSEMU_source.ipa.pending");
        File marker = new File(game, ".AnyiOSEMU_source.ipa.meta");

        try {
            GameLibrary.copyRuntimeIpaAlias(source, alias, pending, marker,
                source.length() + 64L * 1024 * 1024,
                () -> { throw new CancellationException("test cancellation"); }, bytes -> {});
            fail("Expected the copy to be cancelled");
        } catch (CancellationException expected) {
            assertTrue(expected.getMessage().contains("test cancellation"));
        }

        assertArrayEquals(original, Files.readAllBytes(source.toPath()));
        assertFalse(alias.exists());
        assertFalse(pending.exists());
        assertFalse(marker.exists());
    }

    @Test
    public void refusesFallbackCopyWhenTheDeviceHasInsufficientFreeSpace() throws Exception {
        File game = temporary.newFolder("full-device");
        File source = new File(game, "source.bin");
        Files.write(source.toPath(), new byte[] {1, 2, 3, 4});
        File alias = new File(game, "AnyiOSEMU_source.ipa");
        File pending = new File(game, ".AnyiOSEMU_source.ipa.pending");
        File marker = new File(game, ".AnyiOSEMU_source.ipa.meta");

        try {
            GameLibrary.copyRuntimeIpaAlias(
                source, alias, pending, marker, 0, () -> {}, bytes -> {});
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("Free device storage"));
            assertFalse(alias.exists());
            assertFalse(pending.exists());
            return;
        }
        throw new AssertionError("Expected insufficient-space failure");
    }
}
