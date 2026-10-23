package org.podnative;

import static org.junit.Assert.*;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class GameInspectorExtractTest {
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
    public void extractsOnlyTheSingleAppBundleAndKeepsNestedAssets() throws Exception {
        File source = ipa("Game.ipa", "Payload/Angry.app/Info.plist", "plist",
            "Payload/Angry.app/AngryBirds", "mach-o", "Payload/Angry.app/data/atlas.bin",
            "texture-data", "iTunesMetadata.plist", "outside");
        File staging = new File(temporary.getRoot(), "staging");
        File bundle = GameInspector.extractAppBundle(source, staging, "AngryBirds");
        assertTrue(new File(bundle, "Info.plist").isFile());
        assertEquals("texture-data",
            new String(Files.readAllBytes(new File(bundle, "data/atlas.bin").toPath()),
                StandardCharsets.UTF_8));
        assertFalse(new File(bundle, "iTunesMetadata.plist").exists());
        assertTrue(source.isFile());
    }

    @Test
    public void rejectsTraversalAndDoesNotWriteOutsideTheStagingDirectory() throws Exception {
        File source = ipa("Traversal.ipa", "Payload/Test.app/Info.plist", "plist",
            "Payload/Test.app/Test", "mach-o", "Payload/Test.app/../escaped.txt", "nope");
        File staging = new File(temporary.getRoot(), "unsafe-staging");
        try {
            GameInspector.extractAppBundle(source, staging, "Test");
            fail("zip path traversal must be refused");
        } catch (java.io.IOException expected) {
            assertTrue(expected.getMessage().contains("escapes"));
        }
        assertFalse(new File(temporary.getRoot(), "escaped.txt").exists());
        assertTrue(source.isFile());
    }

    @Test
    public void extractionObservesCancellationAndLeavesCleanupToItsStagingOwner() throws Exception {
        File source = ipa("Cancelable.ipa", "Payload/Test.app/Info.plist", "plist",
            "Payload/Test.app/Test", "mach-o", "Payload/Test.app/data.bin", "payload");
        File staging = new File(temporary.getRoot(), "cancelled-staging");
        AtomicInteger checks = new AtomicInteger();
        try {
            GameInspector.extractAppBundle(
                source, staging, "Test", () -> checks.incrementAndGet() >= 8);
            fail("cancelled extraction must stop before completing");
        } catch (CancellationException expected) {
            assertTrue(expected.getMessage().contains("cancelled"));
        }
        assertTrue("the caller owns staging cleanup", staging.isDirectory());
        assertTrue(source.isFile());
    }

    @Test
    public void refusesAmbiguousTopLevelAppBundles() throws Exception {
        File source = ipa("TwoApps.ipa", "Payload/One.app/Info.plist", "one", "Payload/One.app/One",
            "mach-o", "Payload/Two.app/Info.plist", "two", "Payload/Two.app/Two", "mach-o");
        try {
            GameInspector.extractAppBundle(
                source, new File(temporary.getRoot(), "ambiguous-staging"), "One");
            fail("ambiguous IPA must not be extracted");
        } catch (java.io.IOException expected) {
            assertTrue(expected.getMessage().contains("exactly one"));
        }
    }
}
