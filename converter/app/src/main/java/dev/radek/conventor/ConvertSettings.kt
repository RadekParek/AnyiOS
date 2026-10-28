package dev.radek.conventor

import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Per-library-entry choices; the immutable snapshot is signed into each standalone APK. */
internal data class ConvertSettings(
    val classic43: Boolean = false,
    val offline: Boolean = false,
    val performance: Boolean = false,
    val orientation: String = "default",
) {
    init { require(orientation in setOf("default", "portrait", "landscape")) { "Unknown startup rotation" } }

    fun toJson() = JSONObject().put("classic43", classic43).put("offline", offline)
        .put("performance", performance).put("orientation", orientation)

    companion object {
        private const val FILE = "convert-settings.json"
        fun fromJson(json: JSONObject): ConvertSettings {
            fun flag(name: String): Boolean {
                if (!json.has(name)) return false
                return json.opt(name) as? Boolean ?: throw IllegalArgumentException("Invalid convert setting: $name")
            }
            val orientation = if (json.has("orientation")) json.opt("orientation") as? String
                ?: throw IllegalArgumentException("Invalid startup rotation") else "default"
            return ConvertSettings(flag("classic43"), flag("offline"), flag("performance"), orientation)
        }
        fun load(dir: File): ConvertSettings {
            val file = File(dir, FILE)
            if (!file.exists()) return ConvertSettings()
            require(file.isFile && file.length() in 1..8192) { "Invalid convert settings file" }
            return fromJson(JSONObject(file.readText(Charsets.UTF_8)))
        }
        fun save(dir: File, settings: ConvertSettings) {
            require(dir.isDirectory) { "Game entry not found" }
            val pending = File.createTempFile(".convert-settings-", ".tmp", dir)
            try {
                FileOutputStream(pending).use { stream ->
                    stream.write(settings.toJson().toString().toByteArray(Charsets.UTF_8))
                    stream.fd.sync()
                }
                try {
                    Files.move(pending.toPath(), File(dir, FILE).toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
                } catch (_: AtomicMoveNotSupportedException) {
                    Files.move(pending.toPath(), File(dir, FILE).toPath(), StandardCopyOption.REPLACE_EXISTING)
                }
            } finally { pending.delete() }
        }
    }
}
