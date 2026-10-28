package dev.radek.conventor

import org.json.JSONObject
import java.util.Locale

/** Human-readable evidence, kept separate from the machine-readable report. */
internal object CompatibilityIssues {
    /** The activity supplies these only after validating the current output files. */
    data class Artifacts(val native: Boolean = false, val semiEmulated: Boolean = false, val bounded: Boolean = false)

    data class Summary(
        val completed: List<String>,
        val available: List<String>,
        val problems: List<String>,
        val unresolvedImports: Int,
        val importsIncomplete: Boolean,
        val importDetails: String?,
    ) {
        // This describes outstanding issues, not whether any work was completed.
        fun isEmpty() = problems.isEmpty() && unresolvedImports == 0 && !importsIncomplete

        fun format(): String = buildString {
            appendLine("Completed / packaged")
            if (completed.isEmpty()) appendLine("- No converted or packaged components have been verified yet.")
            completed.forEach { appendLine("- $it") }
            if (available.isNotEmpty()) {
                appendLine()
                appendLine("Available providers (not evidence of game linkage)")
                available.forEach { appendLine("- $it") }
            }
            appendLine()
            appendLine("Outstanding compatibility issues")
            if (this@Summary.isEmpty()) appendLine("No outstanding issues are listed by this analysis.")
            problems.forEach { appendLine("- $it") }
            if (importsIncomplete && importDetails == null) appendLine("Import analysis is incomplete.")
            if (importDetails != null && (unresolvedImports > 0 || importsIncomplete)) {
                appendLine()
                appendLine(importDetails)
            }
            append("Analysis and export checks do not prove game linkage or gameplay.")
        }
    }

