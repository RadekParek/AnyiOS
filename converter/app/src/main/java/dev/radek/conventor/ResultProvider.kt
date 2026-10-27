package dev.radek.conventor

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import org.json.JSONObject
import java.io.File

/** Read-only URI access with distinct validation contracts for host and placeholder APKs. */
class ResultProvider : ContentProvider() {
    override fun onCreate() = true

    private fun entry(uri: Uri): File {
        val parts = uri.pathSegments
        require(parts.size == 2 && parts[0].matches(Regex("game-[0-9a-f-]{36}")))
        val root = File(requireNotNull(context).filesDir, "games").canonicalFile
        val entryDirectory = File(root, parts[0]).canonicalFile
        require(entryDirectory.path.startsWith(root.path + File.separator) && entryDirectory.isDirectory)
        val report = JSONObject(File(entryDirectory, "report.json").readText())
        val requestedName = parts[1]
        val anyioseObb = report.optJSONObject("anyioseConversion")?.optJSONObject("expansionObb")?.optString("name")
        if (!anyioseObb.isNullOrBlank() && requestedName == anyioseObb) {
            return AnyiOSEmuArtifactContract.validateExpansion(report, entryDirectory, requestedName)
        }
        val nativeObb = report.optJSONObject("gameRuntimeConversion")?.optJSONObject("expansionObb")?.optString("name")
        if (!nativeObb.isNullOrBlank() && requestedName == nativeObb) {
            return GameRuntimeArtifactContract.validateExpansion(report, entryDirectory, requestedName)
        }
        if (requestedName == ArtifactNames.placeholderApkFileName(report)) {
            return PlaceholderArtifactContract.validate(report, entryDirectory, requestedName)
        }
        if (requestedName == ArtifactNames.anyioseApkFileName(report)) {
            return AnyiOSEmuArtifactContract.validate(report, entryDirectory, requestedName)
        }
        if (requestedName == ArtifactNames.gameApkFileName(report)) {
            return GameRuntimeArtifactContract.validate(report, entryDirectory, requestedName)
        }
        val conversion = report.optJSONObject("hostConversion") ?: error("no complete-game conversion attached")
        val expectedName = ArtifactNames.apkFileName(report)
        require(conversion.optString("status") == "ATTACHED" &&
            conversion.optBoolean("completeGameConversion", false) && conversion.optString("contract") == "complete-game-v1") {
            "only complete-game conversions may be opened"
        }
        require(conversion.optString("artifact") == expectedName && requestedName == expectedName) {
            "unsupported result name"
        }
        val result = File(entryDirectory, expectedName).canonicalFile
        require(result.path.startsWith(root.path + File.separator) && result.isFile)
        return result
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        require(mode == "r") { "read only" }
        return ParcelFileDescriptor.open(entry(uri), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String {
        entry(uri)
        return if (uri.lastPathSegment?.endsWith(".obb") == true) "application/octet-stream"
            else "application/vnd.android.package-archive"
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val file = entry(uri)
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        return MatrixCursor(columns).apply {
            addRow(columns.map { when (it) {
                OpenableColumns.DISPLAY_NAME -> (uri.lastPathSegment ?: file.name)
                OpenableColumns.SIZE -> file.length()
                else -> null
            } })
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException("read only")
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = throw UnsupportedOperationException("read only")
}
