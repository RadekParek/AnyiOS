package dev.radek.conventor

import java.io.File
import org.json.JSONObject

/** Share only the resource sidecar of the currently attached on-device bounded APK. */
internal object CompleteGameExpansionContract {
    fun validate(report: JSONObject, directory: File, requestedName: String): File {
        val conversion = report.optJSONObject("deviceConversion") ?: error("No on-device conversion recorded")
        val attached = report.optJSONObject("hostConversion") ?: error("No complete-game APK attached")
        require(conversion.optString("status") == "GENERATED_ON_DEVICE" &&
            conversion.optString("contract") == "complete-game-v1" &&
            conversion.optBoolean("completeGameConversion", false) &&
            attached.optString("status") == "ATTACHED" && attached.optString("origin") == "ON_DEVICE" &&
            attached.optString("contract") == "complete-game-v1" &&
            attached.optBoolean("completeGameConversion", false)) { "No current on-device complete-game APK" }
        val sourceHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
        val appHash = report.optJSONObject("application")?.optString("sha256").orEmpty()
        val signer = conversion.optJSONObject("signing")?.optString("certificateSha256").orEmpty()
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && sourceHash == appHash &&
            conversion.optString("sourceSha256") == sourceHash && signer.matches(Regex("[0-9a-f]{64}")))
        val packageName = "dev.radek.converted.p${sourceHash.take(20)}${signer.take(8)}"
        val apkName = ArtifactNames.apkFileName(report)
        require(conversion.optString("package") == packageName && attached.optString("package") == packageName &&
            conversion.optString("artifact") == apkName && attached.optString("artifact") == apkName)
        val root = directory.canonicalFile
        val apk = File(root, apkName).canonicalFile
        val apkHash = conversion.optString("sha256")
        require(apk.parentFile == root && apk.isFile && apkHash.matches(Regex("[0-9a-f]{64}")) &&
            AnyiOSEmuArtifactContract.sha256(apk) == apkHash) { "The converted APK is missing or has changed" }
        val expansion = conversion.optJSONObject("expansionObb") ?: error("No expansion file is recorded")
        val obbName = ArtifactNames.obbFileName(packageName, ArtifactNames.CONVERTED_VERSION_CODE)
        val file = File(root, obbName).canonicalFile
        val digest = expansion.optString("sha256")
        require(requestedName == obbName && expansion.optString("name") == obbName &&
            !expansion.optBoolean("required", true) && digest.matches(Regex("[0-9a-f]{64}")) &&
            file.parentFile == root && file.isFile && file.length() == expansion.optLong("bytes", -1) &&
            AnyiOSEmuArtifactContract.sha256(file) == digest) { "The expansion is missing or has changed" }
        return file
    }
}
