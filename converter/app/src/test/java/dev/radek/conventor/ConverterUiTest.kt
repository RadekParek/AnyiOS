package dev.radek.conventor

import android.app.Application
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import java.io.File
import java.util.UUID
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.android.controller.ActivityController

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [29], application = Application::class)
class ConverterUiTest {
    private val entries = mutableListOf<File>()
    private var controller: ActivityController<MainActivity>? = null

    @Before fun setUp() {
        SharedCatalog.end()
        SharedCatalog.install(null)
    }

    @After fun tearDown() {
        controller?.pause()?.stop()?.destroy()
        entries.forEach { it.deleteRecursively() }
        SharedCatalog.end()
        SharedCatalog.install(null)
    }

    private fun entry(title: String): File {
        val root = File(RuntimeEnvironment.getApplication<Application>().filesDir, "games").apply { mkdirs() }
        return File(root, "game-${UUID.randomUUID()}").apply {
            mkdirs()
            File(this, "source.bin").writeText("original source")
            File(this, "game.json").writeText(JSONObject().put("title", title).put("kind", "IPA")
                .put("architecture", "ARM32").put("bundleId", "test.game").toString())
            entries += this
        }
    }

    private fun start(): MainActivity {
        val next = Robolectric.buildActivity(MainActivity::class.java).setup()
        controller = next
        return next.get()
    }

    private fun all(view: View): List<View> = listOf(view) +
        if (view is ViewGroup) (0 until view.childCount).flatMap { all(view.getChildAt(it)) } else emptyList()

    private fun tile(activity: MainActivity, title: String): View = all(activity.window.decorView)
        .first { it.contentDescription?.toString()?.startsWith("Open $title details,") == true }

    private fun text(activity: MainActivity, label: String): TextView? = all(activity.window.decorView)
        .filterIsInstance<TextView>().firstOrNull { it.text.toString() == label }

    @Test fun tileOpensItsOwnUnanalyzedDetailsAndPickerIsAlwaysAvailable() {
        val alpha = entry("Alpha game")
        val beta = entry("Beta game")
        var activity = start()
        assertNotNull(activity.findViewById<View>(R.id.converter_game_grid))
        assertNotNull(activity.findViewById<Button>(R.id.module_picker_button))
        tile(activity, "Beta game").performClick()
        assertNotNull(text(activity, "Beta game"))
        assertNotNull(text(activity, "Analyze shared source"))
        assertNull(text(activity, "Alpha game"))
        assertFalse(File(beta, "report.json").exists())
        assertFalse(File(alpha, "report.json").exists())

        // Configuration recreation must keep an unanalysed shared entry open.
        val state = Bundle()
        controller!!.saveInstanceState(state).pause().stop().destroy()
        controller = Robolectric.buildActivity(MainActivity::class.java).create(state).start().resume().visible()
        activity = controller!!.get()
        assertNotNull(text(activity, "Beta game"))
        assertNotNull(text(activity, "Analyze shared source"))
        val button = activity.findViewById<Button>(R.id.module_picker_button)
        assertEquals("All modules", button.text.toString())
        button.performClick()
        val intent = shadowOf(activity).nextStartedActivity
        assertEquals("org.podnative.ModuleActivity", intent.component!!.className)
        assertTrue(intent.flags and Intent.FLAG_ACTIVITY_CLEAR_TOP != 0)
        assertTrue(intent.flags and Intent.FLAG_ACTIVITY_SINGLE_TOP != 0)
        assertEquals("original source", File(beta, "source.bin").readText())
        assertFalse(File(beta, "report.json").exists())
    }

    @Test fun savedReportRemainsAccessibleAndIsNotReanalyzedOnTap() {
        val game = entry("Renamed game")
        val report = JSONObject().put("state", "FAILED").put("error", "Existing report marker")
            .put("source", JSONObject().put("kind", "IPA"))
            .put("application", JSONObject().put("name", "Original title"))
            .toString()
        val reportFile = File(game, "report.json").apply { writeText(report) }
        val activity = start()
        tile(activity, "Renamed game").performClick()
        assertNotNull(text(activity, "Renamed game"))
        assertNotNull(text(activity, "Existing report marker"))
        assertNotNull(text(activity, "Retry analysis"))
        assertNotNull(activity.findViewById<Button>(R.id.module_picker_button))
        assertEquals(report, reportFile.readText())
    }

    @Test fun gridPagesAreBoundedAndCanRevealTheRemainingGames() {
        repeat(65) { entry("Game ${it.toString().padStart(2, '0')}") }
        val activity = start()
        val before = all(activity.window.decorView).count { it.contentDescription?.toString()?.startsWith("Open Game ") == true }
        assertEquals(60, before)
        text(activity, "Show more games (5 remaining)")!!.performClick()
        val after = all(activity.window.decorView).count { it.contentDescription?.toString()?.startsWith("Open Game ") == true }
        assertEquals(65, after)
        assertTrue(entries.none { File(it, "report.json").exists() })
    }

}
