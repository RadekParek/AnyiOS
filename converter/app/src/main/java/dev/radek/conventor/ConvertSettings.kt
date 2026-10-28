package dev.radek.conventor

import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Per-library-entry choices; the immutable snapshot is signed into each standalone APK. */
internal data class ConvertSettings(
    val classic320: Boolean = false,
    val offline: Boolean = false,
    val performance: Boolean = false,
    val orientation: String = "default",
    val classic1024: Boolean = false,
) {
    init {
        require(orientation in setOf("default", "-90", "90", "180", "portrait")) { "Unknown startup rotation" }
        require(!(classic320 && classic1024)) { "Only one display profile can be selected" }
    }

    fun toJson() = JSONObject().put("classic320", classic320).put("classic1024", classic1024)
        .put("offline", offline).put("performance", performance).put("orientation", orientation)

    companion object {
        private const val FILE = "convert-settings.json"
        fun fromJson(json: JSONObject): ConvertSettings {
            fun flag(name: String): Boolean {
                if (!json.has(name)) return false
                return json.opt(name) as? Boolean ?: throw IllegalArgumentException("Invalid convert setting: $name")
            }
            val orientation = if (json.has("orientation")) json.opt("orientation") as? String
                ?: throw IllegalArgumentException("Invalid startup rotation") else "default"
            // Keep previously saved opt-ins without silently re-enabling any
            // defaults. The retired 4:3 choice becomes the new 3:2 profile;
            // legacy portrait remains available until the user changes it.
            val profile = if (json.has("classic320")) flag("classic320") else flag("classic43")
            val rotation = if (orientation == "landscape") "-90" else orientation
            return ConvertSettings(profile, flag("offline"), flag("performance"), rotation, flag("classic1024"))
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
