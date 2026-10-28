package dev.radek.conventor

import org.json.JSONObject
import java.util.Locale

/** A short issue-only view; the original report and logs remain unchanged. */
internal object CompatibilityIssues {
    data class Summary(
        val problems: List<String>,
        val unresolvedImports: Int,
        val importsIncomplete: Boolean,
        val importDetails: String?,
    ) {
        fun isEmpty() = problems.isEmpty() && unresolvedImports == 0 && !importsIncomplete

        fun format(): String = buildString {
            appendLine("Outstanding compatibility issues")
            if (this@Summary.isEmpty()) appendLine("No outstanding issues are listed by this analysis.")
            if (problems.isNotEmpty()) {
                problems.forEach { appendLine("- $it") }
            }
            if (importsIncomplete && importDetails == null) appendLine("Import analysis is incomplete.")
            if (importDetails != null && (unresolvedImports > 0 || importsIncomplete)) {
                appendLine()
                appendLine(importDetails)
            }
            append("Analysis and export checks do not prove game linkage or gameplay.")
        }
    }

    fun summarize(report: JSONObject): Summary {
        val problems = ArrayList<String>()
        val seen = HashSet<String>()
        fun add(value: String) {
            val text = value.trim()
            if (text.isNotEmpty() && text != "null" && seen.add(text.lowercase(Locale.ROOT))) problems.add(text)
        }
        add(report.optString("error", ""))
        add(report.optString("nextBlocker", ""))
        report.optJSONArray("blockers")?.let { blockers ->
            for (i in 0 until blockers.length()) add(blockers.optString(i, ""))
        }
        val dependencies = report.optJSONObject("dependencies")
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
                val status = edge.optString("status", "no-execution-path-yet").lowercase(Locale.ROOT)
                // "provided" in the provider catalog is only a possible Android
                // target, not proof that this dependency was linked to the game.
                if (status in setOf("resolved", "verified", "complete")) continue
                val name = edge.optString("installName", "unknown dependency")
                if (!names.add(name)) continue
                val reason = edge.optString("reason", "").trim()
                add(if (reason.isEmpty()) "Dependency $name needs a verified runtime link."
                    else "Dependency $name: $reason")
            }
        }
        report.optJSONObject("androidLink")?.let { link ->
            if (link.optString("status").startsWith("BLOCKED_")) {
                val reason = link.optString("reason", "").trim()
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
        val imports = mapping?.let(ApiNeedReport::summary)
        return Summary(
            problems,
            imports?.unresolved ?: 0,
            imports?.incomplete ?: false,
            mapping?.let(ApiNeedReport::format),
        )
    }
}
