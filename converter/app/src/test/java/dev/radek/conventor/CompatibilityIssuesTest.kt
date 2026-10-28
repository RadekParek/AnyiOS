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

    @Test fun availableExportsAndOutstandingIssuesAreSeparate() {
        val mapping = JSONObject().put("distinctImportSymbols", 2).put("classificationStatus", "COMPLETE")
            .put("symbols", JSONArray().put(symbol("_done", "BIONIC_SYMBOL_CANDIDATE", true))
                .put(symbol("_missing", "UNMAPPED")))
        val edges = JSONArray().put(JSONObject().put("installName", "Finished.framework")
            .put("status", "resolved").put("reason", "Catalog says resolved; no game binding."))
            .put(JSONObject().put("installName", "Candidate.framework").put("status", "provided")
                .put("reason", "A candidate exists, but its game link is not verified."))
        val report = JSONObject().put("error", "Bad image").put("nextBlocker", "Bad image")
            .put("blockers", JSONArray().put("Bad image").put("Missing renderer"))
            .put("dependencies", JSONObject().put("edges", edges).put("analyzedImageCount", 1))
            .put("apiMapping", mapping)
        val summary = CompatibilityIssues.summarize(report)
        assertEquals(1, summary.unresolvedImports)
        assertEquals(4, summary.problems.size)
        val text = summary.format()
        assertTrue(text.contains("Analyzed 1 Mach-O image"))
        assertTrue(text.contains("_done: Android export found on this device; game linkage unverified"))
        assertTrue(text.contains("_missing"))
        assertTrue(text.contains("Finished.framework"))
        assertTrue(text.contains("Candidate.framework"))
        assertTrue(text.contains("Missing renderer"))
        assertTrue(text.contains("do not prove game linkage or gameplay"))
    }

    @Test fun hostReportsNeverBecomeDeviceInspectedSymbols() {
        val link = JSONObject().put("status", "VERIFIED_ANDROID_APK_LIBRARY")
            .put("apkProduced", true).put("linkedIntoGame", true).put("allVerificationsPassed", true)
            .put("linkedTranslatedFunctionCount", 2).put("androidLinkedTextBytes", 8)
            .put("executableTextBytes", 100).put("deviceElfInspectionPassed", true)
            .put("hostReportedFunctionSymbols", JSONArray().put("translated_one").put("translated_two"))
        val report = JSONObject().put("androidLink", link).put("gameRuntimeConversion",
            JSONObject().put("status", "GENERATED").put("translatedPortableCIncluded", true))
        assertTrue(CompatibilityIssues.summarize(report).completed.isEmpty())
        val summary = CompatibilityIssues.summarize(report, CompatibilityIssues.Artifacts(native = true))
        assertTrue(summary.completed.single().contains("APK digest was checked"))
        assertTrue(summary.available.any { it.contains("Host reports 2 recompiled and relinked") })
        assertTrue(summary.available.any { it.contains("not independently inspected") })
        assertTrue(summary.available.any { it.contains("Host-reported function: translated_one") })
        assertFalse(summary.completed.any { it.contains("Relinked function") })
    }

    @Test fun semiEmulatedPackagingIsNotClaimedAsStaticConversion() {
        val report = JSONObject().put("anyioseConversion", JSONObject().put("status", "GENERATED"))
        assertTrue(CompatibilityIssues.summarize(report).completed.isEmpty())
        val summary = CompatibilityIssues.summarize(report, CompatibilityIssues.Artifacts(semiEmulated = true))
        assertTrue(summary.completed.single().contains("not static recompilation"))
    }

    @Test fun incompleteListingsNeverLookFullyResolved() {
        val mapping = JSONObject().put("distinctImportSymbols", 3).put("classificationStatus", "PARTIAL")
            .put("symbols", JSONArray().put(symbol("_verified", "BIONIC_SYMBOL_CANDIDATE", true)))
        val summary = CompatibilityIssues.summarize(JSONObject().put("apiMapping", mapping))
        assertEquals(0, summary.unresolvedImports)
        assertTrue(summary.importsIncomplete)
        assertFalse(summary.isEmpty())
        assertTrue(summary.format().contains("analysis may be incomplete"))
        assertTrue(summary.format().contains("game linkage unverified"))
    }

    @Test fun malformedOrNotRunImportInventoryIsStillAnIssue() {
        val malformed = JSONObject().put("distinctImportSymbols", 0).put("symbols", JSONArray().put(JSONObject.NULL))
        assertTrue(CompatibilityIssues.summarize(JSONObject().put("apiMapping", malformed)).importsIncomplete)
        val notRun = JSONObject().put("classificationStatus", "NOT_RUN").put("symbols", JSONArray())
        assertTrue(CompatibilityIssues.summarize(JSONObject().put("apiMapping", notRun)).importsIncomplete)
    }

    @Test fun failedOrBlockedReportsNeverLookLikeCleanAnalysis() {
        for (state in listOf("FAILED", "BLOCKED")) {
            val summary = CompatibilityIssues.summarize(JSONObject().put("state", state))
            assertFalse(summary.isEmpty())
            assertFalse(summary.format().contains("No outstanding issues"))
        }
    }

    @Test fun emptyReportIsNotACompatibilityGuarantee() {
        val summary = CompatibilityIssues.summarize(JSONObject())
        assertTrue(summary.isEmpty())
        assertTrue(summary.format().contains("No outstanding issues"))
        assertTrue(summary.format().contains("do not prove game linkage or gameplay"))
    }
}
