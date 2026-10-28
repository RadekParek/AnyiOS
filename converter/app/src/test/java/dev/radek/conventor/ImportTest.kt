package dev.radek.conventor

import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import java.nio.file.Files
import java.util.Base64
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ImportTest {
    @Test fun readsAppleXmlPlist() {
        val data = """<?xml version="1.0" encoding="UTF-8"?>
            <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
            <plist version="1.0"><dict><key>CFBundleExecutable</key><string>Main</string><key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>Icon</string></array></dict></dict><key>enabled</key><true/></dict></plist>
        """.trimIndent().toByteArray()
        val plist = Plist.read(data)
        assertEquals("Main", plist["CFBundleExecutable"])
        assertEquals(true, plist["enabled"])
        assertTrue(plist["CFBundleIcons"] is Map<*, *>)
    }
    @Test fun readsBinaryPlist() {
        val data = Base64.getDecoder().decode("YnBsaXN0MDDUAQIDBAUGBwlfEBNDRkJ1bmRsZURpc3BsYXlOYW1lXxASQ0ZCdW5kbGVFeGVjdXRhYmxlXxARQ0ZCdW5kbGVJY29uRmlsZXNfEBJDRkJ1bmRsZUlkZW50aWZpZXJpAX0AbAB1AWUAbwB1AQ0AawD9VE1haW6hCFRJY29uWG9yZy50ZXN0CBEnPFBleH1/hAAAAAAAAAEBAAAAAAAAAAoAAAAAAAAAAAAAAAAAAACN")
        val plist = Plist.read(data)
        assertEquals("Main", plist["CFBundleExecutable"])
        assertEquals("org.test", plist["CFBundleIdentifier"])
        assertEquals(listOf("Icon"), plist["CFBundleIconFiles"])
        assertEquals("Žluťoučký", plist["CFBundleDisplayName"])
    }
    @Test fun rejectsMalformedBinaryPlist() {
        assertThrows(IllegalArgumentException::class.java) { Plist.read("bplist00bad".toByteArray()) }
    }
    @Test fun rejectsEntityDeclaration() {
        assertThrows(IllegalArgumentException::class.java) { Plist.read("<!DOCTYPE plist [<!ENTITY a SYSTEM 'file:///etc/passwd'>]><plist><string>&a;</string></plist>".toByteArray()) }
    }
    @Test fun rejectsTraversalNames() {
        listOf("../escape", "/absolute", "a/../b", "a\\b", "a//b", "C:/drive").forEach { path ->
            assertThrows(IllegalArgumentException::class.java) { SafeZip.validateName(path) }
        }
    }

    @Test fun acceptsDeeplyNestedGameBundlePaths() {
        // Real bundles nest a framework inside a framework inside the .app plus
        // deep asset folders; the old 32-component limit rejected them.
        val deep = (1..40).joinToString("/") { "level$it" } + "/asset.png"
        assertEquals(deep, SafeZip.validateName(deep))
        assertEquals(deep, SafeZip.memberName(deep))
        assertThrows(IllegalArgumentException::class.java) {
            SafeZip.validateName((1..200).joinToString("/") { "level$it" })
        }
        assertThrows(IllegalArgumentException::class.java) { SafeZip.validateName("dir/" + "x".repeat(300)) }
        assertThrows(IllegalArgumentException::class.java) { SafeZip.memberName("dir/" + "x".repeat(300)) }
        assertThrows(IllegalArgumentException::class.java) { SafeZip.memberName("../escape") }
    }

    @Test fun extractsEntriesWithDeepNames() {
        val root = Files.createTempDirectory("radek-test").toFile()
        try {
            val deep = "Payload/Test.app/Frameworks/Nested.framework/Versions/A/" +
                (1..30).joinToString("/") { "dir$it" } + "/data.bin"
            val archive = zip(root, listOf("Payload/Test.app/Info.plist" to "data".toByteArray(), deep to byteArrayOf(7)))
            SafeZip.extract(archive, File(root, "out"))
            assertEquals(1, File(root, "out/$deep").readBytes().size)
        } finally { root.deleteRecursively() }
    }
    @Test fun normalizesBenignMemberSyntaxWithoutErasingRealSpaces() {
        assertEquals("Payload/Test.app/ Icon .png",
            SafeZip.memberName("./Payload//Test.app/ Icon .png"))
        assertEquals("Payload/Test.app/asset.bin",
            SafeZip.memberName("/Payload/Test.app/./asset.bin"))
        // Traversal is still rejected, even when a later component would
        // appear to bring the path back under the destination.
        listOf("../escape", "Payload/../../escape", "Payload/dir/../escape").forEach {
            assertThrows(IllegalArgumentException::class.java) { SafeZip.memberName(it) }
        }
        val root = Files.createTempDirectory("radek-normalized").toFile()
        try {
            val archive = zip(root, listOf("./Payload//Test.app/ Icon .png" to byteArrayOf(7)))
            SafeZip.extract(archive, File(root, "out"))
            assertArrayEquals(byteArrayOf(7), File(root, "out/Payload/Test.app/ Icon .png").readBytes())
        } finally { root.deleteRecursively() }
    }
    @Test fun preservesColonNamesAcrossPlistExtractionAndResourcePackaging() {
        val root = Files.createTempDirectory("radek-colon").toFile()
        try {
            // A plausible filename, not a claim about the unreproduced Ninjago IPA.
            val executable = "Ninjago: RotS"
            val plist = """<?xml version="1.0"?><plist version="1.0"><dict>
                <key>CFBundleExecutable</key><string>$executable</string>
                <key>CFBundleIdentifier</key><string>com.example.game</string>
                </dict></plist>""".toByteArray()
            val prefix = "Payload/Game: The Adventure.app/"
            val archive = zip(root, listOf(prefix + "Info.plist" to plist,
                prefix + executable to byteArrayOf(1, 2, 3), prefix + "Textures/Level: 1.png" to byteArrayOf(4)))
            val out = File(root, "out")
            SafeZip.extract(archive, out)
            val app = File(out, prefix)
            val parsed = Plist.read(File(app, "Info.plist").readBytes())
            assertEquals(executable, SafeZip.bundleExecutable(parsed["CFBundleExecutable"]))
            assertArrayEquals(byteArrayOf(1, 2, 3), File(app, executable).readBytes())
            val resource = "Textures/Level: 1.png"
            assertEquals(resource, SafeZip.validateName(resource))
            assertArrayEquals(byteArrayOf(4), File(app, resource).readBytes())
            assertEquals("C_/dir/file", SafeZip.memberName("./C:/dir/file"))
            for (invalid in listOf("../escape", "dir/file", "/absolute", "dir\\file", ".", "..", "bad\u0000name")) {
                val error = assertThrows(IllegalArgumentException::class.java) { SafeZip.bundleExecutable(invalid) }
                assertTrue(error.message!!.contains("CFBundleExecutable"))
            }
        } finally { root.deleteRecursively() }
    }

    @Test fun retainsColonDistinctionsButRejectsNormalizedDriveCollisions() {
        val root = Files.createTempDirectory("radek-colon-collision").toFile()
        try {
            val archive = zip(root, listOf("Payload/Test.app/A:B" to byteArrayOf(1),
                "Payload/Test.app/A_B" to byteArrayOf(2)))
            SafeZip.extract(archive, File(root, "out"))
            assertArrayEquals(byteArrayOf(1), File(root, "out/Payload/Test.app/A:B").readBytes())
            assertArrayEquals(byteArrayOf(2), File(root, "out/Payload/Test.app/A_B").readBytes())
            val collision = zip(root, listOf("./C:/file" to byteArrayOf(1), "C_/file" to byteArrayOf(2)))
            assertThrows(IllegalArgumentException::class.java) { SafeZip.extract(collision, File(root, "collision")) }
            assertFalse(File(root, "collision").exists())
        } finally { root.deleteRecursively() }
    }
    @Test fun rejectsEntriesThatCollideAfterNormalization() {
        val root = Files.createTempDirectory("radek-collision").toFile()
        try {
            val archive = zip(root, listOf("./Payload/Test.app/data" to byteArrayOf(1),
                "Payload//Test.app/data" to byteArrayOf(2)))
            assertThrows(IllegalArgumentException::class.java) { SafeZip.extract(archive, File(root, "out")) }
            assertFalse(File(root, "out").exists())
        } finally { root.deleteRecursively() }
    }
    private fun zip(root: File, entries: List<Pair<String, ByteArray>>): File {
        val file = File(root, "input.ipa")
        ZipOutputStream(file.outputStream()).use { z -> entries.forEach { (name, value) -> z.putNextEntry(ZipEntry(name)); z.write(value); z.closeEntry() } }
        return file
    }
    @Test fun extractsAndCountsRealFiles() {
        val root = Files.createTempDirectory("radek-test").toFile()
        try {
            val archive = zip(root, listOf("Payload/Test.app/Info.plist" to "data".toByteArray(), "Payload/Test.app/icon.png" to byteArrayOf(1, 2)))
            var count = 0
            SafeZip.extract(archive, File(root, "out")) { done, total -> count = done; assertEquals(2, total) }
            assertEquals(2, count); assertEquals("data", File(root, "out/Payload/Test.app/Info.plist").readText())
        } finally { root.deleteRecursively() }
    }
    @Test fun removesPartialTraversalWorkspace() {
        val root = Files.createTempDirectory("radek-test").toFile()
        try {
            val archive = zip(root, listOf("valid" to byteArrayOf(1), "../escape" to byteArrayOf(2)))
            assertThrows(IllegalArgumentException::class.java) { SafeZip.extract(archive, File(root, "out")) }
            assertFalse(File(root, "out").exists()); assertFalse(File(root, "escape").exists())
        } finally { root.deleteRecursively() }
    }
    @Test fun rejectsSymlinksFromCentralDirectory() {
        val root = Files.createTempDirectory("radek-test").toFile()
        try {
            val archive = zip(root, listOf("link" to "target".toByteArray()))
            val bytes = archive.readBytes()
            val pos = (0..bytes.size-46).first { bytes[it] == 0x50.toByte() && bytes[it+1] == 0x4b.toByte() && bytes[it+2] == 1.toByte() && bytes[it+3] == 2.toByte() }
            bytes[pos+40] = 0xff.toByte(); bytes[pos+41] = 0xa1.toByte(); archive.writeBytes(bytes)
            assertThrows(IllegalArgumentException::class.java) { SafeZip.extract(archive, File(root, "out")) }
        } finally { root.deleteRecursively() }
    }
}
