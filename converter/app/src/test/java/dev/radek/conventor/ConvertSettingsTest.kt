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
            val changed = ConvertSettings(classic43 = true, offline = true, performance = true, orientation = "landscape")
            ConvertSettings.save(one, changed)
            assertEquals(changed, ConvertSettings.load(one))
            assertEquals(ConvertSettings(), ConvertSettings.load(two))
            ConvertSettings.save(one, ConvertSettings())
            assertEquals(ConvertSettings(), ConvertSettings.load(one))
        } finally { root.deleteRecursively() }
    }
    @Test fun invalidSettingsFailClosedRatherThanSilentlyEnablingNetwork() {
        assertThrows(IllegalArgumentException::class.java) { ConvertSettings.fromJson(JSONObject().put("offline", "true")) }
        assertThrows(IllegalArgumentException::class.java) { ConvertSettings.fromJson(JSONObject().put("orientation", "invalid")) }
    }
}
