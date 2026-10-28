package dev.radek.conventor

import android.content.pm.ProviderInfo
import android.net.Uri
import android.os.ParcelFileDescriptor
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File
import java.security.MessageDigest

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ResultProviderPlaceholderTest {
    @Test fun servesOnlyTheExplicitPlaceholderNameUnderItsOwnMetadataContract() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "games/game-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "org.podnative.converter.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val report = makeReport(directory)
            File(directory, "report.json").writeText(report.toString())
            val placeholderName = ArtifactNames.placeholderApkFileName(report)
            val uri = Uri.Builder().scheme("content").authority("org.podnative.converter.results")
                .appendPath(directory.name).appendPath(placeholderName).build()

            val descriptor = provider.openFile(uri, "r")
            val contents = ParcelFileDescriptor.AutoCloseInputStream(descriptor).use { it.readBytes() }
            assertEquals("verified-placeholder-fixture", String(contents))
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun placeholderCannotBeServedAtTheCompleteGameHostPath() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "games/game-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "org.podnative.converter.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val report = makeReport(directory)
            val hostName = ArtifactNames.apkFileName(report)
            File(directory, hostName).writeText("placeholder copy")
            report.put("hostConversion", JSONObject()
                .put("status", "ATTACHED")
                .put("completeGameConversion", true)
                .put("contract", "placeholder-v1")
                .put("artifact", hostName))
            File(directory, "report.json").writeText(report.toString())
            val uri = Uri.Builder().scheme("content").authority("org.podnative.converter.results")
                .appendPath(directory.name).appendPath(hostName).build()
            assertThrows(IllegalArgumentException::class.java) { provider.openFile(uri, "r") }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test fun standaloneExpansionIsSharedWithItsObbNameAndVerifiedContents() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "games/game-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "org.podnative.converter.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val bytes = "verified IPA expansion".toByteArray()
            val source = File(directory, "source.bin").apply { writeBytes(bytes) }
            val sourceHash = AnyiOSEmuArtifactContract.sha256(source)
            val signer = "e".repeat(64)
            val report = JSONObject().put("source", JSONObject().put("sha256", sourceHash).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", sourceHash))
            val apkName = ArtifactNames.anyioseApkFileName(report)
            val apk = File(directory, apkName).apply { writeText("signed standalone fixture") }
            val pkg = AnyiOSEmuArtifactContract.packageName(sourceHash, signer)
            val obbName = ArtifactNames.obbFileName(pkg, ArtifactNames.ANYIOSE_VERSION_CODE)
            report.put("anyioseConversion", JSONObject().put("status", "GENERATED")
                .put("contract", AnyiOSEmuArtifactContract.CONTRACT).put("artifact", apkName)
                .put("package", pkg).put("sourceSha256", sourceHash).put("certificateSha256", signer)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(apk))
                .put("embeddedSource", true).put("embeddedRuntime", true).put("installableAndroidPackage", true)
                .put("completeGameConversion", false).put("gamePlayable", false)
                .put("expansionObb", JSONObject().put("name", obbName).put("bytes", bytes.size)
                    .put("sha256", sourceHash).put("required", false)))
            File(directory, "report.json").writeText(report.toString())
            val uri = Uri.Builder().scheme("content").authority("org.podnative.converter.results")
                .appendPath(directory.name).appendPath(obbName).build()
            assertEquals("application/octet-stream", provider.getType(uri))
            provider.query(uri, null, null, null, null).use { cursor ->
                cursor!!.moveToFirst()
                assertEquals(obbName, cursor.getString(cursor.getColumnIndexOrThrow(android.provider.OpenableColumns.DISPLAY_NAME)))
            }
            val contents = ParcelFileDescriptor.AutoCloseInputStream(provider.openFile(uri, "r")).use { it.readBytes() }
            assertEquals(String(bytes), String(contents))
            source.writeText("corrupted source")
            assertThrows(IllegalArgumentException::class.java) { provider.openFile(uri, "r") }
        } finally { directory.deleteRecursively() }
    }

    @Test fun onDeviceCompleteExpansionIsSharedUnderItsOwnName() {
        val context = RuntimeEnvironment.getApplication()
        val directory = File(context.filesDir, "games/game-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider()
        provider.attachInfo(context, ProviderInfo().apply {
            authority = "org.podnative.converter.results"
            exported = false
            grantUriPermissions = true
        })
        try {
            val source = "a".repeat(64)
            val signer = "b".repeat(64)
            val packageName = "dev.radek.converted.p${source.take(20)}${signer.take(8)}"
            val report = JSONObject().put("source", JSONObject().put("sha256", source).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", source))
            val apkName = ArtifactNames.apkFileName(report)
            val apk = File(directory, apkName).apply { writeText("APK") }
            val name = ArtifactNames.obbFileName(packageName, ArtifactNames.CONVERTED_VERSION_CODE)
            val obb = File(directory, name).apply { writeText("bounded resources") }
            report.put("deviceConversion", JSONObject().put("status", "GENERATED_ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true)
                .put("artifact", apkName).put("package", packageName).put("sourceSha256", source)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(apk))
                .put("signing", JSONObject().put("certificateSha256", signer))
                .put("expansionObb", JSONObject().put("name", name).put("bytes", obb.length())
                    .put("sha256", AnyiOSEmuArtifactContract.sha256(obb)).put("required", false)))
            report.put("hostConversion", JSONObject().put("status", "ATTACHED").put("origin", "ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true)
                .put("artifact", apkName).put("package", packageName))
            File(directory, "report.json").writeText(report.toString())
            val uri = Uri.Builder().scheme("content").authority("org.podnative.converter.results")
                .appendPath(directory.name).appendPath(name).build()
            assertEquals("application/octet-stream", provider.getType(uri))
            assertEquals("bounded resources", ParcelFileDescriptor.AutoCloseInputStream(provider.openFile(uri, "r"))
                .bufferedReader().use { it.readText() })
            obb.writeText("tampered")
            assertThrows(IllegalArgumentException::class.java) { provider.openFile(uri, "r") }
        } finally { directory.deleteRecursively() }
    }

    @Test fun sameNamedRuntimeExpansionsRequireAnExplicitKind() {
        val context = RuntimeEnvironment.getApplication()
        val dir = File(context.filesDir, "games/game-12345678-1234-1234-1234-123456789abc").apply { mkdirs() }
        val provider = ResultProvider().apply { attachInfo(context, ProviderInfo().apply {
            authority = "org.podnative.converter.results"; exported = false; grantUriPermissions = true
        }) }
        try {
            val source = File(dir, "source.bin").apply { writeText("IPA bytes") }
            val hash = AnyiOSEmuArtifactContract.sha256(source); val signer = "b".repeat(64)
            val pkg = "com.lego.riseofthesnakes"
            val report = JSONObject().put("source", JSONObject().put("sha256", hash).put("originalName", "Game.ipa"))
                .put("application", JSONObject().put("sha256", hash).put("bundleId", pkg))
            val standaloneName = ArtifactNames.anyioseApkFileName(report)
            val standalone = File(dir, standaloneName).apply { writeText("standalone") }
            val obbName = ArtifactNames.obbFileName(pkg, 1)
            report.put("anyioseConversion", JSONObject().put("status", "GENERATED").put("contract", AnyiOSEmuArtifactContract.CONTRACT)
                .put("artifact", standaloneName).put("package", pkg).put("sourceSha256", hash).put("certificateSha256", signer)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(standalone)).put("embeddedSource", true).put("embeddedRuntime", true)
                .put("installableAndroidPackage", true).put("completeGameConversion", false).put("gamePlayable", false)
                .put("expansionObb", JSONObject().put("name", obbName).put("bytes", source.length()).put("sha256", hash).put("required", false)))
            val boundedName = ArtifactNames.apkFileName(report)
            val boundedApk = File(dir, boundedName).apply { writeText("bounded APK") }
            val boundedObb = File(dir, "expansion-bounded/$obbName").apply { parentFile.mkdirs(); writeText("bounded resources") }
            report.put("deviceConversion", JSONObject().put("status", "GENERATED_ON_DEVICE").put("contract", "complete-game-v1")
                .put("completeGameConversion", true).put("artifact", boundedName).put("package", pkg).put("sourceSha256", hash)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(boundedApk)).put("signing", JSONObject().put("certificateSha256", signer))
                .put("expansionObb", JSONObject().put("storage", "bounded").put("name", obbName).put("bytes", boundedObb.length())
                    .put("sha256", AnyiOSEmuArtifactContract.sha256(boundedObb)).put("required", false)))
            report.put("hostConversion", JSONObject().put("status", "ATTACHED").put("origin", "ON_DEVICE")
                .put("contract", "complete-game-v1").put("completeGameConversion", true).put("artifact", boundedName).put("package", pkg))
            File(dir, "report.json").writeText(report.toString())
            fun uri(kind: String?) = Uri.Builder().scheme("content").authority("org.podnative.converter.results")
                .appendPath(dir.name).appendPath(obbName).apply { if (kind != null) appendQueryParameter("kind", kind) }.build()
            fun read(kind: String) = ParcelFileDescriptor.AutoCloseInputStream(provider.openFile(uri(kind), "r"))
                .bufferedReader().use { it.readText() }
            assertEquals("IPA bytes", read("anyiose"))
            assertEquals("bounded resources", read("bounded"))
            assertThrows(IllegalArgumentException::class.java) { provider.openFile(uri(null), "r") }
        } finally { dir.deleteRecursively() }
    }

    private fun makeReport(directory: File): JSONObject {
        val content = "verified-placeholder-fixture".toByteArray()
        val sourceHash = "b".repeat(64)
        val report = JSONObject()
            .put("source", JSONObject().put("sha256", sourceHash).put("originalName", "Test.ipa"))
            .put("application", JSONObject().put("sha256", sourceHash))
        val fileName = ArtifactNames.placeholderApkFileName(report)
        File(directory, fileName).writeBytes(content)
        val digest = MessageDigest.getInstance("SHA-256").digest(content)
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        val certificateHash = "d".repeat(64)
        report.put("placeholderConversion", JSONObject()
            .put("status", "GENERATED")
            .put("placeholderOnly", true)
            .put("completeGameConversion", false)
            .put("gameCodeIncluded", false)
            .put("gameCodeRecompiled", false)
            .put("gamePlayable", false)
            .put("installableAndroidPackage", true)
            .put("artifact", fileName)
            .put("package", "dev.radek.placeholder.p${sourceHash.take(24)}${certificateHash.take(8)}")
            .put("sourceSha256", sourceHash)
            .put("sha256", digest)
            .put("signing", JSONObject().put("certificateSha256", certificateHash)))
        return report
    }
}
