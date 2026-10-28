package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [29], manifest = Config.NONE)
class CompatibilityIssuesTest {
    private fun symbol(name: String, classification: String, verified: Boolean = false) = JSONObject()
        .put("sourceSymbol", name).put("classification", classification).put("verifiedOnDevice", verified)

    @Test fun onlyOutstandingItemsAreShownAndEvidenceRemainsHonest() {
        val mapping = JSONObject().put("distinctImportSymbols", 2).put("classificationStatus", "COMPLETE")
            .put("symbols", JSONArray().put(symbol("_done", "BIONIC_SYMBOL_CANDIDATE", true))
                .put(symbol("_missing", "UNMAPPED")))
        val edges = JSONArray().put(JSONObject().put("installName", "Finished.framework")
            .put("status", "resolved").put("reason", "done"))
            .put(JSONObject().put("installName", "Candidate.framework").put("status", "provided")
                .put("reason", "A candidate exists, but its game link is not verified."))
        val report = JSONObject().put("error", "Bad image").put("nextBlocker", "Bad image")
            .put("blockers", JSONArray().put("Bad image").put("Missing renderer"))
            .put("dependencies", JSONObject().put("edges", edges))
            .put("apiMapping", mapping)
            .put("analysisProgress", JSONObject().put("percent", 100).put("message", "analysis complete"))
        val summary = CompatibilityIssues.summarize(report)
        assertEquals(1, summary.unresolvedImports)
        assertEquals(3, summary.problems.size)
        val text = summary.format()
        assertTrue(text.contains("_missing"))
        assertFalse(text.contains("_done"))
        assertFalse(text.contains("Finished.framework"))
        assertTrue(text.contains("Candidate.framework"))
        assertTrue(text.contains("Missing renderer"))
        assertFalse(text.contains("analysis complete"))
        assertTrue(text.contains("do not prove game linkage or gameplay"))
    }

    @Test fun incompleteListingsNeverLookFullyResolved() {
        val mapping = JSONObject().put("distinctImportSymbols", 3).put("classificationStatus", "PARTIAL")
            .put("symbols", JSONArray().put(symbol("_verified", "BIONIC_SYMBOL_CANDIDATE", true)))
        val summary = CompatibilityIssues.summarize(JSONObject().put("apiMapping", mapping))
        assertEquals(0, summary.unresolvedImports)
        assertTrue(summary.importsIncomplete)
        assertFalse(summary.isEmpty())
        assertTrue(summary.format().contains("analysis may be incomplete"))
        assertFalse(summary.format().contains("_verified"))
    }

    @Test fun malformedOrNotRunImportInventoryIsStillAnIssue() {
        val malformed = JSONObject().put("distinctImportSymbols", 0).put("symbols", JSONArray().put(JSONObject.NULL))
        assertTrue(CompatibilityIssues.summarize(JSONObject().put("apiMapping", malformed)).importsIncomplete)
        val notRun = JSONObject().put("classificationStatus", "NOT_RUN").put("symbols", JSONArray())
        assertTrue(CompatibilityIssues.summarize(JSONObject().put("apiMapping", notRun)).importsIncomplete)
    }

    @Test fun emptyReportIsNotACompatibilityGuarantee() {
        val summary = CompatibilityIssues.summarize(JSONObject())
        assertTrue(summary.isEmpty())
        assertTrue(summary.format().contains("No outstanding issues"))
        assertTrue(summary.format().contains("do not prove game linkage or gameplay"))
    }
}
