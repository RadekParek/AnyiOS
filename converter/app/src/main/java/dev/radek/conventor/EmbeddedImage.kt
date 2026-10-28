package dev.radek.conventor

import java.io.File
import java.io.RandomAccessFile
import java.util.Locale

/** Distinguish Mach-O files from Java class resources, which share FAT_MAGIC. */
internal object EmbeddedImage {
    private val magics = setOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")

    fun isCandidate(file: File): Boolean {
        if (!file.isFile || file.length() < 4) return false
        val head = ByteArray(4)
        file.inputStream().use { if (it.read(head) != 4) return false }
        val magic = head.joinToString("") { "%02x".format(Locale.ROOT, it.toInt() and 255) }
        if (magic !in magics) return false
        // Do not discard arbitrary FAT files (even malformed ones) merely
        // because their first four bytes are CAFEBABE. Only a .class resource
        // with a complete, structurally valid Java class is excluded.
        return !(magic == "cafebabe" && file.extension.equals("class", ignoreCase = true) && isJavaClass(file))
    }

    private fun isJavaClass(file: File): Boolean = try {
        RandomAccessFile(file, "r").use { input ->
            val length = input.length()
            fun skip(count: Long) {
                require(count >= 0 && count <= length - input.filePointer)
                input.seek(input.filePointer + count)
            }
            fun u2() = input.readUnsignedShort()
            fun attributes(poolSize: Int) {
                repeat(u2()) {
                    require(u2() in 1 until poolSize)
                    skip(input.readInt().toLong() and 0xffffffffL)
                }
            }
            require(input.readInt() == 0xcafebabe.toInt())
            u2() // minor version
            require(u2() in 45..100) // class-file major version
            val poolSize = u2()
            require(poolSize > 1)
            val tags = IntArray(poolSize)
            var index = 1
            while (index < poolSize) {
                val tag = input.readUnsignedByte()
                tags[index] = tag
                when (tag) {
                    1 -> skip(u2().toLong()) // UTF-8
                    3, 4 -> skip(4)
                    5, 6 -> { skip(8); index++; require(index < poolSize) }
                    7, 8, 16, 19, 20 -> skip(2)
                    9, 10, 11, 12, 17, 18 -> skip(4)
                    15 -> skip(3)
                    else -> error("unknown Java constant-pool tag")
                }
                index++
            }
            u2() // access flags
            val thisClass = u2()
            val superClass = u2()
            require(thisClass in 1 until poolSize && tags[thisClass] == 7)
            require(superClass == 0 || (superClass in 1 until poolSize && tags[superClass] == 7))
            repeat(u2()) { require(u2().let { it in 1 until poolSize && tags[it] == 7 }) }
            fun members() {
                repeat(u2()) {
                    u2() // access flags
                    require(u2().let { it in 1 until poolSize && tags[it] == 1 })
                    require(u2().let { it in 1 until poolSize && tags[it] == 1 })
                    attributes(poolSize)
                }
            }
            members() // fields
            members() // methods
            attributes(poolSize)
            input.filePointer == length
        }
    } catch (_: Exception) {
        false
    }
}
