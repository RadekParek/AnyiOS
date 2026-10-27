package dev.radek.conventor

import java.nio.file.Files
import java.security.MessageDigest
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [29], manifest = Config.NONE)
class AnyiOSEmuExpansionTest {
    @Test fun optionalExpansionShareUsesOnlyTheVerifiedRetainedIpa() {
        val dir = Files.createTempDirectory("anyiose-obb").toFile()
        try {
            val bytes = "authorized IPA ZIP fixture".toByteArray()
            val source = dir.resolve("source.bin").apply { writeBytes(bytes) }
            val hash = MessageDigest.getInstance("SHA-256").digest(bytes)
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
            val signer = "b".repeat(64)
            val report = JSONObject().put("source", JSONObject().put("sha256", hash).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", hash))
            val apkName = ArtifactNames.anyioseApkFileName(report)
            val apk = dir.resolve(apkName).apply { writeText("signed fixture") }
            val packageName = AnyiOSEmuArtifactContract.packageName(hash, signer)
            val obbName = ArtifactNames.obbFileName(packageName, ArtifactNames.ANYIOSE_VERSION_CODE)
            val conversion = JSONObject().put("status", "GENERATED").put("contract", AnyiOSEmuArtifactContract.CONTRACT)
                .put("artifact", apkName).put("sourceSha256", hash).put("certificateSha256", signer)
                .put("package", packageName).put("sha256", AnyiOSEmuArtifactContract.sha256(apk))
                .put("embeddedSource", true).put("embeddedRuntime", true).put("installableAndroidPackage", true)
                .put("completeGameConversion", false).put("gamePlayable", false)
                .put("expansionObb", JSONObject().put("name", obbName).put("sha256", hash)
                    .put("bytes", bytes.size).put("required", false))
            report.put("anyioseConversion", conversion)
            assertEquals(source.canonicalFile, AnyiOSEmuArtifactContract.validateExpansion(report, dir, obbName))
            assertThrows(IllegalArgumentException::class.java) {
                AnyiOSEmuArtifactContract.validateExpansion(report, dir, "../$obbName")
            }
            source.writeText("tampered")
            assertThrows(IllegalArgumentException::class.java) {
                AnyiOSEmuArtifactContract.validateExpansion(report, dir, obbName)
            }
            // Old APKs and a missing source still validate as self-contained.
            assertEquals(apk.canonicalFile, AnyiOSEmuArtifactContract.validate(report, dir, apkName))
        } finally { dir.deleteRecursively() }
    }
}
