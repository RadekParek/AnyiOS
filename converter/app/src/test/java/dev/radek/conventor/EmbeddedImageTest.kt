package dev.radek.conventor

import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.io.File
import org.junit.Assert.*
import org.junit.Test

class EmbeddedImageTest {
    private fun javaClass(): ByteArray {
        val bytes = ByteArrayOutputStream()
        DataOutputStream(bytes).use { out ->
            out.writeInt(0xcafebabe.toInt()); out.writeShort(0); out.writeShort(52)
            out.writeShort(5)
            out.writeByte(1); out.writeUTF("Game")
            out.writeByte(7); out.writeShort(1)
            out.writeByte(1); out.writeUTF("java/lang/Object")
            out.writeByte(7); out.writeShort(3)
            out.writeShort(0x21); out.writeShort(2); out.writeShort(4)
            out.writeShort(0); out.writeShort(0); out.writeShort(0); out.writeShort(0)
        }
        return bytes.toByteArray()
    }

    @Test fun javaResourcesAreNotMistakenForFatMachO() {
        val dir = createTempDir(prefix = "embedded-images-")
        try {
            val validClass = File(dir, "Game.class").apply { writeBytes(javaClass()) }
            assertFalse(EmbeddedImage.isCandidate(validClass))
            // A magic collision without the resource's expected extension is
            // never skipped, even when the bytes look like a Java class.
            val noExtension = File(dir, "framework").apply { writeBytes(javaClass()) }
            assertTrue(EmbeddedImage.isCandidate(noExtension))
            val broken = File(dir, "Broken.class").apply { writeBytes(javaClass().dropLast(1).toByteArray()) }
            assertTrue(EmbeddedImage.isCandidate(broken))
            val malformedFat = File(dir, "malformed.class").apply {
                writeBytes(byteArrayOf(0xca.toByte(), 0xfe.toByte(), 0xba.toByte(), 0xbe.toByte(), 0, 0, 0, 1))
            }
            assertTrue(EmbeddedImage.isCandidate(malformedFat))
            for (hex in listOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")) {
                val image = File(dir, "macho-$hex").apply {
                    writeBytes(hex.chunked(2).map { it.toInt(16).toByte() }.toByteArray())
                }
                assertTrue("$hex must still be inspected", EmbeddedImage.isCandidate(image))
            }
            assertFalse(EmbeddedImage.isCandidate(File(dir, "notes.txt").apply { writeText("notes") }))
        } finally {
            dir.deleteRecursively()
        }
    }
}
