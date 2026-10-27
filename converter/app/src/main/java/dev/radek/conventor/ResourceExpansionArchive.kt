package dev.radek.conventor

import java.io.File
import java.util.zip.CRC32
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** ZIP of non-main bundle files laid out like the IPA's .app. Never used to execute code. */
internal object ResourceExpansionArchive {
    // The caller passes the publisher's private pending path and a validated,
    // sorted list of resource paths. Any failure is cleaned up by the publisher.
    fun write(target: File, resources: List<Pair<String, File>>) {
        ZipOutputStream(target.outputStream().buffered()).use { zip ->
            zip.setMethod(ZipOutputStream.STORED)
            zip.setLevel(Deflater.NO_COMPRESSION)
            val buffer = ByteArray(256 * 1024)
            for ((relative, payload) in resources) {
                require(SafeZip.validateName(relative) == relative) { "unsafe expansion resource path" }
                val crc = CRC32()
                payload.inputStream().use { input ->
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        if (count > 0) crc.update(buffer, 0, count)
                    }
                }
                val entry = ZipEntry(relative).apply {
                    method = ZipEntry.STORED
                    size = payload.length()
                    this.crc = crc.value
                    time = 0
                }
                zip.putNextEntry(entry)
                payload.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }
        require(target.isFile && target.length() > 0) { "could not assemble the expansion OBB" }
    }
}
