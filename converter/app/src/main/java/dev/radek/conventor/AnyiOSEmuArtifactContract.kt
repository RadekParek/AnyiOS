package dev.radek.conventor

import org.json.JSONObject
import java.io.File
import java.security.MessageDigest

/** Independently signed one-game AnyiOSEMU package, never a claim of static conversion. */
internal object AnyiOSEmuArtifactContract {
    const val CONTRACT = "anyiose-single-game-v1"

    fun packageName(sourceHash: String, certificateHash: String): String =
        "dev.anyiose.game.p${sourceHash.take(20)}${certificateHash.take(8)}"

    fun validate(report: JSONObject, dir: File, requestedName: String): File {
        val result = report.optJSONObject("anyioseConversion") ?: error("No AnyiOSEMU APK recorded")
        require(result.optString("status") == "GENERATED" && result.optString("contract") == CONTRACT)
        require(result.optBoolean("installableAndroidPackage", false) &&
            result.optBoolean("embeddedSource", false) && result.optBoolean("embeddedRuntime", false) &&
            !result.optBoolean("completeGameConversion", true) && !result.optBoolean("gamePlayable", true))
        val name = ArtifactNames.anyioseApkFileName(report)
        require(requestedName == name && result.optString("artifact") == name)
        val sourceHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
        val signer = result.optString("certificateSha256")
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && signer.matches(Regex("[0-9a-f]{64}")))
        require(report.optJSONObject("application")?.optString("sha256") == sourceHash &&
            result.optString("sourceSha256") == sourceHash && result.optString("package") == packageName(sourceHash, signer))
        val root = dir.canonicalFile
        val file = File(root, name).canonicalFile
        require(file.parentFile == root && file.isFile)
        require(result.optString("sha256").matches(Regex("[0-9a-f]{64}")) && sha256(file) == result.optString("sha256"))
        return file
    }

    /** Share the retained IPA under the expansion filename, without a second converter-side copy. */
    fun validateExpansion(report: JSONObject, dir: File, requestedName: String): File {
        validate(report, dir, ArtifactNames.anyioseApkFileName(report))
        val conversion = report.getJSONObject("anyioseConversion")
        val expansion = conversion.optJSONObject("expansionObb") ?: error("No expansion file is recorded")
        val packageName = conversion.optString("package")
        val name = ArtifactNames.obbFileName(packageName, ArtifactNames.ANYIOSE_VERSION_CODE)
        val sourceHash = conversion.optString("sourceSha256")
        require(requestedName == name && expansion.optString("name") == name &&
            expansion.optString("sha256") == sourceHash && !expansion.optBoolean("required", true))
        val root = dir.canonicalFile
        val source = File(root, "source.bin").canonicalFile
        require(source.parentFile == root && source.isFile && source.length() == expansion.optLong("bytes", -1) &&
            sha256(source) == sourceHash) { "The retained expansion payload is missing or has changed" }
        return source
    }

    fun sha256(file: File): String = file.inputStream().buffered().use { input ->
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(65536)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            digest.update(buffer, 0, count)
        }
        digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
