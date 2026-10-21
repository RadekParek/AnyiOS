package dev.radek.conventor

import java.io.File
import java.util.UUID
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class SharedCatalogTest {
    @Test fun emulatorEntryAppearsWithoutCopyingAndSurvivesConverterRecovery() {
        val library = Library(RuntimeEnvironment.getApplication())
        val entry = File(library.root, "game-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val original = byteArrayOf(1, 2, 3, 4)
            File(entry, "source.bin").writeBytes(original)
            File(entry, "game.json").writeText(JSONObject().put("title", "Shared owned IPA")
                .put("kind", "IPA").put("blockReason", "Unsupported architecture").toString())
            val report = library.entries().first { it.first == entry }.second
            assertEquals("SHARED_SOURCE", report.getString("state"))
            assertEquals("Shared owned IPA", report.getJSONObject("application").getString("name"))
            library.recoverInterrupted()
            assertArrayEquals(original, File(entry, "source.bin").readBytes())
            assertFalse(File(entry, "source.ipa").exists())
            assertFalse(File(entry, "report.json").exists())
            // A failed analysis of an existing source must never truncate/delete it.
            val analyzed = library.analyzeShared(entry) { _, _ -> }.second
            assertEquals("FAILED", analyzed.getString("state"))
            assertArrayEquals(original, File(entry, "source.bin").readBytes())
            assertTrue(File(entry, "game.json").isFile)
        } finally { entry.deleteRecursively() }
    }

    @Test fun jobsInDifferentModulesCannotOwnTheCatalogTogether() {
        SharedCatalog.end()
        try {
            assertTrue(SharedCatalog.tryBegin())
            assertTrue(SharedCatalog.isBusy())
            assertFalse(SharedCatalog.tryBegin())
            SharedCatalog.end()
            assertTrue(SharedCatalog.tryBegin())
        } finally { SharedCatalog.end() }
    }

    @Test fun recoveryCannotMutateAnEntryWhileAnotherModuleOwnsTheCatalog() {
        val library = Library(RuntimeEnvironment.getApplication())
        val entry = File(library.root, "game-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            File(entry, "source.bin").writeText("original bytes")
            File(entry, "convert-workspace").mkdirs()
            library.save(entry, JSONObject().put("state", "ANALYZING"))
            assertTrue(SharedCatalog.tryBegin())
            library.recoverInterrupted()
            assertEquals("ANALYZING", JSONObject(File(entry, "report.json").readText()).getString("state"))
            assertTrue(File(entry, "convert-workspace").isDirectory)
            assertTrue(SharedCatalog.isBusy())
            SharedCatalog.end()
            library.recoverInterrupted()
            assertEquals("FAILED", JSONObject(File(entry, "report.json").readText()).getString("state"))
            assertFalse(File(entry, "convert-workspace").exists())
            assertEquals("original bytes", File(entry, "source.bin").readText())
        } finally { SharedCatalog.end(); entry.deleteRecursively() }
    }

    @Test fun interruptedConverterReportWithoutHashCannotDeleteCanonicalSource() {
        val library = Library(RuntimeEnvironment.getApplication())
        val entry = File(library.root, "game-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            File(entry, "source.bin").writeText("original bytes")
            library.save(entry, JSONObject().put("state", "ANALYZING"))
            library.recoverInterrupted()
            assertEquals("original bytes", File(entry, "source.bin").readText())
            assertEquals("FAILED", JSONObject(File(entry, "report.json").readText()).getString("state"))
        } finally { entry.deleteRecursively() }
    }
}
