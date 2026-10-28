package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

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
    }
}
