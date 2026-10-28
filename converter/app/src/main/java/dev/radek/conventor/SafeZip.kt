package dev.radek.conventor

import java.io.File
import java.io.RandomAccessFile
import java.util.zip.ZipFile

/** Rejects symlinks, ZIP64, encrypted members, duplicate paths and ZIP bombs. */
object SafeZip {
    /** Largest non-ZIP64 member (0xffffffff is the ZIP64 sentinel). */
    internal const val MAX_FILE = 0xffff_fffeL
    internal const val MAX_EXPANSION_RATIO = 250
    /** Archive-wide budget for bytes above the ordinary 250:1 limit. */
    internal const val HIGH_COMPRESSION_ALLOWANCE = 256L * 1024 * 1024

    /**
     * Real assets can legitimately compress beyond 250:1 (for example sparse
     * textures). Allow a bounded amount above that ratio across the *whole*
     * archive rather than rejecting each tiny member. A large bomb or many
     * smaller members cannot multiply this allowance. All output is also
     * bounded by member size, declared size, CRC and available storage.
     */
    internal class ExpansionBudget(
        private val maxFile: Long = MAX_FILE,
        private val ratio: Long = MAX_EXPANSION_RATIO.toLong(),
        private val allowance: Long = HIGH_COMPRESSION_ALLOWANCE,
    ) {
        private var excess = 0L
        init { require(maxFile >= 0 && ratio > 0 && allowance >= 0) }

        fun admit(name: String, size: Long, compressed: Long) {
            require(size >= 0 && compressed >= 0) { "ZIP member '$name' has an unknown size" }
            require(size <= maxFile) { "ZIP member '$name' exceeds the ${formatMib(maxFile)} MiB member-size limit" }
            // APK readback may see ZIP64 metadata; do not let multiplication wrap.
            val basis = maxOf(1L, compressed)
            val ordinary = if (basis > Long.MAX_VALUE / ratio) Long.MAX_VALUE else basis * ratio
            val additional = (size - ordinary).coerceAtLeast(0L)
            require(additional <= allowance - excess) {
                "ZIP expansion safety limit at '$name': ${formatMib(size)} MiB expanded from " +
                    "${formatMib(compressed)} MiB compressed; the archive's " +
                    "${formatMib(allowance)} MiB high-compression allowance is exhausted"
            }
            excess += additional
        }
    }
    /** Largest entry count a non-ZIP64 archive can declare (see [centralDirectory]). */
    private const val MAX_ENTRIES = 65534
    private const val MIN_FREE_HEADROOM = 64L * 1024 * 1024
    /**
     * Name bounds. Shipped games nest deeply (a framework inside a framework
     * inside the `.app`, plus deep asset folders inside each), and the earlier
     * 32-component/1 KiB limits rejected real bundles with "unsafe ZIP path".
     * These bounds are the ones the destination filesystem actually has —
     * 255 bytes per component, 4096 bytes per path — with a generous depth cap.
     * Traversal, absolute paths, NUL bytes and absurd names still fail closed.
     */
    private const val MAX_NAME_COMPONENTS = 96
    private const val MAX_NAME_BYTES = 4096
    private const val MAX_COMPONENT_BYTES = 255

    /** Strict form: used for names that must already be canonical. */
    fun validateName(name: String): String {
        // A colon is a valid iOS/Android filename character, including in
        // CFBundleExecutable. Only a leading Windows drive path is unsafe.
        val drivePath = name.length >= 3 && name[0].isLetter() && name[1] == ':' && name[2] == '/'
        require(name.isNotEmpty() && !name.startsWith('/') && '\\' !in name && !drivePath && '\u0000' !in name) { "unsafe ZIP path" }
        val parts = name.trimEnd('/').split('/')
        require(parts.none { it == ".." || it == "." || it.isEmpty() }) { "unsafe ZIP path" }
        require(parts.size <= MAX_NAME_COMPONENTS) { "unsafe ZIP path: more than $MAX_NAME_COMPONENTS path components" }
        require(parts.all { it.toByteArray().size <= MAX_COMPONENT_BYTES }) { "unsafe ZIP path: path component exceeds $MAX_COMPONENT_BYTES bytes" }
        val joined = parts.joinToString("/")
        require(joined.toByteArray().size <= MAX_NAME_BYTES) { "unsafe ZIP path: path exceeds $MAX_NAME_BYTES bytes" }
        return joined
    }

