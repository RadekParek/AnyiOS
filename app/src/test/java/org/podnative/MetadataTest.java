package org.podnative;

import static org.junit.Assert.*;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.junit.Test;

public class MetadataTest {
    private byte[] xml(String body) {
        return (
            "<?xml version=\"1.0\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://example.invalid/never-fetch\"><plist version=\"1.0\">"
            + body + "</plist>")
            .getBytes(StandardCharsets.UTF_8);
    }
    private void reject(byte[] bytes) {
        assertThrows(IOException.class, () -> Plist.read(bytes));
    }
    @Test
    public void xmlMetadataAndExternalDtd() throws Exception {
        var parsed = Plist.read(xml(
            "<dict><key>name</key><string>Owned game</string><key>negative</key><integer>-7</integer><key>enabled</key><true/></dict>"));
        assertEquals("Owned game", parsed.get("name"));
        assertEquals(-7L, parsed.get("negative"));
        assertEquals(true, parsed.get("enabled"));
    }
    @Test
    public void binaryMetadata() throws Exception {
        try (var stream = getClass().getResourceAsStream("/metadata.bplist")) {
            assertNotNull(stream);
            var parsed = Plist.read(stream.readAllBytes());
            assertEquals("Spinjitzu ☯", parsed.get("CFBundleDisplayName"));
            assertEquals(-7L, parsed.get("negative"));
            assertEquals(2, ((java.util.List<?>) parsed.get("icons")).size());
        }
    }
    @Test
    public void rejectsMalformedAndExcessiveMetadata() {
        reject(xml("<array/>"));
        reject(xml("<dict><key>x</key></dict>"));
        reject(xml("<dict><key>x</key><true/><key>x</key><false/></dict>"));
        reject(xml("<dict><key>x</key><integer>oops</integer></dict>"));
        reject(xml("<array>".repeat(10000) + "</array>".repeat(10000)));
        reject("<!DOCTYPE plist [<!ENTITY x 'expanded'>]><plist><dict/></plist>".getBytes(
            StandardCharsets.UTF_8));
        reject(new byte[1024 * 1024 + 1]);
        reject("bplist00".getBytes(StandardCharsets.US_ASCII));
    }
    @Test
    public void binaryCycleIsRejected() {
        // One array object referring to itself; valid offsets/trailer, invalid graph.
        byte[] bytes = new byte[43];
        System.arraycopy("bplist00".getBytes(StandardCharsets.US_ASCII), 0, bytes, 0, 8);
        bytes[8] = (byte) 0xa1;
        bytes[10] = 8;
        bytes[17] = 1;
        bytes[18] = 1; // trailer widths at t+6/t+7
        bytes[26] = 1;
        bytes[42] = 10;
        reject(bytes);
    }
    @Test
    public void namesAndTitles() throws Exception {
        assertEquals("Info.plist", LibraryRules.fileName("Info.plist"));
        for (String name : new String[] {"", ".", "..", "../Game", "a/b", "a\\b", "a\0b"})
            assertThrows(IOException.class, () -> LibraryRules.fileName(name));
        assertEquals("My Game", LibraryRules.title("My_Game.IPA"));
        assertEquals("Untitled game", LibraryRules.title(".ipa"));
        assertEquals("fallback", LibraryRules.metadataText(12, "fallback"));
        assertEquals("Game name", LibraryRules.metadataText("Game\nname", "fallback"));
    }
}
