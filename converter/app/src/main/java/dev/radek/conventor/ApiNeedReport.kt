package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/**
 * Formats ONLY the imported symbols that still need a real Android binding or
 * behavior. Fully-resolved imports (a same-name NDK export verified on this
 * device, or a concrete/tested compatibility implementation) are omitted so the
 * output is a short, sendable list of exactly what is missing - no triage prose.
 */
internal object ApiNeedReport {

    /** What is still needed for one import. */
    private enum class Need {
        STUB,              // explicit unimplemented stub handler
        UNMAPPED,          // no provider of any kind
        SEMANTIC,          // needs a real API/object rewrite
        COMPILER_RUNTIME,  // needs the NDK compiler-rt/libunwind runtime
        GUEST_ADAPTER,     // bounded guest adapter exists, semantics incomplete
        NDK_UNVERIFIED,    // same-name NDK candidate, not confirmed on device
        IMPLEMENTATION_UNVERIFIED, // code exists, but the export was not resolved
    }

    data class Summary(val total: Int, val listed: Int, val unresolved: Int, val incomplete: Boolean)

    fun summary(mapping: JSONObject): Summary {
        val symbols = mapping.optJSONArray("symbols") ?: JSONArray()
        val items = (0 until symbols.length()).mapNotNull { symbols.optJSONObject(it) }
        val total = mapping.optInt("distinctImportSymbols", symbols.length()).coerceAtLeast(0)
        val incomplete = items.size != total || items.size != symbols.length() || mapping.optBoolean("truncated", false) ||
            mapping.optString("classificationStatus") in setOf("PARTIAL", "TRUNCATED", "INCOMPLETE", "FAILED", "NOT_RUN")
        return Summary(total, items.size, items.count { needFor(it) != null }, incomplete)
    }

    fun format(mapping: JSONObject): String {
        val symbols = mapping.optJSONArray("symbols") ?: JSONArray()
        val summary = summary(mapping)
        val total = summary.total

        val items = (0 until symbols.length()).mapNotNull { symbols.optJSONObject(it) }

        // Split into "already has a real, working provider" vs "still needs work".
        var resolved = 0
        val byNeed = LinkedHashMap<Need, MutableList<JSONObject>>()
        for (item in items) {
            val need = needFor(item)
            if (need == null) {
                resolved++
            } else {
                byNeed.getOrPut(need) { mutableListOf() }.add(item)
            }
        }

        val needCount = items.size - resolved
        val lines = mutableListOf<String>()
        lines += "Unimplemented / unresolved NDK needs: $needCount of $total imports"
        if (summary.incomplete) {
            lines += "Only ${items.size} of $total imports are listed; analysis may be incomplete."
        }
        if (needCount == 0) {
            lines += "No unresolved providers in the listed imports. Verified exports do not prove the game linked or will run."
            return lines.joinToString("\n")
        }
        lines += "Resolved providers are hidden. Export checks do not prove guest slot binding or game compatibility."
        lines += ""

        // Ordered from most actionable (stubs / unmapped) to least.
        section(lines, byNeed[Need.STUB], "STUBS - explicit unimplemented handlers, need a real body") { item ->
            handlerOf(item)
        }
        section(lines, byNeed[Need.UNMAPPED], "UNMAPPED - no provider at all, need a mapping + implementation") { item ->
            item.optString("reason").takeIf { it.isNotBlank() }?.let { "($it)" }.orEmpty()
        }
        section(lines, byNeed[Need.SEMANTIC], "SEMANTIC REWRITES - need a real API/object-lifecycle rewrite") { item ->
            targetOf(item)
        }
        section(lines, byNeed[Need.COMPILER_RUNTIME], "COMPILER-RUNTIME - need the NDK compiler-rt/libunwind runtime") { item ->
            targetOf(item)
        }
        section(lines, byNeed[Need.GUEST_ADAPTER], "GUEST ADAPTERS - bounded adapter only, semantics incomplete") { item ->
            adapterOf(item)
        }
        section(lines, byNeed[Need.NDK_UNVERIFIED], "NDK CANDIDATES - same-name export, NOT yet verified on this device") { item ->
            targetOf(item)
        }
        section(lines, byNeed[Need.IMPLEMENTATION_UNVERIFIED], "IMPLEMENTED - export not resolved on this device") { item ->
            targetOf(item)
        }
        return lines.joinToString("\n")
    }

    /** Returns the need for an import, or null when it is already fully resolved. */
    private fun needFor(item: JSONObject): Need? {
        return when (item.optString("classification", "UNMAPPED")) {
            "BIONIC_SYMBOL_CANDIDATE" ->
                if (item.optBoolean("verifiedOnDevice")) null else Need.NDK_UNVERIFIED
            "IMPLEMENTED_API_REPLACEMENT_AVAILABLE" ->
                if (item.optBoolean("runtimeVerified")) null else Need.IMPLEMENTATION_UNVERIFIED
            "COMPAT_VERIFIED_HANDLER_RESOLVED" -> null
            "SEMANTIC_REWRITE_CANDIDATE" -> Need.SEMANTIC
            "COMPILER_RUNTIME_CANDIDATE" -> Need.COMPILER_RUNTIME
            "GUEST_RUNTIME_ADAPTER_CATALOGUED" -> Need.GUEST_ADAPTER
            "COMPAT_STUB_HANDLER_REGISTERED" -> Need.STUB
            else -> Need.UNMAPPED
        }
    }

    private fun section(
        lines: MutableList<String>,
        group: List<JSONObject>?,
        heading: String,
        detail: (JSONObject) -> String,
    ) {
        if (group.isNullOrEmpty()) return
        lines += "$heading: ${group.size}"
        for (item in group.sortedBy { it.optString("sourceSymbol") }) {
            val symbol = item.optString("sourceSymbol", "<unnamed import>")
            val extra = detail(item)
            lines += if (extra.isBlank()) "  $symbol" else "  $symbol  $extra"
        }
        lines += ""
    }

    private fun targetOf(item: JSONObject): String {
        val library = item.optString("targetLibrary")
        if (library.isNotBlank()) {
            val targetSymbol = item.optString("targetSymbol")
            return if (targetSymbol.isBlank()) "-> $library" else "-> $library:$targetSymbol"
        }
        val targetApi = item.optString("targetApi")
        return if (targetApi.isNotBlank()) "-> $targetApi" else ""
    }

    private fun handlerOf(item: JSONObject): String =
        item.optString("targetSymbol").takeIf { it.isNotBlank() }?.let { "-> stub:$it" }.orEmpty()

    private fun adapterOf(item: JSONObject): String =
        item.optString("targetSymbol").takeIf { it.isNotBlank() }?.let { "-> adapter:$it" }.orEmpty()
}