    /**
     * Lenient form for real archives. Shipped IPAs contain names that are odd but
     * harmless (backslash separators, Windows drive prefixes, `//`, `./`), and
     * failing the whole import over one of them is worse than normalising it.
     *
     * Traversal, NUL bytes, absolute paths that survive normalisation, over-deep
     * and over-long names still throw, and every result is written under the
     * destination directory only.
     */
    fun memberName(name: String): String {
        require(name.isNotEmpty() && '\u0000' !in name) { "unsafe ZIP path" }
        var candidate = name.replace('\\', '/')
        while (candidate.startsWith("/")) candidate = candidate.substring(1)
        val parts = candidate.split('/').filter { it.isNotEmpty() && it != "." }
        require(parts.isNotEmpty() && parts.none { it == ".." }) { "unsafe ZIP path" }
        require(parts.size <= MAX_NAME_COMPONENTS) { "unsafe ZIP path: more than $MAX_NAME_COMPONENTS path components" }
        require(parts.all { it.toByteArray().size <= MAX_COMPONENT_BYTES }) {
            "unsafe ZIP path: path component exceeds $MAX_COMPONENT_BYTES bytes"
        }
        var joined = parts.joinToString("/")
        // Preserve legitimate colons in executables and resources, but keep
        // Windows drive prefixes relative and unambiguous on all hosts.
        if (joined.length >= 3 && joined[0].isLetter() && joined[1] == ':' && joined[2] == '/') {
            joined = joined[0] + "_" + joined.substring(2)
        }
        require(joined.toByteArray().size <= MAX_NAME_BYTES) { "unsafe ZIP path: path exceeds $MAX_NAME_BYTES bytes" }
        return joined
    }

    /** CFBundleExecutable is a single filename, not a relative or absolute path. */
    fun bundleExecutable(value: Any?): String {
        val name = value as? String ?: throw IllegalArgumentException("Info.plist CFBundleExecutable is missing or is not a string")
        require(name.isNotBlank() && '/' !in name && '\\' !in name) {
            "Info.plist CFBundleExecutable must be a single safe filename, not a path"
        }
        try {
            require(validateName(name) == name)
        } catch (error: IllegalArgumentException) {
            throw IllegalArgumentException("Info.plist CFBundleExecutable is not a safe filename: ${error.message}", error)
        }
        return name
    }

    /** True when the two names differ only by case (ZIP is case-insensitive on iOS). */
    fun collides(existing: Set<String>, name: String): Boolean =
        existing.contains(name.lowercase(java.util.Locale.ROOT))
    private fun centralDirectory(file: File) {
        RandomAccessFile(file, "r").use { f ->
            val length = f.length()
            require(length >= 22) { "invalid IPA size" }
            val tail = ByteArray(minOf(length, 65557).toInt())
            f.seek(length - tail.size); f.readFully(tail)
            fun u16(b: ByteArray, p: Int): Int = (b[p].toInt() and 255) or ((b[p+1].toInt() and 255) shl 8)
            fun u32(b: ByteArray, p: Int): Long = u16(b, p).toLong() or (u16(b, p+2).toLong() shl 16)
            val end = (tail.size - 22 downTo 0).firstOrNull { u32(tail, it) == 0x06054b50L && it + 22 + u16(tail, it + 20) == tail.size }
                ?: error("ZIP end record missing")
            require(u16(tail, end+4) == 0 && u16(tail, end+6) == 0) { "multi-disk ZIP unsupported" }
            val entries = u16(tail, end+10)
            // 65534 is the largest count the non-ZIP64 end record can hold; a
            // larger archive reports 0xFFFF here and is rejected as ZIP64.
            require(entries <= MAX_ENTRIES && entries == u16(tail, end+8)) {
                "ZIP entry limit / ZIP64 unsupported"
            }
            val size = u32(tail, end+12); val offset = u32(tail, end+16)
            require(size <= 16 * 1024 * 1024 && offset + size == length - tail.size + end) { "invalid central directory / ZIP64 unsupported" }
            f.seek(offset)
            val header = ByteArray(46)
            repeat(entries) {
                require(f.filePointer + 46 <= offset + size)
                f.readFully(header)
                require(u32(header, 0) == 0x02014b50L) { "invalid central directory signature" }
                require((u16(header, 8) and 1) == 0) { "encrypted ZIP prohibited" }
                require(u16(header, 10) in listOf(0, 8)) { "unsupported ZIP compression" }
                val kind = (u32(header, 38) shr 16).toInt() and 0xf000
                require(kind in listOf(0, 0x8000, 0x4000)) { "ZIP links / special files prohibited" }
                require(u32(header, 20) != 0xffffffffL && u32(header, 24) != 0xffffffffL && u32(header, 42) != 0xffffffffL) { "ZIP64 unsupported on device" }
                val remaining = u16(header, 28) + u16(header, 30) + u16(header, 32)
                require(f.filePointer + remaining <= offset + size)
                f.seek(f.filePointer + remaining)
            }
            require(f.filePointer == offset + size)
        }
    }
    /** Reserve additional free space, including a fixed safety margin. */
    fun requireStorage(destination: File, requiredBytes: Long) {
        var directory: File? = destination
        while (directory != null && !directory.isDirectory) directory = directory.parentFile
        val usable = try { directory?.usableSpace ?: Long.MAX_VALUE } catch (_: Exception) { Long.MAX_VALUE }
        require(requiredBytes >= 0 && requiredBytes <= Long.MAX_VALUE - MIN_FREE_HEADROOM &&
            usable >= requiredBytes + MIN_FREE_HEADROOM) {
            "not enough free storage: ${formatMib(requiredBytes)} MiB needed, ${formatMib(maxOf(0L, usable))} MiB free"
        }
    }

