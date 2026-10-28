package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class SourcePolicyTest {
    @Test fun onlyIpaAndLegacyIpaReportsCanBuild() {
        fun report(kind: String) = JSONObject().put("source", JSONObject().put("kind", kind))
        assertTrue(SourcePolicy.supportsIpaPackaging(report("IPA")))
        assertTrue(SourcePolicy.supportsIpaPackaging(JSONObject().put("source", JSONObject())))
        for (kind in listOf("FOLDER", "EXECUTABLE", "unknown")) {
            assertFalse(SourcePolicy.supportsIpaPackaging(report(kind)))
            try { SourcePolicy.requireIpa(report(kind)); fail("Accepted $kind") }
            catch (_: IllegalArgumentException) { }
        }
        assertFalse(SourcePolicy.supportsIpaPackaging(JSONObject()))
    }

    @Test fun failedAnalysisCanBeRetriedWithoutDiscardingGeneratedArtifacts() {
        val report = JSONObject().put("state", "FAILED")
        assertTrue(SourcePolicy.canRetryFailedAnalysis(report))
        for (key in listOf("hostConversion", "deviceConversion", "gameRuntimeConversion", "anyioseConversion", "placeholderConversion")) {
            val withArtifact = JSONObject(report.toString()).put(key, JSONObject())
            assertFalse(SourcePolicy.canRetryFailedAnalysis(withArtifact))
        }
        assertFalse(SourcePolicy.canRetryFailedAnalysis(JSONObject().put("state", "PARTIAL")))
        val blocked = JSONObject().put("state", "BLOCKED")
            .put("dependencies", JSONObject().put("failedImageCount", 1))
        assertTrue(SourcePolicy.canRetryFailedAnalysis(blocked))
        assertFalse(SourcePolicy.canRetryFailedAnalysis(JSONObject(blocked.toString()).put("anyioseConversion", JSONObject())))
        assertFalse(SourcePolicy.canRetryFailedAnalysis(JSONObject().put("state", "BLOCKED")))
    }

    @Test fun retryBacksUpExactReportAndDoesNotRemoveSourceOrExistingArtifacts() {
        val context = org.robolectric.RuntimeEnvironment.getApplication()
        val library = Library(context)
        val entry = java.io.File(library.root, "game-${java.util.UUID.randomUUID()}").apply { mkdirs() }
        try {
            java.io.File(entry, "game.json").writeText(JSONObject().put("kind", "Folder").toString())
            val source = java.io.File(entry, "bundle").apply { mkdirs() }
            java.io.File(source, "marker.txt").writeText("source remains")
            val oldReport = """{"state":"BLOCKED","dependencies":{"failedImageCount":1},"marker":"previous"}"""
            java.io.File(entry, "report.json").writeText(oldReport)
            val artifact = java.io.File(entry, "untracked-output.apk").apply { writeText("artifact remains") }
            library.analyzeShared(entry) { _, _ -> }
            val backups = java.io.File(entry, "analysis-history").listFiles().orEmpty()
            assertEquals(1, backups.size)
            assertEquals(oldReport, backups.single().readText())
            assertEquals("source remains", java.io.File(source, "marker.txt").readText())
            assertEquals("artifact remains", artifact.readText())
        } finally { entry.deleteRecursively() }
    }
}
