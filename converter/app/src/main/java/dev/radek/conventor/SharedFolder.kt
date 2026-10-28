package dev.radek.conventor

import java.io.File
import java.nio.ByteBuffer
import java.nio.file.Files
import java.security.MessageDigest

/** Inspect an already-owned extracted bundle without making a second source copy. */
internal object SharedFolder {
    data class Snapshot(val app: File, val sha256: String, val bytes: Long)

    fun inspect(root: File, progress: (Long) -> Unit): Snapshot {
        require(root.isDirectory && !Files.isSymbolicLink(root.toPath())) { "Imported folder is unavailable" }
        val canonical = root.canonicalFile
        val files = mutableListOf<File>()
        val names = mutableSetOf<String>()
        root.walkTopDown().onEnter { dir ->
            require(!Files.isSymbolicLink(dir.toPath()) && dir.canonicalFile.toPath().startsWith(canonical.toPath())) {
                "Imported folder contains an unsafe link"
            }
            true
        }.forEach { file ->
            if (file == root) return@forEach
            require(!Files.isSymbolicLink(file.toPath()) && file.canonicalFile.toPath().startsWith(canonical.toPath())) {
                "Imported folder contains an unsafe link"
            }
            require(file.isFile || file.isDirectory) { "Imported folder contains a special file" }
            val path = file.relativeTo(root).path.replace(File.separatorChar, '/')
            require(SafeZip.validateName(path) == path) { "Imported folder has an unsafe path" }
            require(names.add(path.lowercase(java.util.Locale.ROOT))) { "Imported folder contains case-colliding paths" }
            require(files.size < 65534) { "Imported folder has too many entries" }
            files += file
            if (files.size % 64 == 0) progress(0)
        }
        val apps = mutableListOf<File>()
        fun find(dir: File, depth: Int) {
            if (depth > 4 || apps.size > 1) return
            if (File(dir, "Info.plist").isFile) { apps += dir; return }
            dir.listFiles().orEmpty().filter { it.isDirectory && it.name !in setOf("Frameworks", "PlugIns") }
                .forEach { find(it, depth + 1) }
        }
        find(root, 0)
        require(apps.size == 1) { "Select one extracted .app folder with an Info.plist" }
        val digest = MessageDigest.getInstance("SHA-256")
        var total = 0L
        // Hash a framed, sorted directory inventory, not an imaginary IPA hash.
        for (file in files.sortedBy { it.relativeTo(root).path }) {
            val path = file.relativeTo(root).path.replace(File.separatorChar, '/').toByteArray(Charsets.UTF_8)
            digest.update(ByteBuffer.allocate(4).putInt(path.size).array())
            digest.update(path)
            digest.update(if (file.isDirectory) 0.toByte() else 1.toByte())
            if (file.isFile) {
                require(file.length() <= SafeZip.MAX_FILE) { "Imported folder contains an oversized file" }
                digest.update(ByteBuffer.allocate(8).putLong(file.length()).array())
                file.inputStream().use { input ->
                    val buffer = ByteArray(65536)
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count
                        digest.update(buffer, 0, count)
                        progress(total)
                    }
                }
            }
        }
        return Snapshot(apps.single(), digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }, total)
    }
}
