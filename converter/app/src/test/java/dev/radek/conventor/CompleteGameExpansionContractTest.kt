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
    @Test fun exactBundleIdAndSeparatedSidecarAreValidated() {
        val dir = Files.createTempDirectory("converted-bundle-obb").toFile()
        try {
            val source = "a".repeat(64); val signer = "b".repeat(64)
            val pkg = "com.lego.riseofthesnakes"
            val report = JSONObject().put("source", JSONObject().put("sha256", source).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", source).put("bundleId", pkg))
            val apkName = ArtifactNames.apkFileName(report)
            val apk = File(dir, apkName).apply { writeText("APK") }
            val name = ArtifactNames.obbFileName(pkg, 1)
            val obb = File(dir, "expansion-bounded/$name").apply { parentFile.mkdirs(); writeText("bounded") }
            val expansion = JSONObject().put("name", name).put("storage", "bounded").put("bytes", obb.length())
                .put("sha256", AnyiOSEmuArtifactContract.sha256(obb)).put("required", false)
            report.put("deviceConversion", JSONObject().put("status", "GENERATED_ON_DEVICE").put("contract", "complete-game-v1")
                .put("completeGameConversion", true).put("artifact", apkName).put("package", pkg).put("sourceSha256", source)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(apk)).put("signing", JSONObject().put("certificateSha256", signer))
                .put("expansionObb", expansion))
            report.put("hostConversion", JSONObject().put("status", "ATTACHED").put("origin", "ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true).put("artifact", apkName).put("package", pkg))
            assertEquals(obb.canonicalFile, CompleteGameExpansionContract.validate(report, dir, name))
            expansion.put("storage", "native")
            assertThrows(IllegalArgumentException::class.java) { CompleteGameExpansionContract.validate(report, dir, name) }
        } finally { dir.deleteRecursively() }
    }

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
