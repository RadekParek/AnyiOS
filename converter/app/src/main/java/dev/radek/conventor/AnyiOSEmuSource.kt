package dev.radek.conventor

import org.json.JSONObject

/** Fail closed on unknown or protected executable metadata. */
internal object AnyiOSEmuSource {
    fun requireSupportedSource(report: JSONObject) {
        val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: error("Analyze the IPA first")
        require(slices.length() > 0) { "No Mach-O slices were analyzed" }
        var arm32 = false
        for (i in 0 until slices.length()) {
            val slice = slices.getJSONObject(i)
            require(slice.has("encrypted") && !slice.optBoolean("encrypted", true)) {
                "Encrypted or unverified executables cannot be packaged; DRM is not bypassed"
            }
            if (slice.optString("architecture").lowercase() in setOf("armv5tej", "armv6", "armv7", "armv7s", "armv7f", "arm32")) arm32 = true
        }
        require(arm32) { "AnyiOSEMU requires an ARM32 slice; ARM64-only apps are unsupported" }
        val dependencies = report.optJSONObject("dependencies") ?: error("Embedded images were not analyzed")
        require(dependencies.optInt("failedImageCount", -1) == 0) {
            ImageDiagnostics.firstFailure(dependencies.optJSONArray("analysisErrors"))
                ?: "An embedded executable could not be verified"
        }
        val nodes = dependencies.optJSONArray("nodes") ?: error("Embedded image inventory is missing")
        for (i in 0 until nodes.length()) {
            val embedded = nodes.optJSONObject(i)?.optJSONObject("analysis")?.optJSONArray("slices")
                ?: error("An embedded executable could not be verified")
            require(embedded.length() > 0) { "An embedded executable has no verified slices" }
            for (j in 0 until embedded.length()) {
                val slice = embedded.getJSONObject(j)
                require(slice.has("encrypted") && !slice.optBoolean("encrypted", true)) {
                    "A bundled executable is encrypted or unverified; DRM is not bypassed"
                }
            }
        }
    }
}
