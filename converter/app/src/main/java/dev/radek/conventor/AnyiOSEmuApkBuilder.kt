package dev.radek.conventor

import android.content.Context
import android.graphics.BitmapFactory
import com.android.apksig.ApkSigner
import com.android.apksig.ApkVerifier
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipFile

/** Packages the unchanged IPA plus the exact installed AnyiOSEMU host, libraries and assets. */
internal class AnyiOSEmuApkBuilder(private val context: Context) {
    companion object {
        private val DEX = Regex("classes[0-9]+\\.dex")
        private const val MAX_ASSET = 128L * 1024 * 1024
        private val requiredAssets = setOf(
            "assets/LICENSE", "assets/anyiose-runtime-notices.txt",
            "assets/anyiose-rust-dependencies.txt", "assets/anyiose-dynarmic-license.txt",
            "assets/AnyiOSEMU_default_options.txt", "assets/AnyiOSEMU_dylibs/COPYING.libstdcxx",
            "assets/AnyiOSEMU_fonts/LICENSE.noto", "assets/vendor/SDL/LICENSE.txt",
            "assets/angle-notices/INDEX.txt",
        )
        private val prefixes = listOf("assets/AnyiOSEMU_dylibs/", "assets/AnyiOSEMU_fonts/",
            "assets/vendor/", "assets/angle-notices/")
        private val exact = requiredAssets + setOf("assets/AnyiOSEMU_wallpaper.png", "assets/angle-revision.txt")

        /** Fail closed on unknown/protected executable metadata, without rejecting other compatibility gaps. */
        internal fun requireSupportedSource(report: JSONObject) {
            val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: error("Analyze the IPA first")
            require(slices.length() > 0) { "No Mach-O slices were analyzed" }
            var arm32 = false
            for (i in 0 until slices.length()) {
                val slice = slices.getJSONObject(i)
                require(slice.has("encrypted") && !slice.optBoolean("encrypted", true)) {
                    "Encrypted or unverified executables cannot be packaged; DRM is not bypassed"
                }
                if (slice.optString("architecture").lowercase() in setOf("armv6", "armv7", "armv7s", "armv7f", "arm32")) arm32 = true
            }
            require(arm32) { "AnyiOSEMU requires an ARM32 slice; ARM64-only apps are unsupported" }
            val dependencies = report.optJSONObject("dependencies") ?: error("Embedded images were not analyzed")
            require(dependencies.optInt("failedImageCount", -1) == 0) { "An embedded executable could not be verified" }
            val nodes = dependencies.optJSONArray("nodes") ?: error("Embedded image inventory is missing")
            for (i in 0 until nodes.length()) {
                val embedded = nodes.optJSONObject(i)?.optJSONObject("analysis")?.optJSONArray("slices")
                    ?: error("An embedded executable could not be verified")
                require(embedded.length() > 0) { "An embedded executable has no verified slices" }
                for (j in 0 until embedded.length()) {
                    val slice = embedded.getJSONObject(j)
                    require(slice.has("encrypted") && !slice.optBoolean("encrypted", true)) {
                        "A bundled executable is encrypted or unverified; DRM is not bypassed"
                    }
                }
            }
        }
    }

