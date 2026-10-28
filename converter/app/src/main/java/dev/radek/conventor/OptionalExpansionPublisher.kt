package dev.radek.conventor

import java.io.File
import java.security.MessageDigest

/**
 * Publishes a nonessential expansion only after its self-contained APK is ready.
 * A storage or report failure must not invalidate the APK or replace a previous
 * sidecar with a partial file. The caller records the returned bytes/hash only
 * inside [record], after which the backup can be discarded.
 */
internal object OptionalExpansionPublisher {
    data class Artifact(val name: String, val bytes: Long, val sha256: String)

    /** Null means success; otherwise a short diagnostic suitable for a report. */
    fun publish(directory: File, name: String, write: (File) -> Unit, record: (Artifact) -> Unit): String? =
        publish(directory, name, "", write, record)

    fun publish(
        directory: File,
        name: String,
        storage: String,
        write: (File) -> Unit,
        record: (Artifact) -> Unit,
    ): String? {
        var pending: File? = null
        var target: File? = null
        var backup: File? = null
        var installed = false
        var preserved = false
        var ownsPending = false
        try {
            require(name.matches(Regex("main\\.[1-9][0-9]*\\.[A-Za-z0-9_.]+\\.obb"))) {
                "invalid expansion name"
            }
            val entry = directory.canonicalFile
            require(entry.isDirectory) { "expansion directory is unavailable" }
            require(storage in setOf("", "native", "bounded")) { "invalid expansion storage" }
            val root = if (storage.isEmpty()) entry else File(entry, "expansion-$storage").also {
                require(!java.nio.file.Files.isSymbolicLink(it.toPath()) && it.canonicalFile.parentFile == entry) {
                    "unsafe expansion storage"
                }
                require(it.isDirectory || it.mkdir()) { "cannot create expansion storage" }
            }.canonicalFile
            target = File(root, name)
            pending = File(root, ".$name.optional-pending")
            backup = File(root, ".$name.optional-backup")
            for (file in listOf(target, pending, backup)) {
                require(!java.nio.file.Files.isSymbolicLink(file.toPath()) && file.canonicalFile.parentFile == root) {
                    "unsafe expansion path"
                }
            }
            require(!target.exists() || target.isFile) { "invalid expansion target" }
            // Recover an interruption before starting a new optional attempt.
            if (backup.exists()) {
                require(backup.isFile) { "invalid expansion backup" }
                if (target.exists()) require(backup.delete()) { "cannot remove old expansion backup" }
                else require(backup.renameTo(target)) { "cannot restore previous expansion" }
            }
            if (pending.exists()) require(pending.isFile && pending.delete()) { "cannot remove incomplete expansion" }
            ownsPending = true
            write(pending)
            require(pending.isFile && pending.length() > 0) { "expansion was not produced" }
            val digest = sha256(pending)
            val bytes = pending.length()
            if (target.exists()) {
                require(target.isFile && target.renameTo(backup)) { "cannot preserve previous expansion" }
                preserved = true
            }
            require(pending.renameTo(target)) { "cannot install expansion" }
            installed = true
            record(Artifact(name, bytes, digest))
            // If a backup cannot be removed, it will be cleaned on the next build.
            backup.delete()
            return null
        } catch (error: Throwable) {
            if (error !is Exception && error !is OutOfMemoryError) throw error
            if (installed) target?.delete()
            if (preserved && backup?.isFile == true && target?.exists() == false) backup.renameTo(target)
            return if (error is OutOfMemoryError) "not enough memory for the optional expansion"
                else (error.message ?: error.javaClass.simpleName).take(240)
        } finally {
            if (ownsPending) pending?.delete()
        }
    }

    private fun sha256(file: File): String = file.inputStream().buffered().use { input ->
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
