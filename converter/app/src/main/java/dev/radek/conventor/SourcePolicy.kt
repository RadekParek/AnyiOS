package dev.radek.conventor

import org.json.JSONObject

/** Keep in-place analysis separate from operations that require an IPA archive. */
internal object SourcePolicy {
    fun supportsIpaPackaging(report: JSONObject): Boolean {
        val source = report.optJSONObject("source") ?: return false
        // Reports made before source kinds were introduced could only contain IPAs.
        return source.optString("kind") in setOf("", "IPA")
    }

    fun requireIpa(report: JSONObject) {
        require(supportsIpaPackaging(report)) { "APK packaging requires an IPA; import a complete IPA first" }
    }

    fun canRetryFailedAnalysis(report: JSONObject): Boolean =
        report.optString("state") == "FAILED" &&
            listOf("hostConversion", "deviceConversion", "gameRuntimeConversion", "anyioseConversion", "placeholderConversion")
                .none { report.has(it) }
}
