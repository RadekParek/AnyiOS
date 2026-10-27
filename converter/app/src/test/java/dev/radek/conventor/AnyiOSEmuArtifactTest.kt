package dev.radek.conventor

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [29], manifest = Config.NONE)
class AnyiOSEmuArtifactTest {
    private fun slice(architecture: String, encrypted: Boolean = false) =
        JSONObject().put("architecture", architecture).put("encrypted", encrypted)

    private fun analysis(vararg slices: JSONObject) = JSONObject()
        .put("machO", JSONObject().put("slices", JSONArray().apply { slices.forEach(::put) }))
        .put("dependencies", JSONObject().put("failedImageCount", 0).put("nodes", JSONArray()))

    @Test fun onlyAnalyzedUnencryptedArm32GamesArePackaged() {
        AnyiOSEmuApkBuilder.requireSupportedSource(analysis(slice("arm64"), slice("armv7")))
        assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuApkBuilder.requireSupportedSource(analysis(slice("arm64"))) }
        assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuApkBuilder.requireSupportedSource(analysis(slice("armv7", true))) }
        assertThrows(IllegalArgumentException::class.java) {
            AnyiOSEmuApkBuilder.requireSupportedSource(analysis(JSONObject().put("architecture", "armv7")))
        }
        val unverified = analysis(slice("armv7"))
        unverified.getJSONObject("dependencies").put("failedImageCount", 1)
        assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuApkBuilder.requireSupportedSource(unverified) }
        val embedded = analysis(slice("armv7"))
        embedded.getJSONObject("dependencies").getJSONArray("nodes").put(
            JSONObject().put("analysis", JSONObject().put("slices", JSONArray().put(slice("armv7", true)))))
        assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuApkBuilder.requireSupportedSource(embedded) }
    }

    @Test fun separateStandaloneArtifactIsBoundToSourceSignerAndDigest() {
        val dir = Files.createTempDirectory("anyiose-artifact").toFile()
        try {
            val sourceHash = "a".repeat(64)
            val signer = "b".repeat(64)
            val report = JSONObject().put("source", JSONObject().put("sha256", sourceHash).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", sourceHash))
            val name = ArtifactNames.anyioseApkFileName(report)
            assertEquals("Game-anyiose.apk", name)
            val apk = dir.resolve(name).apply { writeText("fixture") }
            val conversion = JSONObject().put("status", "GENERATED").put("contract", AnyiOSEmuArtifactContract.CONTRACT)
                .put("artifact", name).put("sourceSha256", sourceHash).put("certificateSha256", signer)
                .put("package", AnyiOSEmuArtifactContract.packageName(sourceHash, signer))
                .put("sha256", AnyiOSEmuArtifactContract.sha256(apk)).put("embeddedSource", true).put("embeddedRuntime", true)
                .put("installableAndroidPackage", true).put("completeGameConversion", false).put("gamePlayable", false)
            report.put("anyioseConversion", conversion)
            assertEquals(apk.canonicalFile, AnyiOSEmuArtifactContract.validate(report, dir, name))
            assertThrows(IllegalArgumentException::class.java) {
                AnyiOSEmuArtifactContract.validate(report, dir, ArtifactNames.gameApkFileName(report))
            }
            conversion.put("embeddedRuntime", false)
            assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuArtifactContract.validate(report, dir, name) }
            conversion.put("embeddedRuntime", true)
            apk.writeText("tampered")
            assertThrows(IllegalArgumentException::class.java) { AnyiOSEmuArtifactContract.validate(report, dir, name) }
        } finally { dir.deleteRecursively() }
    }
}