    fun summarize(report: JSONObject, artifacts: Artifacts = Artifacts()): Summary {
        val completed = ArrayList<String>()
        val available = ArrayList<String>()
        val problems = ArrayList<String>()
        val seen = HashSet<String>()
        fun add(value: String) {
            val text = value.trim()
            if (text.isNotEmpty() && text != "null" && seen.add(text.lowercase(Locale.ROOT))) problems.add(text)
        }

        val dependencies = report.optJSONObject("dependencies")
        val analyzed = dependencies?.optInt("analyzedImageCount", 0) ?: 0
        val failed = dependencies?.optInt("failedImageCount", 0) ?: 0
        if (analyzed > 0) completed.add("Analyzed $analyzed Mach-O image(s); $failed could not be decoded.")
        if (report.optJSONObject("icon")?.optString("status") == "SUPPORTED")
            completed.add("Recovered the launcher icon (resource only).")
        report.optJSONObject("reconstruction")?.takeIf { it.optString("status") == "ok" }?.let { recon ->
            val count = recon.optJSONObject("summary")?.optInt("functionCount", 0) ?: 0
            completed.add("Reconstructed a bounded analysis of $count function(s); this is not original source or executable code.")
        }

        val native = report.optJSONObject("gameRuntimeConversion")
        val link = report.optJSONObject("androidLink")
        if (artifacts.native && native?.optString("status") == "GENERATED") {
            if (native.optBoolean("translatedPortableCIncluded", false)) {
                if (link?.optString("status") == "VERIFIED_ANDROID_APK_LIBRARY" &&
                    link.optBoolean("apkProduced", false) && link.optBoolean("linkedIntoGame", false) &&
                    link.optBoolean("allVerificationsPassed", false) &&
                    link.optInt("linkedTranslatedFunctionCount", 0) > 0) {
                    val count = link.optInt("linkedTranslatedFunctionCount")
                    val bytes = link.optLong("androidLinkedTextBytes", 0)
                    val total = link.optLong("executableTextBytes", 0)
                    completed.add("Packaged a translated ARM64 library in the Native APK; the APK digest was checked.")
                    available.add("Host reports $count recompiled and relinked function(s) ($bytes/$total source text bytes); ELF symbols and dependencies were not independently inspected on this device.")
                    link.optJSONArray("hostReportedFunctionSymbols")?.let { symbols ->
                        for (index in 0 until minOf(symbols.length(), 100)) {
                            symbols.optString(index).takeIf { it.isNotBlank() }?.let { available.add("Host-reported function: $it") }
                        }
                    }
                } else {
                    available.add("The Native APK records a portable-C handoff; per-symbol ELF checks have not been independently confirmed for this build.")
                }
            } else {
                completed.add("Packaged the original ARM32 executable and experimental guest-CPU runtime in the Native APK; no static game conversion is claimed.")
            }
        }
        if (artifacts.semiEmulated && report.optJSONObject("anyioseConversion")?.optString("status") == "GENERATED")
            completed.add("Packaged the original IPA and AnyiOSEMU in a standalone Semi-Emulated APK; this is not static recompilation.")
        val bounded = report.optJSONObject("deviceConversion")
        if (artifacts.bounded && bounded?.optString("status") == "GENERATED_ON_DEVICE" &&
            link?.optString("status") == "VERIFIED_ANDROID_APK_LIBRARY" &&
            link.optBoolean("exportsVerified", false) && link.optBoolean("linkedIntoAndroidSharedLibrary", false)) {
            completed.add("Recompiled and packaged one proven closed-integer routine in an ARM64 Android library; this is not general game conversion.")
        }

        add(report.optString("error", ""))
        add(report.optString("nextBlocker", ""))
        report.optJSONArray("blockers")?.let { blockers ->
            for (i in 0 until blockers.length()) add(blockers.optString(i, ""))
        }
        dependencies?.optJSONArray("analysisErrors")?.let { errors ->
            for (i in 0 until errors.length()) {
                val item = errors.optJSONObject(i) ?: continue
                add("Image ${item.optString("path", "unknown")}: ${item.optString("reason", "decode failed")}")
            }
        }
        val names = HashSet<String>()
        dependencies?.optJSONArray("edges")?.let { edges ->
            for (i in 0 until edges.length()) {
                val edge = edges.optJSONObject(i) ?: continue
                val name = edge.optString("installName", "unknown dependency")
                if (!names.add(name)) continue
                // Even a provider catalog's "provided"/"resolved" entry is not
                // proof of per-game binding, a rewritten callsite, or execution.
                val evidence = edge.optJSONObject("evidence")
                val observed = evidence?.optBoolean("runtimeCallsObserved", false) == true &&
                    (evidence.optInt("linkedGameCallCount", 0) > 0)
                if (observed) {
                    completed.add("Runtime-observed dependency: $name")
                } else {
                    val reason = edge.optString("reason", "").trim()
                    add(if (reason.isEmpty()) "Dependency $name needs a verified runtime link."
                        else "Dependency $name: $reason")
                }
            }
        }
        link?.let {
            if (it.optString("status").startsWith("BLOCKED_")) {
                val reason = it.optString("reason", "").trim()
                if (reason.isNotEmpty()) add("Native Android link: $reason")
            }
        }
        if (problems.isEmpty()) {
            when (report.optString("state")) {
                "FAILED" -> add("Analysis failed without a detailed reason; inspect the full report and logs.")
                "BLOCKED" -> add("Conversion is blocked; inspect the full report and logs for details.")
            }
        }
        val mapping = report.optJSONObject("apiMapping")
        mapping?.optJSONArray("symbols")?.let { symbols ->
            for (index in 0 until symbols.length()) {
                val item = symbols.optJSONObject(index) ?: continue
                val name = item.optString("sourceSymbol").takeIf { it.isNotBlank() } ?: continue
                when (item.optString("classification")) {
                    "BIONIC_SYMBOL_CANDIDATE" -> if (item.optBoolean("verifiedOnDevice", false))
                        available.add("$name: Android export found on this device; game linkage unverified.")
                    "IMPLEMENTED_API_REPLACEMENT_AVAILABLE" -> if (item.optBoolean("runtimeVerified", false))
                        available.add("$name: compatibility export found on this device; game linkage unverified.")
                    "COMPAT_VERIFIED_HANDLER_RESOLVED" ->
                        available.add("$name: tested guest adapter registered; this game's binding is unverified.")
                }
            }
        }
        val imports = mapping?.let(ApiNeedReport::summary)
        return Summary(completed, available.distinct(), problems, imports?.unresolved ?: 0,
            imports?.incomplete ?: false, mapping?.let(ApiNeedReport::format))
    }
}