    fun build(dir: File, progress: (Int, String) -> Unit = { _, _ -> }): JSONObject {
        val source = File(dir, "source.bin")
        require(source.isFile && source.length() > 0) { "Retained IPA not found" }
        val report = JSONObject(File(dir, "report.json").readText())
        val app = report.optJSONObject("application") ?: error("Missing application metadata")
        val sourceHash = report.optJSONObject("source")?.optString("sha256").orEmpty()
        require(sourceHash.matches(Regex("[0-9a-f]{64}")) && app.optString("sha256") == sourceHash)
        val name = ArtifactNames.anyioseApkFileName(report)
        val result = File(dir, name)
        val workspace = File(dir, "anyiose-packaging-workspace")
        val unsigned = File(dir, ".anyiose-unsigned.apk")
        val signed = File(dir, ".anyiose-signed.apk")
        val pending = File(dir, ".$name.pending")
        val backup = File(dir, ".$name.previous")
        val library = Library(context)
        fun update(percent: Int, status: String, message: String) {
            report.put("anyioseBuildProgress", JSONObject().put("percent", percent).put("status", status)
                .put("message", message).put("updatedAt", java.time.Instant.now().toString()))
            library.save(dir, report)
            progress(percent, message)
        }
        var replaced = false
        var complete = false
        val previousConversion = report.optJSONObject("anyioseConversion")?.let { JSONObject(it.toString()) }
        try {
            if (backup.isFile) { if (!result.exists()) backup.renameTo(result) else backup.delete() }
            workspace.deleteRecursively(); require(workspace.mkdirs())
            unsigned.delete(); signed.delete(); pending.delete()
            update(4, "CONVERTING", "Checking the retained IPA and its ARM32 executable")
            require(AnyiOSEmuArtifactContract.sha256(source) == sourceHash) { "The IPA no longer matches the analysis" }
            requireSupportedSource(report)
            require(source.length() < 2L * 1024 * 1024 * 1024) { "The self-contained APK exceeds the supported on-device size" }
            SafeZip.requireStorage(dir, source.length() * 4 + 512L * 1024 * 1024)
            update(10, "CONVERTING", "Staging AnyiOSEMU and the original IPA")
            val identity = PlaceholderSigningIdentity.loadOrCreate(File(context.noBackupFilesDir, "placeholder-apk-signing-identity.bin"))
            val certificateHash = MessageDigest.getInstance("SHA-256").digest(identity.certificate.encoded)
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
            val packageName = AnyiOSEmuArtifactContract.packageName(sourceHash, certificateHash)
            val label = app.optString("name").filterNot { Character.isISOControl(it) }.take(160).trim().ifBlank { "Game" }
            val libraries = CompatibilityRuntime.extractAnyiOSEmuInstalled(context, File(workspace, "lib"))
            val runtimeAssets = stageRuntimeAssets(File(workspace, "assets"))

            update(35, "PACKAGING", "Preparing the standalone game launcher")
            fun asset(path: String, limit: Int = 32 * 1024 * 1024): ByteArray = context.assets.open("anyiose-template/$path").use {
                val bytes = it.readBytes()
                require(bytes.isNotEmpty() && bytes.size <= limit) { "Missing or oversized template: $path" }
                bytes
            }
            val manifest = BinaryXmlManifest.customize(asset("AndroidManifest.xml"), packageName, label, 29, 35)
            val resources = ResourceTablePackagePatcher.customize(asset("resources.arsc"), packageName)
            val dexNames = asset("template-dex-entries.txt", 16 * 1024).toString(Charsets.UTF_8).lineSequence()
                .map { it.substringBefore(':').trim() }.filter { it == "classes.dex" || DEX.matches(it) }
                .distinct().toList()
            require(dexNames.firstOrNull() == "classes.dex") { "Standalone launcher has no DEX" }
            val dexes = dexNames.map { it to asset(it) }
            val dexText = dexes.joinToString("") { String(it.second, Charsets.ISO_8859_1) }
            require(dexText.contains("Lorg/podnative/StandaloneLauncherActivity;") &&
                dexText.contains("Lorg/podnative/StandaloneGameActivity;") &&
                dexText.contains("Lorg/podnative/StandaloneApplication;") &&
                dexText.contains("Lorg/podnative/StandaloneOptions;") &&
                dexText.contains("Lorg/libsdl/app/SDLActivity;")) { "Standalone launcher or SDL is missing" }
            val iconPath = SafeZip.validateName(asset("icon-entry-path.txt", 1024).toString(Charsets.UTF_8))
            require(iconPath.startsWith("res/") && iconPath.endsWith("/generated_anyiose_icon.png"))
            val icon = File(dir, "icon.png").takeIf { it.isFile && it.length() in 1..(16L * 1024 * 1024) }
                ?.readBytes()?.takeIf {
                    val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                    BitmapFactory.decodeByteArray(it, 0, it.size, options)
                    options.outWidth in 1..8192 && options.outHeight in 1..8192
                } ?: asset("fallback-icon.png", 16 * 1024 * 1024)
            val metadata = JSONObject().put("contract", AnyiOSEmuArtifactContract.CONTRACT)
                .put("sourceSha256", sourceHash).put("sourceBytes", source.length())
                .put("runtime", "AnyiOSEMU").put("gameplayVerified", false)
            val entries = ArrayList<AlignedApkZip.Entry>()
            entries += AlignedApkZip.Entry("AndroidManifest.xml", manifest)
            entries += AlignedApkZip.Entry("resources.arsc", resources)
            entries += AlignedApkZip.Entry(iconPath, icon)
            dexes.forEach { (path, bytes) -> entries += AlignedApkZip.Entry(path, bytes) }
            libraries.forEach { entries += AlignedApkZip.Entry.stream(it.apkPath, it.file, alignment = AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT) }
            runtimeAssets.forEach { (path, file) -> entries += AlignedApkZip.Entry.stream(path, file, compressed = true) }
            entries += AlignedApkZip.Entry("assets/anyiose-game.json", metadata.toString().toByteArray(), compressed = true)
            entries += AlignedApkZip.Entry.stream("assets/anyiose-game.ipa", source, compressed = false)
            val aligned = buildMap {
                put("AndroidManifest.xml", AlignedApkZip.ALIGNMENT); put("resources.arsc", AlignedApkZip.ALIGNMENT)
                put(iconPath, AlignedApkZip.ALIGNMENT)
                dexNames.forEach { put(it, AlignedApkZip.ALIGNMENT) }
                libraries.forEach { put(it.apkPath, AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT) }
                put("assets/anyiose-game.ipa", AlignedApkZip.ALIGNMENT)
            }
            AlignedApkZip.write(unsigned, entries)
            AlignedApkZip.verify(unsigned, entries.map { it.name }.toSet(), aligned)
            update(65, "SIGNING", "Signing the standalone AnyiOSEMU APK")
            val signer = ApkSigner.SignerConfig.Builder("AnyiOSEMU", identity.privateKey, listOf(identity.certificate)).build()
            ApkSigner.Builder(listOf(signer)).setInputApk(unsigned).setOutputApk(signed).setMinSdkVersion(29)
                .setV1SigningEnabled(true).setV2SigningEnabled(true).setV3SigningEnabled(true)
                .setAlignmentPreserved(true).setLibraryPageAlignmentBytes(AlignedApkZip.NATIVE_LIBRARY_ALIGNMENT).build().sign()
            update(85, "VERIFYING", "Checking signature, launcher and embedded runtime")
            AlignedApkZip.verifyAlignedEntries(signed, aligned)
            val verification = ApkVerifier.Builder(signed).build().verify()
            require(verification.isVerified && verification.signerCertificates.isNotEmpty()) { "APK signature verification failed" }
            val actualCert = MessageDigest.getInstance("SHA-256").digest(verification.signerCertificates.first().encoded)
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
            require(actualCert == certificateHash) { "Unexpected APK signer" }
            @Suppress("DEPRECATION")
            val info = context.packageManager.getPackageArchiveInfo(signed.path, android.content.pm.PackageManager.GET_ACTIVITIES)
                ?: error("Android could not parse the standalone APK")
            require(info.packageName == packageName && info.applicationInfo?.className == "org.podnative.StandaloneApplication" &&
                info.activities.orEmpty().any { it.name == "org.podnative.StandaloneLauncherActivity" } &&
                info.activities.orEmpty().any { it.name == "org.podnative.StandaloneGameActivity" }) { "Standalone launcher is invalid" }
            require(InstallAudit.inspect(context, signed, packageName).blockers.isEmpty()) { "Standalone APK is not installable on this device" }
            signed.copyTo(pending, overwrite = true)
            if (result.exists()) require(result.renameTo(backup)) { "Cannot preserve the previous APK" }
            require(pending.renameTo(result)) { "Cannot save the standalone APK" }
            replaced = true
            val conversion = JSONObject().put("status", "GENERATED").put("contract", AnyiOSEmuArtifactContract.CONTRACT)
                .put("artifact", name).put("package", packageName).put("sourceSha256", sourceHash)
                .put("sha256", AnyiOSEmuArtifactContract.sha256(result)).put("certificateSha256", certificateHash)
                .put("embeddedSource", true).put("embeddedRuntime", true).put("installableAndroidPackage", true)
                .put("completeGameConversion", false).put("gamePlayable", false).put("runtimeExecution", "NOT_TESTED")
                .put("graphics", "Bundled ANGLE with native GLES fallback").put("targetAbi", "arm64-v8a")
            report.put("anyioseConversion", conversion)
            update(100, "GENERATED", "Standalone AnyiOSEMU APK ready; gameplay requires device testing")
            complete = true; backup.delete()
            return conversion
        } catch (error: Throwable) {
            if (!complete) {
                if (replaced) result.delete()
                if (backup.isFile && !result.exists()) backup.renameTo(result)
                if (previousConversion == null) report.remove("anyioseConversion")
                else report.put("anyioseConversion", previousConversion)
            }
            val message = if (error is OutOfMemoryError) "Not enough memory to package this game" else error.message ?: error.javaClass.simpleName
            try { update(0, "FAILED", message) } catch (_: Throwable) { }
            if (error is OutOfMemoryError) throw IllegalStateException(message)
            throw error
        } finally {
            workspace.deleteRecursively(); unsigned.delete(); signed.delete(); pending.delete()
        }
    }