    private fun formatMib(bytes: Long): String = (bytes / (1024 * 1024)).toString()

    fun extract(
        source: File,
        destination: File,
        onBytes: (Long, Long, String) -> Unit = { _, _, _ -> },
        onFile: (Int, Int) -> Unit = { _, _ -> },
    ) {
        require(!destination.exists()) { "workspace already exists" }
        centralDirectory(source)
        requireStorage(destination, 0)
        require(destination.mkdirs())
        try {
            ZipFile(source).use { zip ->
                val entries = zip.entries().toList()
                require(entries.size <= MAX_ENTRIES) { "ZIP entry limit" }
                val names = mutableSetOf<String>()
                val budget = ExpansionBudget()
                val validated = entries.map { entry ->
                    val name = memberName(entry.name)
                    require(names.add(name.lowercase(java.util.Locale.ROOT))) { "duplicate/case-colliding ZIP path" }
                    budget.admit(name, entry.size, entry.compressedSize)
                    require(!entry.isDirectory || entry.size == 0L) { "ZIP directory '$name' contains data" }
                    entry to name
                }
                val uncompressedTotal = entries.sumOf { it.size }
                // source already occupies disk space; usableSpace is the space
                // *remaining*, so charging for it again rejects large valid IPAs.
                requireStorage(destination, uncompressedTotal)
                var extractedBytes = 0L
                validated.forEachIndexed { index, (entry, name) ->
                    val target = File(destination, name)
                    require(target.canonicalPath.startsWith(destination.canonicalPath + File.separator))
                    if (entry.isDirectory) {
                        require(target.isDirectory || target.mkdirs())
                    } else {
                        require(target.parentFile!!.isDirectory || target.parentFile!!.mkdirs())
                        require(target.createNewFile()) { "ZIP path collision" }
                        zip.getInputStream(entry).use { input ->
                            target.outputStream().use { output ->
                                val buffer = ByteArray(65536); var written = 0L
                                val crc = java.util.zip.CRC32()
                                while (true) {
                                    val count = input.read(buffer); if (count < 0) break
                                    written += count; require(written <= entry.size && written <= MAX_FILE)
                                    crc.update(buffer, 0, count); output.write(buffer, 0, count)
                                    extractedBytes += count
                                    onBytes(extractedBytes, uncompressedTotal, name)
                                }
                                require(written == entry.size && crc.value == entry.crc) { "ZIP size/CRC mismatch" }
                            }
                        }
                    }
                    onFile(index + 1, entries.size)
                }
            }
        } catch (error: Throwable) {
            destination.deleteRecursively()
            throw error
        }
    }
}
