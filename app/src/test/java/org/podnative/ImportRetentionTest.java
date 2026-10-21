package org.podnative;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.file.Files;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class ImportRetentionTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    @Test
    public void arm64BundleKeepsMetadataAndArtworkWithoutNativeExecution() throws Exception {
        File folder = temporary.newFolder("Game.app");
        Files.write(new File(folder, "Info.plist").toPath(),
            "<plist><dict><key>CFBundleDisplayName</key><string>Owned 64-bit game</string><key>CFBundleExecutable</key><string>Game</string></dict></plist>"
                .getBytes(java.nio.charset.StandardCharsets.UTF_8));
        byte[] header = new byte[32];
        header[0] = (byte) 0xcf;
        header[1] = (byte) 0xfa;
        header[2] = (byte) 0xed;
        header[3] = (byte) 0xfe;
        header[4] = 12;
        header[7] = 1;
        Files.write(new File(folder, "Game").toPath(), header);
        Files.write(new File(folder, "Icon.png").toPath(), new byte[] {1, 2});
        Files.write(new File(folder, "Default.png").toPath(), new byte[] {3, 4});
        var result = GameInspector.analyze(folder, "Game.app", temporary.getRoot());
        assertEquals("Owned 64-bit game", result.title);
        assertEquals("ARM64", result.architecture);
        assertEquals("Unsupported", result.status);
        assertFalse(result.blockReason.isEmpty());
        assertNotNull(result.icon);
        assertEquals(1, result.splashImages.size());
        assertTrue(new File(folder, "Game").exists());
    }
    @Test
    public void arm64IpaRetainsIconAndOriginalBytes() throws Exception {
        File ipa = temporary.newFile("Universal64.ipa");
        byte[] header = new byte[32];
        header[0] = (byte) 0xcf;
        header[1] = (byte) 0xfa;
        header[2] = (byte) 0xed;
        header[3] = (byte) 0xfe;
        header[4] = 12;
        header[7] = 1;
        try (var zip = new java.util.zip.ZipOutputStream(new java.io.FileOutputStream(ipa))) {
            String[] names = {"Info.plist", "Game", "Icon.png", "Default.png"};
            byte[][] data = {
                "<plist><dict><key>CFBundleExecutable</key><string>Game</string><key>CFBundleDisplayName</key><string>IPA metadata</string></dict></plist>"
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8),
                header, new byte[] {1, 2}, new byte[] {3, 4}};
            for (int i = 0; i < names.length; i++) {
                zip.putNextEntry(new java.util.zip.ZipEntry("Payload/Game.app/" + names[i]));
                zip.write(data[i]);
                zip.closeEntry();
            }
        }
        byte[] original = Files.readAllBytes(ipa.toPath());
        var result = GameInspector.analyze(ipa, "Universal64.ipa", temporary.getRoot());
        assertEquals("IPA", result.kind);
        assertEquals("IPA metadata", result.title);
        assertEquals("Unsupported", result.status);
        assertNotNull(result.icon);
        assertEquals(1, result.splashImages.size());
        assertArrayEquals(original, Files.readAllBytes(ipa.toPath()));
    }
    @Test
    public void missingExecutableAndCorruptIpaRemainInspectable() throws Exception {
        File folder = temporary.newFolder("Broken.app");
        var missing = GameInspector.analyze(folder, "Broken.app", temporary.getRoot());
        assertEquals("Invalid", missing.status);
        assertTrue(missing.blockReason.contains("No Mach-O"));
        File ipa = temporary.newFile("Bad.ipa");
        Files.write(ipa.toPath(), new byte[] {'P', 'K', 3, 4, 0, 0});
        var invalid = GameInspector.analyze(ipa, "Bad.ipa", temporary.getRoot());
        assertEquals("IPA", invalid.kind);
        assertEquals("Invalid", invalid.status);
        assertTrue(invalid.report.contains("IMPORT_RETAINED"));
        assertTrue(ipa.exists());
    }
}