    private fun stageRuntimeAssets(destination: File): List<Pair<String, File>> {
        require(destination.mkdirs() || destination.isDirectory)
        val info = context.applicationInfo
        val apks = listOfNotNull(info.sourceDir?.let(::File)) + info.splitSourceDirs.orEmpty().map(::File)
        val found = linkedMapOf<String, File>()
        for (apk in apks.distinctBy { it.absolutePath }) ZipFile(apk).use { zip ->
            for (entry in zip.entries().asSequence()) {
                val name = entry.name
                if (entry.isDirectory || (name !in exact && prefixes.none { name.startsWith(it) })) continue
                SafeZip.validateName(name)
                require(name !in found && entry.size in 0..MAX_ASSET) { "Invalid or duplicate runtime asset: $name" }
                val target = File(destination, name.removePrefix("assets/"))
                require(target.parentFile!!.mkdirs() || target.parentFile!!.isDirectory)
                zip.getInputStream(entry).use { input -> target.outputStream().use { output ->
                    val count = input.copyTo(output)
                    require(count == entry.size) { "Truncated runtime asset: $name" }
                } }
                found[name] = target
            }
        }
        require(found.keys.containsAll(requiredAssets)) { "Installed AnyiOSEMU is missing runtime assets: ${requiredAssets - found.keys}" }
        return found.toList()
    }
}
