package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject

/** Shared user-facing text for failed embedded-image verification. */
internal object ImageDiagnostics {
    fun format(item: JSONObject): String {
        val path = item.optString("path", "unknown").ifBlank { "unknown" }
        val reason = item.optString("reason", "").ifBlank { "the Mach-O image could not be decoded" }
        return "Image $path: $reason"
    }

    fun firstFailure(errors: JSONArray?): String? {
        if (errors == null) return null
        for (index in 0 until errors.length()) {
            val item = errors.optJSONObject(index) ?: continue
            return format(item)
        }
        return null
    }
}
