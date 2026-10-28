package dev.radek.conventor

import java.io.File
import java.nio.file.Files
import org.junit.Assert.*
import org.junit.Test

class SharedFolderTest {
    @Test fun snapshotsTheExistingFolderWithoutCreatingAnotherArchive() {
        val root = Files.createTempDirectory("shared-folder").toFile()
        try {
            val app = File(root, "Nested/Game.app").apply { mkdirs() }
            File(app, "Info.plist").writeText("plist")
            File(app, "Ninjago: RotS").writeBytes(byteArrayOf(1, 2, 3))
            val first = SharedFolder.inspect(root) {}
            assertEquals(app.canonicalFile, first.app.canonicalFile)
            assertEquals(8, first.bytes)
            assertEquals(first.sha256, SharedFolder.inspect(root) {}.sha256)
            assertFalse(File(root, "source.bin").exists())
            File(app, "Ninjago: RotS").writeBytes(byteArrayOf(4, 5, 6))
            assertNotEquals(first.sha256, SharedFolder.inspect(root) {}.sha256)
        } finally { root.deleteRecursively() }
    }

    @Test fun refusesSymlinksAndCaseCollisions() {
        val root = Files.createTempDirectory("shared-folder-safe").toFile()
        try {
            File(root, "Info.plist").writeText("plist")
            File(root, "Data").writeText("a")
            File(root, "data").writeText("b")
            assertThrows(IllegalArgumentException::class.java) { SharedFolder.inspect(root) {} }
            File(root, "data").delete()
            try { Files.createSymbolicLink(File(root, "link").toPath(), File(root, "Data").toPath()) }
            catch (e: Exception) { org.junit.Assume.assumeNoException(e) }
            assertThrows(IllegalArgumentException::class.java) { SharedFolder.inspect(root) {} }
        } finally { root.deleteRecursively() }
    }
}
