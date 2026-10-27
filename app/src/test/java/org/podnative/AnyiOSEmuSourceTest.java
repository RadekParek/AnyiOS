package org.podnative;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

import android.app.Application;
import java.io.File;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class AnyiOSEmuSourceTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void usesConverterRecoveredIconWhenNoCoverWasFound() throws Exception {
        File gameDirectory = temporary.newFolder("game-icon-fallback");
        File recoveredIcon = new File(gameDirectory, "icon.png");
        assertEquals(true, recoveredIcon.createNewFile());
        GameLibrary.Game game = new GameLibrary.Game(gameDirectory,
            new JSONObject().put("kind", "IPA"));

        assertEquals(recoveredIcon, game.cover());
        File cover = new File(gameDirectory, "cover.png");
        assertEquals(true, cover.createNewFile());
        assertEquals(cover, game.cover());
    }

    @Test
    public void universalArm32AndArm64GameIsLaunchableThroughItsArm32Slice() throws Exception {
        File gameDirectory = temporary.newFolder("game-universal");
        GameLibrary.Game game = new GameLibrary.Game(gameDirectory,
            new JSONObject().put("kind", "IPA")
                .put("architecture", "ARM64 + ARM32 universal (ARM32 selected: ARMv7)"));

        assertEquals(true, game.canLaunchInAnyiOSEMU());
        assertEquals("ARM64 + ARM32", game.architectureChip());
    }

    @Test
    public void olderUniversalLabelsStillShowArm64FirstInTheChip() throws Exception {
        File directory = temporary.newFolder("game-old-universal");
        GameLibrary.Game game = new GameLibrary.Game(directory,
            new JSONObject().put("kind", "IPA").put("architecture", "ARMv7 · ARM32 + ARM64"));
        assertEquals("ARM64 + ARM32", game.architectureChip());
    }

    @Test
    public void negativeArm64NoteDoesNotBecomeAPositiveBadge() throws Exception {
        File directory = temporary.newFolder("game-arm32-universal");
        GameLibrary.Game game = new GameLibrary.Game(directory,
            new JSONObject().put("kind", "IPA").put("architecture",
                "ARMv7 · ARM32 universal (no ARM64 slice detected)"));
        assertEquals(true, game.canLaunchInAnyiOSEMU());
        assertEquals("ARM32", game.architectureChip());
        GameLibrary.Game old = new GameLibrary.Game(directory,
            new JSONObject().put("kind", "IPA").put("architecture",
                "ARMv7 · ARM32 universal - no ARM64 slice detected"));
        assertEquals("ARM32", old.architectureChip());
    }

    @Test
    public void arm64_32IsNotMisrepresentedAsAnArm64Slice() throws Exception {
        File directory = temporary.newFolder("game-arm64-32");
        GameLibrary.Game game = new GameLibrary.Game(directory,
            new JSONObject().put("kind", "IPA").put("architecture",
                "ARM64_32 + ARM32 universal (ARM32 selected: ARMv7)"));
        assertEquals(true, game.canLaunchInAnyiOSEMU());
        assertEquals("ARM64_32 + ARM32", game.architectureChip());
        GameLibrary.Game both = new GameLibrary.Game(directory,
            new JSONObject().put("kind", "IPA").put("architecture",
                "ARM64 + ARM64_32 + ARM32 universal (ARM32 selected: ARMv7)"));
        assertEquals("ARM64 + ARM64_32 + ARM32", both.architectureChip());
    }

    @Test
    public void supportedArmV7fSliceIsLaunchable() throws Exception {
        File gameDirectory = temporary.newFolder("game-armv7f");
        GameLibrary.Game game = new GameLibrary.Game(gameDirectory,
            new JSONObject().put("kind", "Folder").put("architecture", "ARMv7f · ARM32"));

        assertEquals(true, game.canLaunchInAnyiOSEMU());
    }

    @Test
    public void arm64OnlyAndPreviouslyBlockedEntriesAreNotLaunchable() throws Exception {
        File arm64Directory = temporary.newFolder("game-arm64");
        GameLibrary.Game arm64 = new GameLibrary.Game(arm64Directory,
            new JSONObject().put("kind", "IPA").put("architecture", "ARM64")
                .put("status", "Unsupported"));
        File staleDirectory = temporary.newFolder("game-stale-unsupported");
        GameLibrary.Game stale = new GameLibrary.Game(staleDirectory,
            new JSONObject().put("kind", "IPA").put("architecture", "ARM64")
                .put("status", "Imported"));
        File legacyDirectory = temporary.newFolder("game-armv4t");
        GameLibrary.Game legacy = new GameLibrary.Game(legacyDirectory,
            new JSONObject().put("kind", "IPA").put("architecture", "ARMv4T")
                .put("status", "Imported"));

        assertEquals(false, arm64.canLaunchInAnyiOSEMU());
        assertEquals(false, stale.canLaunchInAnyiOSEMU());
        assertEquals(false, legacy.canLaunchInAnyiOSEMU());
    }

    @Test
    public void resolvesNestedExtractedAppBundleInsteadOfItsParentFolder() throws Exception {
        File gameDirectory = temporary.newFolder("game-123");
        File importRoot = new File(gameDirectory, "bundle");
        File appBundle = new File(importRoot, "Payload/Example.app");
        assertEquals(true, appBundle.mkdirs());
        assertEquals(true, new File(appBundle, "Info.plist").createNewFile());
        assertEquals(true, new File(appBundle, "Example").createNewFile());

        GameLibrary.Game game = new GameLibrary.Game(gameDirectory,
            new JSONObject().put("kind", "Folder").put("executable", "Payload/Example.app/Example"));

        assertEquals(appBundle.getCanonicalFile(), game.anyioseSource().getCanonicalFile());
        assertNotEquals(importRoot.getCanonicalFile(), game.anyioseSource().getCanonicalFile());
    }

    @Test
    public void refusesExecutableMetadataThatEscapesTheImportedFolder() throws Exception {
        File gameDirectory = temporary.newFolder("game-456");
        File importRoot = new File(gameDirectory, "bundle");
        assertEquals(true, importRoot.mkdirs());
        File outside = new File(temporary.getRoot(), "Outside.app");
        assertEquals(true, outside.mkdirs());
        assertEquals(true, new File(outside, "Info.plist").createNewFile());

        GameLibrary.Game game = new GameLibrary.Game(gameDirectory,
            new JSONObject().put("kind", "Folder").put("executable", "../../Outside.app/Outside"));

        assertEquals(importRoot.getCanonicalFile(), game.anyioseSource().getCanonicalFile());
    }
}
