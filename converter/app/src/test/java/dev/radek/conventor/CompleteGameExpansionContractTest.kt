package dev.radek.conventor

import java.io.File
import java.nio.file.Files
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class CompleteGameExpansionContractTest {
    @Test fun sharesOnlyTheOptionalExpansionBoundToTheCurrentApk() {
        val dir = Files.createTempDirectory("converted-obb").toFile()
        try {
            val source = "a".repeat(64)
            val signer = "b".repeat(64)
            val packageName = "dev.radek.converted.p${source.take(20)}${signer.take(8)}"
            val report = JSONObject()
                .put("source", JSONObject().put("sha256", source).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", source))
            val apkName = ArtifactNames.apkFileName(report)
            val apk = File(dir, apkName).apply { writeText("signed bounded APK fixture") }
            val obbName = ArtifactNames.obbFileName(packageName, ArtifactNames.CONVERTED_VERSION_CODE)
            val obb = File(dir, obbName).apply { writeText("resource ZIP fixture") }
            val expansion = JSONObject().put("name", obbName).put("bytes", obb.length())
                .put("sha256", AnyiOSEmuArtifactContract.sha256(obb)).put("required", false)
            val device = JSONObject().put("status", "GENERATED_ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true)
                .put("artifact", apkName).put("package", packageName).put("sourceSha256", source)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(apk))
                .put("signing", JSONObject().put("certificateSha256", signer)).put("expansionObb", expansion)
            val attached = JSONObject().put("status", "ATTACHED").put("origin", "ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true)
                .put("artifact", apkName).put("package", packageName)
            report.put("deviceConversion", device).put("hostConversion", attached)
            assertEquals(obb.canonicalFile, CompleteGameExpansionContract.validate(report, dir, obbName))
            assertThrows(IllegalArgumentException::class.java) {
                CompleteGameExpansionContract.validate(report, dir, "../$obbName")
            }
            obb.writeText("changed")
            assertThrows(IllegalArgumentException::class.java) {
                CompleteGameExpansionContract.validate(report, dir, obbName)
            }
            obb.writeText("resource ZIP fixture")
            apk.writeText("replaced by another APK")
            assertThrows(IllegalArgumentException::class.java) {
                CompleteGameExpansionContract.validate(report, dir, obbName)
            }
            attached.put("origin", "HOST")
            assertThrows(IllegalArgumentException::class.java) {
                CompleteGameExpansionContract.validate(report, dir, obbName)
            }
        } finally { dir.deleteRecursively() }
    }
}
