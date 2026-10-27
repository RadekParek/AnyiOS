package org.podnative;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class GameInspectorArchiveTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    private File ipa(String name, String... entries) throws Exception {
        File archive = new File(temporary.getRoot(), name);
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(archive))) {
            for (int index = 0; index < entries.length; index += 2) {
                zip.putNextEntry(new ZipEntry(entries[index]));
                zip.write(entries[index + 1].getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
        }
        return archive;
    }

    @Test
    public void indexesTheSingleAppBundleWithoutExtractingIt() throws Exception {
        File source = ipa("Game.ipa", "Payload/Angry.app/Info.plist", "plist",
            "Payload/Angry.app/AngryBirds", "mach-o", "Payload/Angry.app/data/atlas.pvr",
            "texture-data", "iTunesMetadata.plist", "outside");
        try (ZipFile zip = new ZipFile(source)) {
            GameInspector.ArchiveEntries entries = GameInspector.listArchiveEntries(zip);
            assertEquals("Payload/Angry.app/", entries.bundlePrefix);
            assertTrue(entries.names.contains("Payload/Angry.app/Info.plist"));
            assertTrue(entries.names.contains("Payload/Angry.app/data/atlas.pvr"));
            assertFalse(new File(temporary.getRoot(), "Angry.app").exists());
        }
        assertTrue("the imported IPA remains available as the resource source", source.isFile());
    }

    @Test
    public void readsRequirementsFromExistingIpaAndNestedAppFolder() throws Exception {
        String xml = "<plist><dict><key>MinimumOSVersion</key><string>4.3</string>"
            + "<key>UIRequiredDeviceCapabilities</key><array><string>gyroscope</string>"
            + "</array><key>UIDeviceFamily</key><array><integer>2</integer></array></dict></plist>";
        File source = ipa("requirements.ipa", "Payload/Game.app/Info.plist", xml,
            "Payload/Game.app/Game", "mach-o");
        GameRequirements fromIpa = GameInspector.readRequirements(source, "IPA");
        assertEquals("4.3", fromIpa.minimumIos);
        assertEquals("iPad", fromIpa.deviceFamilies);
        assertEquals("Gyroscope", fromIpa.required);
        File folder = temporary.newFolder("nested");
        File bundle = new File(folder, "Payload/Game.app");
        assertTrue(bundle.mkdirs());
        java.nio.file.Files.writeString(new File(bundle, "Info.plist").toPath(), xml);
        assertEquals(fromIpa.description(), GameInspector.readRequirements(folder, "Folder").description());
        assertEquals("Unavailable", GameInspector.readRequirements(source, "Executable").minimumIos);
    }

    @Test
    public void oversizedAndInvalidRequirementsDoNotLookLikeEmptyDeclarations() throws Exception {
        File invalid = ipa("invalid.ipa", "Payload/Game.app/Info.plist", "not a plist");
        org.junit.Assert.assertThrows(IOException.class,
            () -> GameInspector.readRequirements(invalid, "IPA"));
        File large = ipa("large.ipa", "Payload/Game.app/Info.plist", "x".repeat(1024 * 1024 + 1));
        org.junit.Assert.assertThrows(IOException.class,
            () -> GameInspector.readRequirements(large, "IPA"));
    }

    @Test
    public void rejectsMultipleAppBundlesAsAmbiguous() throws Exception {
        File source = ipa("TwoApps.ipa", "Payload/One.app/Info.plist", "one",
            "Payload/Two.app/Info.plist", "two");
        try (ZipFile zip = new ZipFile(source)) {
            try {
                GameInspector.listArchiveEntries(zip);
                fail("an ambiguous IPA must not be indexed as a game");
            } catch (IOException expected) {
                assertTrue(expected.getMessage().contains("one top-level app"));
            }
        }
    }

    @Test
    public void rejectsArchivesWithoutATopLevelPayloadApp() throws Exception {
        File source = ipa("NoApp.ipa", "Payload/Assets.car", "assets",
            "Payload/Framework.framework/Framework", "framework");
        try (ZipFile zip = new ZipFile(source)) {
            try {
                GameInspector.listArchiveEntries(zip);
                fail("an IPA without an app bundle must not be indexed");
            } catch (IOException expected) {
                assertTrue(expected.getMessage().contains("one top-level app"));
            }
        }
    }

    @Test
    public void listsArm64BeforeArm32ButRetainsArm32Selection() {
        assertEquals("ARM64 + ARM32",
            GameInspector.universalArchitectureSlices(new long[] {0, 0, 0, 1, 1, 0}));
        assertEquals(
            "ARM32", GameInspector.universalArchitectureSlices(new long[] {0, 0, 0, 1, 0, 0}));
    }
}
