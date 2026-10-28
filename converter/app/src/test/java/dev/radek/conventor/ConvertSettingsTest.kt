package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [29], manifest = Config.NONE)
class ConvertSettingsTest {
    @Test fun defaultsAreOffAndChoicesArePerGame() {
        val root = Files.createTempDirectory("convert-settings").toFile()
        try {
            val one = root.resolve("one").apply { mkdir() }
            val two = root.resolve("two").apply { mkdir() }
            assertEquals(ConvertSettings(), ConvertSettings.load(one))
            val changed = ConvertSettings(classic320 = true, offline = true, performance = true, orientation = "-90")
            ConvertSettings.save(one, changed)
            assertEquals(changed, ConvertSettings.load(one))
            assertEquals(ConvertSettings(), ConvertSettings.load(two))
            ConvertSettings.save(one, ConvertSettings())
            assertEquals(ConvertSettings(), ConvertSettings.load(one))
        } finally { root.deleteRecursively() }
    }
    @Test fun migratesOldSettingsWithoutLosingOptIns() {
        val old = JSONObject().put("classic43", true).put("offline", true)
            .put("performance", true).put("orientation", "landscape")
        assertEquals(ConvertSettings(true, true, true, "-90"), ConvertSettings.fromJson(old))
        assertEquals("portrait", ConvertSettings.fromJson(JSONObject().put("orientation", "portrait")).orientation)
        for (value in listOf("default", "-90", "90", "180")) {
            assertEquals(value, ConvertSettings.fromJson(JSONObject().put("orientation", value)).orientation)
        }
        assertEquals(false, ConvertSettings.fromJson(JSONObject().put("classic43", true).put("classic320", false)).classic320)
    }
    @Test fun tabletProfileRoundTripsAndProfilesAreExclusive() {
        val root = Files.createTempDirectory("convert-tablet").toFile()
        try {
            val tablet = ConvertSettings(classic1024 = true)
            ConvertSettings.save(root, tablet)
            assertEquals(tablet, ConvertSettings.load(root))
            assertEquals(false, ConvertSettings.load(root).classic320)
            assertThrows(IllegalArgumentException::class.java) { ConvertSettings(classic320 = true, classic1024 = true) }
            assertThrows(IllegalArgumentException::class.java) {
                ConvertSettings.fromJson(JSONObject().put("classic320", true).put("classic1024", true))
            }
            assertThrows(IllegalArgumentException::class.java) {
                ConvertSettings.fromJson(JSONObject().put("classic1024", "true"))
            }
        } finally { root.deleteRecursively() }
    }
    @Test fun invalidSettingsFailClosedRatherThanSilentlyEnablingNetwork() {
        assertThrows(IllegalArgumentException::class.java) { ConvertSettings.fromJson(JSONObject().put("offline", "true")) }
        assertThrows(IllegalArgumentException::class.java) { ConvertSettings.fromJson(JSONObject().put("orientation", "invalid")) }
    }
}
