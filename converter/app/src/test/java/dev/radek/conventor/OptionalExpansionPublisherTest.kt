package dev.radek.conventor

import java.io.File
import java.io.IOException
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class OptionalExpansionPublisherTest {
    private val name = "main.1.dev.example.game.obb"

    @Test fun publishesAndRecordsOnlyACompleteExpansion() = withDirectory { dir ->
        var recorded: OptionalExpansionPublisher.Artifact? = null
        val error = OptionalExpansionPublisher.publish(dir, name, { it.writeText("new resources") }) {
            recorded = it
        }
        assertNull(error)
        assertEquals("new resources", File(dir, name).readText())
        assertEquals(name, recorded?.name)
        assertEquals(File(dir, name).length(), recorded?.bytes)
        val digest = java.security.MessageDigest.getInstance("SHA-256").digest(File(dir, name).readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        assertEquals(digest, recorded?.sha256)
        assertFalse(File(dir, ".$name.optional-pending").exists())
    }

    @Test fun identicalObbNamesAreIsolatedByRuntime() = withDirectory { dir ->
        assertNull(OptionalExpansionPublisher.publish(dir, name, "native", { it.writeText("native") }) {})
        assertNull(OptionalExpansionPublisher.publish(dir, name, "bounded", { it.writeText("bounded") }) {})
        assertEquals("native", ArtifactNames.expansionFile(dir, name, "native", "native").readText())
        assertEquals("bounded", ArtifactNames.expansionFile(dir, name, "bounded", "bounded").readText())
        assertNotNull(OptionalExpansionPublisher.publish(dir, name, "../unsafe", { it.writeText("no") }) {})
        org.junit.Assert.assertThrows(IllegalArgumentException::class.java) {
            ArtifactNames.expansionFile(dir, name, "bounded", "native")
        }
    }

    @Test fun resourceArchivePreservesRelativePathsAndContents() = withDirectory { dir ->
        val first = File(dir, "input1").apply { writeBytes(byteArrayOf(0, 1, 2, 3)) }
        val second = File(dir, "input2").apply { writeText("hello") }
        val archive = File(dir, "out.obb")
        ResourceExpansionArchive.write(archive, listOf("assets/texture.bin" to first, "Info.plist" to second))
        java.util.zip.ZipFile(archive).use { zip ->
            assertEquals(listOf("assets/texture.bin", "Info.plist"), zip.entries().asSequence().map { it.name }.toList())
            assertEquals(java.util.zip.ZipEntry.STORED, zip.getEntry("assets/texture.bin").method)
            org.junit.Assert.assertArrayEquals(first.readBytes(), zip.getInputStream(zip.getEntry("assets/texture.bin")).use { it.readBytes() })
            assertEquals("hello", zip.getInputStream(zip.getEntry("Info.plist")).bufferedReader().use { it.readText() })
        }
    }

    @Test fun failedWriteLeavesOldExpansionAndNeverRecordsPartialBytes() = withDirectory { dir ->
        File(dir, name).writeText("previous")
        var called = false
        val error = OptionalExpansionPublisher.publish(dir, name, {
            it.writeText("partial")
            throw IOException("disk full")
        }) { called = true }
        assertEquals("disk full", error)
        assertEquals("previous", File(dir, name).readText())
        assertFalse(called)
        assertFalse(File(dir, ".$name.optional-pending").exists())
    }

    @Test fun unsafeResourceDoesNotReplaceAnExistingExpansion() = withDirectory { dir ->
        File(dir, name).writeText("previous")
        val payload = File(dir, "input").apply { writeText("data") }
        val error = OptionalExpansionPublisher.publish(dir, name, {
            ResourceExpansionArchive.write(it, listOf("../bad" to payload))
        }) { throw AssertionError("must not record") }
        assertNotNull(error)
        assertEquals("previous", File(dir, name).readText())
        assertFalse(File(dir, ".$name.optional-pending").exists())
    }

    @Test fun outOfMemoryDuringTheOptionalCopyKeepsThePreviousFile() = withDirectory { dir ->
        File(dir, name).writeText("previous")
        val error = OptionalExpansionPublisher.publish(dir, name, {
            it.writeText("partial")
            throw OutOfMemoryError("simulated")
        }) { throw AssertionError("must not record") }
        assertEquals("not enough memory for the optional expansion", error)
        assertEquals("previous", File(dir, name).readText())
        assertFalse(File(dir, ".$name.optional-pending").exists())
    }

    @Test fun failedReportRollsBackToPreviousExpansion() = withDirectory { dir ->
        File(dir, name).writeText("previous")
        val error = OptionalExpansionPublisher.publish(dir, name, { it.writeText("new") }) {
            throw IOException("report unavailable")
        }
        assertEquals("report unavailable", error)
        assertEquals("previous", File(dir, name).readText())
        assertFalse(File(dir, ".$name.optional-pending").exists())
        assertFalse(File(dir, ".$name.optional-backup").exists())
    }

    @Test fun recoversAnInterruptedBackupBeforeAttemptingAWrite() = withDirectory { dir ->
        File(dir, ".$name.optional-backup").writeText("previous")
        val error = OptionalExpansionPublisher.publish(dir, name, { throw IOException("disk full") }) {}
        assertEquals("disk full", error)
        assertEquals("previous", File(dir, name).readText())
    }

    @Test fun rejectsUnsafeNamesAndSymlinks() = withDirectory { dir ->
        assertNotNull(OptionalExpansionPublisher.publish(dir, "../escape.obb", { it.writeText("bad") }) {})
        val victim = File(dir, "victim").apply { writeText("unchanged") }
        val link = File(dir, name)
        try {
            Files.createSymbolicLink(link.toPath(), victim.toPath())
        } catch (error: UnsupportedOperationException) {
            org.junit.Assume.assumeNoException(error)
        } catch (error: IOException) {
            org.junit.Assume.assumeNoException(error)
        } catch (error: SecurityException) {
            org.junit.Assume.assumeNoException(error)
        }
        assertNotNull(OptionalExpansionPublisher.publish(dir, name, { it.writeText("bad") }) {})
        assertEquals("unchanged", victim.readText())
    }

    private fun withDirectory(body: (File) -> Unit) {
        val dir = Files.createTempDirectory("optional-obb").toFile()
        try { body(dir) } finally { dir.deleteRecursively() }
    }
}
