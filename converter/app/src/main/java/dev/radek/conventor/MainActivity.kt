package dev.radek.conventor

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.content.ContentValues
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.View
import android.view.WindowInsets
import android.text.TextUtils
import android.widget.*
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.ZipFile

internal object Jobs {
    private val executor = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    @Volatile var busy = false
    @Volatile var percent = 0
    @Volatile var title = "Working"
    @Volatile var message = ""
    var listener: (() -> Unit)? = null
    fun begin(jobTitle: String) { title = jobTitle; percent = 0; message = "Preparing" }
    fun update(value: Int, text: String) {
        percent = value.coerceIn(0, 100)
        message = text
        main.post { runCatching { listener?.invoke() } }
    }
    fun update(text: String) = update(percent, text)

    /** Runs [block] unless a job is already running; never throws on the caller's thread. */
    @Synchronized fun tryRun(block: () -> Unit): Boolean {
        if (busy || !SharedCatalog.tryBegin()) return false
        busy = true
        executor.execute {
            try {
                block()
            } catch (e: Throwable) {
                update(percent, "Failed at $percent%: ${e.message ?: e.javaClass.simpleName}")
            } finally {
                busy = false
                SharedCatalog.end()
                SharedCatalog.changed()
                main.post { runCatching { listener?.invoke() } }
            }
        }
        return true
    }
}

/** Bounded, off-main-thread artwork loading for large libraries. */
private object IconLoader {
    private val executor = Executors.newFixedThreadPool(2)
    private val main = Handler(Looper.getMainLooper())
    private val cache = object : android.util.LruCache<String, android.graphics.Bitmap>(16 * 1024 * 1024) {
        override fun sizeOf(key: String, bitmap: android.graphics.Bitmap) = bitmap.byteCount
    }

    fun load(file: File, targetPixels: Int, image: ImageView) {
        if (!file.isFile) return
        val target = ArtworkSizing.target(targetPixels)
        val key = "${file.path}:${file.lastModified()}:$target"
        image.tag = key
        cache.get(key)?.let { image.setImageBitmap(it); return }
        // A queued decode must not keep an old Activity or detached grid alive.
        val imageRef = java.lang.ref.WeakReference(image)
        executor.execute {
            val current = imageRef.get()
            if (current == null || !current.isAttachedToWindow || current.tag != key) return@execute
            val bitmap = try {
                val bounds = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
                android.graphics.BitmapFactory.decodeFile(file.path, bounds)
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0) null else {
                    val sample = ArtworkSizing.sample(bounds.outWidth, bounds.outHeight, target)
                    val decoded = android.graphics.BitmapFactory.decodeFile(
                        file.path, android.graphics.BitmapFactory.Options().apply { inSampleSize = sample },
                    )
                    if (decoded == null) null else {
                        val (width, height) = ArtworkSizing.scaled(decoded.width, decoded.height, target)
                        if (width == decoded.width && height == decoded.height) decoded else {
                            android.graphics.Bitmap.createScaledBitmap(decoded, width, height, true).also {
                                if (it !== decoded) decoded.recycle()
                            }
                        }
                    }
                }
            } catch (_: Throwable) {
                null
            }
            if (bitmap != null) runCatching { cache.put(key, bitmap) }
            main.post {
                val view = imageRef.get()
                if (bitmap != null && view != null && view.isAttachedToWindow && view.tag == key)
                    view.setImageBitmap(bitmap)
            }
        }
    }
}

class MainActivity : Activity() {
    private var bgColor = Color.rgb(11, 16, 29)
    private var panel = Color.rgb(20, 29, 47)
    private var muted = Color.rgb(160, 178, 199)
    private var accent = Color.rgb(92, 227, 181)
    private var textColor = Color.WHITE
    private var outlineColor = Color.rgb(45, 59, 79)
    private var lightMode = false
    private val preferences by lazy { getSharedPreferences("radek_settings", MODE_PRIVATE) }
    private lateinit var library: Library
    private lateinit var body: LinearLayout
    private lateinit var screenRoot: FrameLayout
    private var currentScroll: ScrollView? = null
    private var screenInitialized = false
    private var hasResumed = false
    private var renderedSharedBusy = false
    private var selected: File? = null
    private var progressLabel: TextView? = null
    private var progressBar: ProgressBar? = null
    private var wasBusy = false
    private var returnToDetailAfterJob: File? = null
    private var openImportedResultAfterJob = false
    private var pendingInstall: Pair<File, String>? = null
    private val pickerIpa = 100
    private val pickerApk = 101
    private val pickerTranslatedRuntimeInput = 102
    private val maxTranslatedRuntimeInputBytes = 1024L * 1024 * 1024
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()

    /** Accent presets for the converter UI; each has a dark-theme and a light-theme tone. */
    private data class AccentScheme(val id: String, val label: String, val dark: Int, val light: Int)
    private val accentSchemes = listOf(
        AccentScheme("teal", "Teal (default)", Color.rgb(92, 227, 181), Color.rgb(0, 125, 112)),
        AccentScheme("blue", "Blue", Color.rgb(125, 180, 255), Color.rgb(29, 78, 216)),
        AccentScheme("amber", "Amber", Color.rgb(251, 191, 36), Color.rgb(180, 83, 9)),
        AccentScheme("violet", "Violet", Color.rgb(196, 167, 255), Color.rgb(109, 40, 217)),
    )

    private fun applyThemePalette() {
        lightMode = preferences.getBoolean("light_theme", false)
        val scheme = accentSchemes.firstOrNull { it.id == preferences.getString("accent_scheme", "teal") } ?: accentSchemes[0]
        if (lightMode) {
            bgColor = Color.rgb(245, 247, 250)
            panel = Color.WHITE
            muted = Color.rgb(92, 104, 119)
            accent = scheme.light
            textColor = Color.rgb(22, 32, 46)
            outlineColor = Color.rgb(220, 226, 234)
        } else {
            bgColor = Color.rgb(11, 16, 29)
            panel = Color.rgb(20, 29, 47)
            muted = Color.rgb(160, 178, 199)
            accent = scheme.dark
            textColor = Color.WHITE
            outlineColor = Color.rgb(45, 59, 79)
        }
        window.statusBarColor = bgColor
        window.navigationBarColor = bgColor
        window.decorView.systemUiVisibility = if (lightMode) {
            View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
        } else 0
    }

    private val sharedChanged: () -> Unit = {
        // Emulator reports copy progress frequently. Rebuild the grid only when
        // its job starts or finishes, not for each progress notification.
        if (!isFinishing && !isDestroyed && !Jobs.busy && currentScreen == Screen.LIBRARY &&
            (!SharedCatalog.isBusy() || !renderedSharedBusy)) home()
    }
    private val sharedObserver = Runnable { sharedChanged() }
    private val jobsListener: () -> Unit = {
        runCatching {
            progressLabel?.text = "${Jobs.percent}% · ${Jobs.message}"
            progressBar?.progress = Jobs.percent
            if (wasBusy && !Jobs.busy) {
                wasBusy = false
                val target = returnToDetailAfterJob
                returnToDetailAfterJob = null
                val openImport = openImportedResultAfterJob
                openImportedResultAfterJob = false
                if (target != null && target.isDirectory && File(target, "report.json").isFile &&
                    ((currentScreen == Screen.DETAIL && selected == target) ||
                        (openImport && currentScreen == Screen.LIBRARY))) {
                    detail(target, if (selected == target) currentScroll?.scrollY ?: 0 else 0)
                } else if (currentScreen == Screen.LIBRARY) home()
            }
        }
    }
    private fun sharedIcon(dir: File): File = File(dir, "cover.png").takeIf { it.isFile } ?: File(dir, "icon.png")

    override fun onCreate(state: Bundle?) {
        setTheme(if (preferences.getBoolean("light_theme", false)) android.R.style.Theme_Material_Light_NoActionBar else android.R.style.Theme_Material_NoActionBar)
        super.onCreate(state)
        applyThemePalette()
        library = Library(applicationContext)
        if (!SharedCatalog.isBusy()) library.recoverInterrupted()
        selected = state?.getString("selected")?.let { File(library.root, it) }
        visibleEntries = state?.getInt("visibleEntries", 60)?.coerceIn(60, 10000) ?: 60
        val savedScroll = state?.getInt("scrollY", 0) ?: 0
        if (Jobs.busy) returnToDetailAfterJob = selected
        Jobs.listener = jobsListener
        if (selected?.isDirectory == true && (File(selected, "report.json").isFile || File(selected, "game.json").isFile)) {
            detail(selected!!, savedScroll)
        } else {
            home()
            currentScroll?.let { scroll ->
                if (savedScroll > 0) scroll.post { if (currentScroll === scroll) scroll.scrollTo(0, savedScroll) }
            }
        }
    }
    override fun onSaveInstanceState(out: Bundle) {
        out.putString("selected", selected?.name)
        out.putInt("visibleEntries", visibleEntries)
        out.putInt("scrollY", currentScroll?.scrollY ?: 0)
        super.onSaveInstanceState(out)
    }
    override fun onDestroy() {
        // A newly opened Converter can register before an older instance is destroyed.
        if (Jobs.listener === jobsListener) Jobs.listener = null
        super.onDestroy()
    }
    override fun onPause() { SharedCatalog.unobserve(sharedObserver); super.onPause() }
    override fun onResume() {
        super.onResume()
        SharedCatalog.observe(sharedObserver)
        applyThemePalette()
        if (hasResumed && !Jobs.busy && !wasBusy && currentScreen == Screen.LIBRARY) home()
        hasResumed = true
        if (Jobs.busy) {
            wasBusy = true
            progressLabel?.text = "${Jobs.percent}% · ${Jobs.message}"
            progressBar?.progress = Jobs.percent
        } else if (wasBusy) {
            // A job ended while we were away. Clear any build progress it left
            // marked RUNNING so the entry is never stuck in a state the user has
            // to wipe app data to escape.
            wasBusy = false
            runCatching { library.recoverInterrupted() }
            if (currentScreen == Screen.DETAIL && selected != null && File(selected, "report.json").isFile)
                detail(selected!!, currentScroll?.scrollY ?: 0)
            else if (currentScreen == Screen.LIBRARY) home()
        }
        val pending = pendingInstall
        if (pending != null && packageManager.canRequestPackageInstalls()) {
            pendingInstall = null
            installArtifact(pending.first, pending.second)
        }
    }

    private fun rounded(color: Int): GradientDrawable = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(18).toFloat()
        setStroke(dp(1), outlineColor)
    }
    private fun screen(next: Screen) {
        val animatePage = !screenInitialized || currentScreen != next
        screenInitialized = true
        currentScreen = next
        progressLabel = null
        progressBar = null
        val scroll = ScrollView(this).apply { isFillViewport = true; clipToPadding = false }
        currentScroll = scroll
        screenRoot = FrameLayout(this).apply {
            background = SharedTheme.canvas(this@MainActivity)
            setOnApplyWindowInsetsListener { view, insets ->
                if (Build.VERSION.SDK_INT >= 30) {
                    val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                    view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                } else {
                    @Suppress("DEPRECATION")
                    view.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                        insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
                }
                insets
            }
        }
        val shell = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val toolbar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(22), dp(12), dp(22), dp(10))
        }
        val heading = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        toolbar.addView(heading, LinearLayout.LayoutParams(0, -2, 1f))
        TextView(this).apply {
            text = "AnyiOS"; textSize = 19f; setTextColor(textColor)
            typeface = SharedTheme.typeface(this@MainActivity, true)
            heading.addView(this)
        }
        TextView(this).apply {
            text = "CONVERTER"; textSize = 10f; setTextColor(accent); letterSpacing = 0.08f
            typeface = SharedTheme.typeface(this@MainActivity, true)
            heading.addView(this)
        }
        toolbar.addView(SharedTheme.moduleButton(this))
        shell.addView(toolbar)
        body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(22), 0, dp(22), dp(36)) }
        scroll.addView(body)
        shell.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        screenRoot.addView(shell, FrameLayout.LayoutParams(-1, -1))
        setContentView(screenRoot)
        screenRoot.requestApplyInsets()
        if (animatePage) SharedTheme.reveal(body)
    }

    /** Persistent progress on the detail screen, without scrolling away from the action. */
    private fun addDetailJobOverlay() {
        body.setPadding(dp(22), 0, dp(22), dp(160))
        val overlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = SharedTheme.glass(this@MainActivity, 18)
            elevation = dp(8).toFloat()
            setPadding(dp(16), dp(12), dp(16), dp(12))
            accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        }
        text(Jobs.title, 14f, textColor, true, overlay)
        progressBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100; progress = Jobs.percent
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
            overlay.addView(this, LinearLayout.LayoutParams(-1, dp(8)))
        }
        progressLabel = text("${Jobs.percent}% · ${Jobs.message}", 12f, muted, parent = overlay)
        screenRoot.addView(overlay, FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM).apply {
            setMargins(dp(16), 0, dp(16), dp(16))
        })
    }
    private fun text(value: String, size: Float = 16f, color: Int = textColor, bold: Boolean = false, parent: LinearLayout = body): TextView = TextView(this).apply {
        text = value; textSize = size; setTextColor(color); typeface = SharedTheme.typeface(this@MainActivity, bold)
        includeFontPadding = false
        setPadding(0, dp(6), 0, dp(7)); parent.addView(this)
    }
    private fun button(label: String, primary: Boolean = false, parent: LinearLayout = body, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(if (primary) bgColor else accent)
        typeface = SharedTheme.typeface(this@MainActivity, true)
        minHeight = dp(54); minimumWidth = 0; minWidth = 0
        setPadding(dp(16), dp(10), dp(16), dp(10))
        background = if (primary) android.graphics.drawable.RippleDrawable(
            android.content.res.ColorStateList.valueOf(0x40ffffff), rounded(accent), null,
        ) else SharedTheme.glassRipple(this@MainActivity, 18)
        elevation = 0f
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10); bottomMargin = dp(3) })
        SharedTheme.pressFeedback(this)
        setOnClickListener { action() }
    }
    private fun dangerButton(label: String, action: () -> Unit): Button = Button(this).apply {
        text = label; isAllCaps = false; textSize = 15f; setTextColor(Color.WHITE)
        typeface = SharedTheme.typeface(this@MainActivity, true)
        minHeight = dp(54); minimumWidth = 0; minWidth = 0
        setPadding(dp(16), dp(10), dp(16), dp(10))
        background = android.graphics.drawable.RippleDrawable(
            android.content.res.ColorStateList.valueOf(Color.WHITE), rounded(Color.rgb(189, 52, 58)), null,
        )
        elevation = dp(2).toFloat()
        body.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(12); bottomMargin = dp(4) })
        SharedTheme.pressFeedback(this)
        setOnClickListener { action() }
    }
    private fun progressIndicator(parent: LinearLayout, value: Int, height: Int = 8) {
        val indicator = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progress = value.coerceIn(0, 100)
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(outlineColor)
            contentDescription = "IPA analysis or validated host conversion progress ${progress}%"
        }
        parent.addView(indicator, LinearLayout.LayoutParams(-1, dp(height)).apply { topMargin = dp(4); bottomMargin = dp(4) })
    }
    private fun addJobProgressCard(parent: LinearLayout = body) {
        val jobCard = card(parent)
        text(Jobs.title, 17f, textColor, true, jobCard)
        val bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progress = Jobs.percent
            progressTintList = android.content.res.ColorStateList.valueOf(accent)
        }
        progressBar = bar
        jobCard.addView(bar, LinearLayout.LayoutParams(-1, dp(12)).apply { topMargin = dp(8); bottomMargin = dp(4) })
        progressLabel = text("${Jobs.percent}% · ${Jobs.message}", 13f, muted, parent = jobCard)
    }
    private fun card(parent: LinearLayout = body): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; background = SharedTheme.glass(this@MainActivity, 22); setPadding(dp(18), dp(14), dp(18), dp(16))
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })
    }
    // Rendering hundreds of library cards at once stalls the UI thread, so the
    // home list is paginated; "Show more" reveals the next page.
    private var visibleEntries = 60
    private fun statusColor(state: String) = when (state) {
        "READY", "ATTACHED", "GENERATED_ON_DEVICE" -> accent
        "FAILED", "BLOCKED" -> if (lightMode) Color.rgb(179, 38, 30) else Color.rgb(255, 157, 139)
        else -> if (lightMode) Color.rgb(145, 83, 0) else Color.rgb(245, 203, 116)
    }
    private fun formatBytes(n: Long) = "%.1f MiB".format(n / 1048576.0)

    /**
     * Which top-level screen is showing, so the system back gesture navigates
     * inside the app instead of closing it: detail/settings go back to the Game
     * Library, and only the library itself closes the app.
     */
    private enum class Screen { LIBRARY, DETAIL, SETTINGS }
    private var currentScreen = Screen.LIBRARY

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (currentScreen == Screen.LIBRARY) {
            SharedTheme.openModules(this)
            return
        }
        home()
    }

    private fun home() {
        val previousScroll = if (currentScreen == Screen.LIBRARY) currentScroll?.scrollY ?: 0 else 0
        selected = null
        screen(Screen.LIBRARY)
        renderedSharedBusy = SharedCatalog.isBusy()
        currentScroll?.let { scroll ->
            if (previousScroll > 0) scroll.post { if (currentScroll === scroll) scroll.scrollTo(0, previousScroll) }
        }
        text("YOUR COLLECTION  /  CONVERTER", 10f, accent, true).letterSpacing = 0.08f
        text("Game Library", 30f, textColor, true)
        text("Choose a game to see its analysis and build options.", 14f, muted)

        val actions = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        body.addView(actions)
        val add = button("Add IPA", true, actions) { authorize() }.apply { isEnabled = !SharedCatalog.isBusy() }
        add.layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { rightMargin = dp(5); topMargin = dp(10) }
        val settings = button("Settings", parent = actions) { settingsScreen() }.apply { isEnabled = !SharedCatalog.isBusy() }
        settings.layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply { leftMargin = dp(5); topMargin = dp(10) }
        button("Import folder of IPAs") {
            startActivity(Intent().setClassName(this, "org.podnative.MainActivity").putExtra("importIpaFolder", true))
        }.isEnabled = !SharedCatalog.isBusy()

        if (SharedCatalog.isBusy() && !Jobs.busy) text("Emulator is updating the shared library. New entries will appear when it finishes.", 13f, muted)
        if (Jobs.busy) {
            wasBusy = true
            addJobProgressCard()
        } else if (Jobs.message.startsWith("Failed")) text(Jobs.message, 14f, statusColor("FAILED"))

        val entries = library.entries()
        text("${entries.size} ${if (entries.size == 1) "game" else "games"} · stored on this device", 12f, muted)
        if (entries.isEmpty()) {
            val empty = card()
            text("Your games, in one place", 19f, textColor, true, empty)
            text("Add an IPA you own, or import games in Emulator. Their shared sources appear here without automatic Converter analysis.", 14f, muted, parent = empty)
            text("Encrypted and FairPlay-protected binaries are never decrypted.", 12f, muted, parent = empty)
        } else {
            val width = resources.configuration.screenWidthDp
            val columns = SharedTheme.gridColumns(width, resources.configuration.fontScale)
            val grid = LinearLayout(this).apply {
                id = dev.radek.conventor.R.id.converter_game_grid
                orientation = LinearLayout.VERTICAL
            }
            body.addView(grid, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(12) })
            val visible = entries.take(visibleEntries)
            visible.chunked(columns).forEach { group ->
                val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.TOP }
                grid.addView(row)
                for (column in 0 until columns) {
                    val cell = if (column < group.size) gameTile(group[column].first, group[column].second, columns) else View(this)
                    row.addView(cell, LinearLayout.LayoutParams(0, -2, 1f).apply {
                        setMargins(if (column == 0) 0 else dp(4), 0, if (column == columns - 1) 0 else dp(4), dp(10))
                    })
                }
            }
        }
        if (entries.size > visibleEntries) {
            button("Show more games (${entries.size - visibleEntries} remaining)") {
                visibleEntries += 60
                home()
            }
        }
        val info = card()
        text("Analysis is manual", 15f, textColor, true, info)
        text("Emulator imports share their original private source. Open a game and choose Analyze when you want a Converter report. A narrowly proven single-routine IPA may still be built during explicit analysis.", 12f, muted, parent = info)
        text("Builds are experimental and do not guarantee gameplay.", 12f, muted, parent = info)
    }

    private fun gameTile(dir: File, report: JSONObject, columns: Int): View {
        val name = library.displayTitle(dir, report)
        val metadata = library.metadata(dir)
        val state = report.optString("state", "FAILED")
        val stateLabel = when (state) {
            "SHARED_SOURCE" -> "Not analyzed"
            "READY" -> "Ready"
            "PARTIAL" -> "Partial"
            "BLOCKED" -> "Blocked"
            "FAILED" -> "Analysis failed"
            else -> state.lowercase(Locale.ROOT).replace('_', ' ').replaceFirstChar { it.titlecase(Locale.ROOT) }
        }
        val item = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(9), dp(9), dp(9), dp(11))
            background = SharedTheme.glassRipple(this@MainActivity, 18)
            isFocusable = true
            isClickable = true
            contentDescription = "Open $name details, $stateLabel"
            setOnClickListener { detail(dir) }
            SharedTheme.pressFeedback(this)
        }
        val artwork = FrameLayout(this).apply {
            background = SharedTheme.artwork(this@MainActivity, 16)
            clipToOutline = true
        }
        val placeholder = ImageView(this).apply {
            setImageResource(dev.radek.conventor.R.drawable.ic_game_outline)
            imageTintList = android.content.res.ColorStateList.valueOf(accent)
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        artwork.addView(placeholder, FrameLayout.LayoutParams(dp(32), dp(32), Gravity.CENTER))
        val artworkHeight = SharedTheme.artworkSizeDp(resources.configuration.screenWidthDp, columns)
        val iconPath = sharedIcon(dir)
        if (iconPath.isFile) {
            val icon = ImageView(this).apply {
                scaleType = ImageView.ScaleType.CENTER_CROP
                importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            }
            artwork.addView(icon, FrameLayout.LayoutParams(-1, -1))
            icon.post { IconLoader.load(iconPath, dp(artworkHeight), icon) }
        }
        item.addView(artwork, LinearLayout.LayoutParams(dp(artworkHeight), dp(artworkHeight)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
        })
        fun tileLabel(value: String, size: Float, color: Int, bold: Boolean = false): TextView = TextView(this).apply {
            text = value; textSize = size; setTextColor(color)
            typeface = SharedTheme.typeface(this@MainActivity, bold)
            includeFontPadding = false
        }
        item.addView(tileLabel(name, 13f, textColor, true).apply {
            maxLines = 2; ellipsize = TextUtils.TruncateAt.END; minHeight = dp(32)
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        val kind = metadata?.optString("kind")?.takeIf { it.isNotBlank() } ?: report.optJSONObject("source")?.optString("kind").orEmpty()
        val architecture = metadata?.optString("architecture")?.takeIf { it.isNotBlank() } ?: architectures(report)
        val subtitle = listOf(architecture, kind).filter { it.isNotBlank() }.joinToString(" · ")
        item.addView(tileLabel(subtitle, 10f, muted).apply { maxLines = 1; ellipsize = TextUtils.TruncateAt.END },
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(5) })
        item.addView(tileLabel(stateLabel, 10f, if (state == "SHARED_SOURCE") muted else statusColor(state), true).apply {
            maxLines = 1; ellipsize = TextUtils.TruncateAt.END
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })
        return item
    }
    private fun settingsScreen() {
        if (Jobs.busy) return
        screen(Screen.SETTINGS)
        button("Game Library") { home() }
        text("Settings", 28f, textColor, true)
        text("Appearance and host analysis target architecture.", 14f, muted)

        val appearance = card()
        text("Appearance", 17f, textColor, true, appearance)
        val lightSwitch = android.widget.Switch(this).apply {
            text = "Use light theme"
            textSize = 15f
            setTextColor(textColor)
            isChecked = preferences.getBoolean("light_theme", false)
        }
        appearance.addView(lightSwitch, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
        lightSwitch.setOnCheckedChangeListener { _, enabled ->
            preferences.edit().putBoolean("light_theme", enabled).apply()
            applyThemePalette()
            settingsScreen()
        }

        val accentCard = card()
        text("Accent color", 17f, textColor, true, accentCard)
        val currentScheme = preferences.getString("accent_scheme", "teal")
        val currentLabel = accentSchemes.firstOrNull { it.id == currentScheme }?.label ?: accentSchemes[0].label
        button("Accent · $currentLabel", parent = accentCard) {
            val labels = accentSchemes.map { it.label }.toTypedArray()
            val selectedIndex = accentSchemes.indexOfFirst { it.id == currentScheme }.coerceAtLeast(0)
            AlertDialog.Builder(this)
                .setTitle("Accent color")
                .setSingleChoiceItems(labels, selectedIndex) { dialog, which ->
                    preferences.edit().putString("accent_scheme", accentSchemes[which].id).apply()
                    dialog.dismiss()
                    applyThemePalette()
                    settingsScreen()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }

        val host = card()
        text("Host analysis target ABI", 17f, textColor, true, host)
        val preferredAbi = preferences.getString("target_abi", "auto") ?: "auto"
        val abiLabel = when (preferredAbi) {
            "armeabi-v7a" -> "32-bit ARM (armeabi-v7a)"
            "arm64-v8a" -> "64-bit ARM (arm64-v8a)"
            else -> "Automatic · prefers 64-bit ARM64 when present"
        }
        button("Preferred host analysis ABI\n$abiLabel", parent = host) {
            val options = arrayOf(
                "Automatic (prefer ARM64 if the IPA contains it)",
                "32-bit ARM (armeabi-v7a)",
                "64-bit ARM (arm64-v8a)",
            )
            val selectedIndex = when (preferredAbi) { "armeabi-v7a" -> 1; "arm64-v8a" -> 2; else -> 0 }
            AlertDialog.Builder(this)
                .setTitle("Host analysis ABI")
                .setSingleChoiceItems(options, selectedIndex) { dialog, which ->
                    preferences.edit().putString("target_abi", when (which) {
                        1 -> "armeabi-v7a"
                        2 -> "arm64-v8a"
                        else -> "auto"
                    }).apply()
                    dialog.dismiss()
                    settingsScreen()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
        text("When analysis is requested, the automatic ABI setting prefers the ARM64 slice in a FAT IPA containing both 32-bit and 64-bit code. Supported ARM32-only inputs select a 32-bit ARMv7 target.", 12f, muted, parent = host)
        text("This preference only affects the copied host analysis command. It does not mean a game APK can currently be generated.", 12f, muted, parent = host)

        button("Restore defaults") {
            preferences.edit()
                .putBoolean("light_theme", false)
                .putString("accent_scheme", "teal")
                .putString("target_abi", "auto")
                .apply()
            applyThemePalette()
            settingsScreen()
        }
        button("Done", true) { home() }
    }

    private fun documentDisplayName(uri: Uri): String {
        val queried = try {
            contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val index = cursor.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (index >= 0 && cursor.moveToFirst()) cursor.getString(index) else null
            }
        } catch (_: Exception) { null }
        return queried?.takeIf { it.isNotBlank() }
            ?: uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
            ?: "Selected IPA"
    }

    private fun architectures(report: JSONObject): String {
        val slices = report.optJSONObject("machO")?.optJSONArray("slices") ?: return "unknown CPU"
        val architectures = (0 until slices.length())
            .map { slices.getJSONObject(it).getString("architecture") }
        val hasArm64 = architectures.any { it.startsWith("arm64", ignoreCase = true) }
        val hasArm32 = architectures.any {
            it.startsWith("armv", ignoreCase = true) || it.equals("arm", ignoreCase = true)
        }
        val ordered = if (hasArm64 && hasArm32) {
            architectures.sortedBy {
                when {
                    it.startsWith("arm64", ignoreCase = true) -> 0
                    it.startsWith("armv", ignoreCase = true) || it.equals("arm", ignoreCase = true) -> 1
                    else -> 2
                }
            }
        } else architectures
        val display = ordered.joinToString(" / ")
        return if (hasArm64 && hasArm32) "$display · AnyiOSEMU launches ARM32" else display
    }
    private fun authorize() {
        AlertDialog.Builder(this).setTitle("Authorized files only")
            .setMessage("Confirm that you own this IPA or have permission to convert it. Protection mechanisms will not be bypassed. The source IPA is retained in app-private storage for analysis until you delete this library entry. Force convert offers a Native diagnostic boot-attempt runtime and a standalone Semi-Emulated AnyiOSEMU APK for authorized ARM32 games. A source-free preview shell is a separate fallback. Complete-game host APKs still require the strict conversion contract.")
            .setNegativeButton("Cancel", null).setPositiveButton("I have permission") { _, _ ->
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "*/*"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerIpa)
            }.show()
    }
    private fun confirmDelete(dir: File) {
        if (SharedCatalog.isBusy()) return
        AlertDialog.Builder(this).setTitle("Delete shared entry from both modules?")
            .setMessage("Removes this source and local outputs from BOTH Emulator and Converter. Original external files are unchanged.")
            .setNegativeButton("Keep", null).setPositiveButton("Delete") { _, _ ->
                if (Jobs.tryRun {
                    Jobs.begin("Removing shared entry")
                    require(dir.canonicalFile.parentFile == library.root.canonicalFile) { "Invalid shared entry" }
                    require(dir.deleteRecursively()) { "Could not remove the complete entry" }
                }) home()
            }.show()
    }
    private fun detailArtwork(dir: File) {
        val artwork = FrameLayout(this).apply {
            background = SharedTheme.artwork(this@MainActivity, 20)
            clipToOutline = true
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val placeholder = ImageView(this).apply {
            setImageResource(dev.radek.conventor.R.drawable.ic_game_outline)
            imageTintList = android.content.res.ColorStateList.valueOf(accent)
        }
        artwork.addView(placeholder, FrameLayout.LayoutParams(dp(36), dp(36), Gravity.CENTER))
        val iconPath = sharedIcon(dir)
        if (iconPath.isFile) {
            val icon = ImageView(this).apply { scaleType = ImageView.ScaleType.CENTER_CROP }
            artwork.addView(icon, FrameLayout.LayoutParams(-1, -1))
            icon.post { IconLoader.load(iconPath, dp(96), icon) }
        }
        body.addView(artwork, LinearLayout.LayoutParams(dp(96), dp(96)).apply {
            gravity = Gravity.CENTER_HORIZONTAL; topMargin = dp(14)
        })
    }
    private fun detail(dir: File, restoreScrollY: Int = 0) {
        selected = dir; screen(Screen.DETAIL)
        if (Jobs.busy && returnToDetailAfterJob == dir) addDetailJobOverlay()
        val scroll = currentScroll
        if (restoreScrollY > 0) scroll?.post { if (currentScroll === scroll) scroll.scrollTo(0, restoreScrollY) }
        val report = try {
            (if (File(dir, "report.json").isFile) JSONObject(File(dir, "report.json").readText()) else library.sharedReport(dir)) ?: error("missing shared entry")
        } catch (_: Exception) {
            Toast.makeText(this, "Library entry is unreadable; returning to the library", Toast.LENGTH_LONG).show()
            home()
            return
        }
        val name = library.displayTitle(dir, report)
        button("Game Library") { home() }
        detailArtwork(dir)
        text(name, 28f, textColor, true)
        if (report.optString("state") == "SHARED_SOURCE") {
            text("Not analyzed", 12f, muted, true)
            text("One source copy, shared with Emulator. Analysis and generated APKs never replace it.", 15f, muted)
            val shared = library.metadata(dir)
            val sourceInfo = card()
            for ((label, value) in listOf(
                "Source" to shared?.optString("kind").orEmpty(),
                "Architecture" to shared?.optString("architecture").orEmpty(),
                "Bundle ID" to shared?.optString("bundleId").orEmpty(),
                "Version" to shared?.optString("version").orEmpty(),
            )) {
                if (value.isNotBlank()) {
                    text(label.uppercase(Locale.ROOT), 10f, muted, true, sourceInfo)
                    text(value, 15f, parent = sourceInfo)
                }
            }
            text(report.optString("nextBlocker"), 14f, muted)
            if (report.optString("sharedSourceKind") in setOf("IPA", "Folder", "Executable")) {
                if (report.optString("sharedSourceKind") == "IPA")
                    text("Explicit analysis may also build an APK only for a narrowly proven single-routine IPA.", 12f, muted)
                button("Analyze shared source", true) { startSharedAnalysis(dir) }.isEnabled = !SharedCatalog.isBusy()
            } else text("This source type cannot currently be analyzed here. It remains available in Emulator.", 14f, muted)
            button("Delete shared entry") { confirmDelete(dir) }
            return
        }
        val app = report.optJSONObject("application") ?: JSONObject()
        val ipaSource = SourcePolicy.supportsIpaPackaging(report)
        val state = report.optString("state"); text(state, 12f, statusColor(state), true)
        val metadata = card()
        for ((name, value) in listOf(
            "Bundle ID" to app.optString("bundleId"),
            "Version / build" to "${app.optString("version")} / ${app.optString("build")}",
            "Minimum iOS" to app.optString("minimumIOSVersion").ifBlank { "Not declared in Info.plist" },
            "Architecture" to architectures(report),
            (if (ipaSource) "IPA size" else "Source size") to formatBytes(app.optLong("fileSize")),
            "Executable" to app.optString("executable"),
        )) {
            text(name.uppercase(), 10f, muted, true, metadata); text(value, 15f, parent = metadata)
        }
        // Completed analysis/coverage and resolved items live in the full JSON;
        // the ordinary report focuses on work that remains or failed.
        report.optJSONObject("analysisProgress")?.takeIf {
            Jobs.busy && it.optString("status") == "RUNNING" && it.optInt("percent", 0) < 100
        }?.let { analysis ->
            val progress = card()
            val value = analysis.optInt("percent", 0)
            text("Analysis in progress · $value%", 16f, textColor, true, progress)
            progressIndicator(progress, value, 10)
            text(analysis.optString("message"), 12f, muted, parent = progress)
        }
        text("Compatibility report", 22f, textColor, true)
        val issues = CompatibilityIssues.summarize(report)
        if (issues.isEmpty()) {
            text("No outstanding issues are listed. Gameplay is not verified.", 14f, muted)
        } else {
            issues.problems.take(4).forEach { text(it, 15f, statusColor("BLOCKED")) }
            if (issues.problems.size > 4) text("${issues.problems.size - 4} more issue(s) in the report.", 13f, muted)
            if (issues.unresolvedImports > 0)
                text("${issues.unresolvedImports} imported API(s) still need work or verification.", 14f, textColor, true)
            if (issues.importsIncomplete) text("Import analysis is incomplete; more issues may be missing.", 13f, statusColor("BLOCKED"))
            button("View outstanding issues") { showText("Outstanding compatibility issues", issues.format()) }
            button("Export outstanding issues") {
                val bundleId = app.optString("bundleId")
                val output = ndkNeedsHeader(app.optString("name"), bundleId, app.optString("version")) + issues.format()
                val safeBundle = bundleId.ifBlank { "game" }.replace(Regex("[^A-Za-z0-9._-]"), "_")
                saveNeedsText(output, "$safeBundle-compatibility-issues.txt")
            }
        }
        if (File(dir, "game.json").isFile && SourcePolicy.canRetryFailedAnalysis(report)) {
            text("The shared source and previous report are kept. Retry analysis without importing again; this does not rebuild an APK.", 13f, muted)
            button("Retry analysis") { startSharedAnalysis(dir) }.isEnabled = !SharedCatalog.isBusy()
        }
        button("View full machine-readable report") { showText("Conversion report", report.toString(2)) }
        button("View real conversion logs") { showText("Logs", File(dir, "conversion.jsonl").takeIf { it.isFile }?.readText() ?: "No logs") }
        text("APK conversion", 22f, textColor, true)
        if (!ipaSource && report.optJSONObject("source")?.optString("kind") in setOf("FOLDER", "EXECUTABLE")) {
            text("This source was analyzed in place. Import an IPA to build a standalone APK; folder and executable packaging is not yet supported.", 14f, muted)
        }
        text("A general iOS-to-Android game static recompilation backend and framework/API replacements are not implemented. The Native runtime is the former diagnostic guest-CPU boot-attempt path; its renderer is not connected to Android EGL/GLES. The Semi-Emulated runtime packages AnyiOSEMU and the original ARM32 IPA into an independent APK. Neither path guarantees compatibility or gameplay. A branded preview shell with no executable remains an explicit fallback.", 14f, muted)
        button("Copy host analysis command") {
            val abi = preferences.getString("target_abi", "auto") ?: "auto"
            val suffix = if (abi == "auto") "" else " --target-abi $abi"
            val command = "python3 -m radek analyze input.ipa --authorized --output workspace/analysis-result$suffix"
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("Host analysis command", command))
            Toast.makeText(this, "Analysis command copied", Toast.LENGTH_SHORT).show()
        }
        button("Copy host NDK/CMake relink command") {
            val command = "python3 -m radek relink workspace/analysis-result/bytecode-translation --authorized --backend cmake"
            (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager).setPrimaryClip(android.content.ClipData.newPlainText("Host NDK/CMake relink command", command))
            Toast.makeText(this, "Host command copied; import the verified handoff after relinking", Toast.LENGTH_LONG).show()
        }
        text("NDK/CMake relinks generated portable C on a computer. It does not compile arbitrary iOS binaries or add a renderer.", 12f, muted)
        if (ipaSource && app.has("sha256")) button("Attach complete host-converted APK") {
            if (!Jobs.busy) startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "application/vnd.android.package-archive"; addCategory(Intent.CATEGORY_OPENABLE) }, pickerApk)
        }
        val hostConversion = report.optJSONObject("hostConversion")
        val hostAttached = hostConversion != null && hostConversion.optString("status") == "ATTACHED" &&
            hostConversion.optBoolean("completeGameConversion", false) && hostConversion.optString("contract") == "complete-game-v1"
        val expectedHostName = ArtifactNames.apkFileName(report)
        val reportedHostName = hostConversion?.optString("artifact").orEmpty()
        val hostOutputFile = if (hostAttached && reportedHostName == expectedHostName) {
            File(dir, expectedHostName).takeIf { it.isFile }
        } else null
        if (hostAttached && hostOutputFile == null) {
            text("The attached APK is missing or its IPA-basename record does not match; it cannot be installed or shared.", 13f, statusColor("FAILED"))
        }
        if (hostOutputFile != null) {
            val hostAbi = hostConversion?.optString("targetAbi").orEmpty()
            val hostOrigin = if (hostConversion?.optString("origin") == "ON_DEVICE")
                "Complete-game conversion built on-device · ABI $hostAbi. The bounded conversion proof and package checks passed; on-device gameplay has not been tested."
            else
                "Complete-game host APK attached · ABI $hostAbi. The host conversion contract passed static checks; on-device execution and actual gameplay have not been tested."
            text(hostOrigin, 13f, accent)
            text("Statically recompiled reachable functions: ${hostConversion?.optInt("recompiledReachableFunctions", 0) ?: 0} · generated API replacements: ${hostConversion?.optInt("generatedApiReplacements", 0) ?: 0} · unresolved reachable functions: ${hostConversion?.optInt("notRecompiledReachableFunctions", -1) ?: -1}.", 12f, muted)
            button("Install ${hostOutputFile.name}", true) { installArtifact(dir, hostOutputFile.name) }
            button("Share ${hostOutputFile.name}") { shareResultApk(dir, hostOutputFile.name) }
            if (hostConversion?.optString("origin") == "ON_DEVICE") {
                val deviceResult = report.optJSONObject("deviceConversion")
                deviceResult?.optJSONObject("expansionObb")?.optString("name")?.takeIf { it.isNotBlank() }?.let { obb ->
                    text("Optional resource OBB: $obb. The APK keeps all resources; the launcher does not require the OBB.", 12f, muted)
                    button("Share $obb") { shareResultExpansion(dir, obb, "bounded") }
                }
                deviceResult?.optJSONObject("expansionObbStatus")?.let { status ->
                    if (status.optString("status") == "NOT_CREATED") text(status.optString("message"), 12f, muted)
                }
            }
            if (app.has("sha256")) button("Open installed converted program") {
                val pkg = report.optJSONObject("deviceConversion")?.optString("package").orEmpty()
                    .ifBlank { hostConversion?.optString("package").orEmpty() }
                    .ifBlank { "dev.radek.converted.p" + app.getString("sha256").take(20) }
                val intent = packageManager.getLaunchIntentForPackage(pkg)
                if (intent == null) Toast.makeText(this, "Converted program is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        val placeholderConversion = report.optJSONObject("placeholderConversion")
        val placeholderName = ArtifactNames.placeholderApkFileName(report)
        val placeholderOutputFile = if (placeholderConversion?.optString("status") == "GENERATED") {
            runCatching { PlaceholderArtifactContract.validate(report, dir, placeholderName) }.getOrNull()
        } else null
        if (placeholderConversion?.optString("status") == "GENERATED" && placeholderOutputFile == null) {
            text("The preview APK is missing or its digest/metadata is invalid; it cannot be installed or shared.", 13f, statusColor("FAILED"))
        }
        report.optJSONObject("placeholderBuildProgress")?.takeIf { it.optString("status") == "FAILED" }?.let { build ->
            text("Preview shell build failed: ${build.optString("message")}", 13f, statusColor("FAILED"))
        }
        report.optJSONObject("convertedBuildProgress")?.takeIf { it.optString("status") == "FAILED" }?.let { build ->
            text("Conversion build failed: ${build.optString("message")}", 13f, statusColor("FAILED"))
        }
        if (placeholderOutputFile != null) {
            val iconDescription = when (placeholderConversion?.optString("iconSource")) {
                "RECOVERED_IPA_ICON" -> "Original IPA icon included"
                "GENERATED_APP_NAME_ICON" -> "Generated name-based icon included; no original icon was recovered"
                else -> "Fallback icon included; no original icon was recovered"
            }
            text("Installable preview APK · $iconDescription", 13f, accent, true)
            text("This is a branded shell built from the IPA name and icon only. No iOS executable, statically recompiled game code, or playable gameplay is included.", 12f, muted)
            button("Install ${placeholderOutputFile.name}", true) { installArtifact(dir, placeholderOutputFile.name) }
            button("Share ${placeholderOutputFile.name}") { shareResultApk(dir, placeholderOutputFile.name) }
        }
        val gameRuntimeConversion = report.optJSONObject("gameRuntimeConversion")
        val gameName = ArtifactNames.gameApkFileName(report)
        val gameOutputFile = if (gameRuntimeConversion?.optString("status") == "GENERATED") {
            runCatching { GameRuntimeArtifactContract.validate(report, dir, gameName) }.getOrNull()
        } else null
        if (gameRuntimeConversion?.optString("status") == "GENERATED" && gameOutputFile == null) {
            text("The Native runtime APK is missing or its digest/metadata is invalid; it cannot be installed or shared.", 13f, statusColor("FAILED"))
        }
        report.optJSONObject("gameRuntimeBuildProgress")?.takeIf { it.optString("status") == "FAILED" }?.let { build ->
            text("Native runtime build failed: ${build.optString("message")}", 13f, statusColor("FAILED"))
        }
        if (gameOutputFile != null) {
            val translated = gameRuntimeConversion?.optBoolean("translatedPortableCIncluded", false) == true
            text("Native runtime · independent diagnostic boot-attempt APK", 13f, accent, true)
            text(if (translated) "Includes the validated portable-C handoff. The runner has no Android EGL/GLES renderer; gameplay is unverified."
                else "Runs the original ARM32 code in the diagnostic guest runtime. It has no Android EGL/GLES renderer and may stop at an unsupported call; gameplay is unverified.", 12f, muted)
            button("Install ${gameOutputFile.name}", true) { installArtifact(dir, gameOutputFile.name) }
            button("Share ${gameOutputFile.name}") { shareResultApk(dir, gameOutputFile.name) }
            gameRuntimeConversion?.optJSONObject("expansionObb")?.let { expansion ->
                val obb = expansion.optString("name")
                if (obb.isNotBlank()) {
                    val embedded = expansion.optBoolean("alsoEmbeddedInApk", false) && !expansion.optBoolean("required", true)
                    text(if (embedded) "Optional game-resource OBB: $obb. The APK also contains the resources."
                        else "Older build may have resources only in $obb, which the diagnostic launcher cannot extract. Rebuild for a self-contained APK.", 12f, muted)
                    button("Share $obb") { shareResultExpansion(dir, obb, "native") }
                }
            }
            gameRuntimeConversion?.optJSONObject("expansionObbStatus")?.let { status ->
                if (status.optString("status") == "NOT_CREATED")
                    text(status.optString("message"), 12f, muted)
            }
            button("Open installed Native runtime") {
                val intent = gameRuntimeConversion?.optString("package")?.let { packageManager.getLaunchIntentForPackage(it) }
                if (intent == null) Toast.makeText(this, "Native runtime is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        val anyioseConversion = report.optJSONObject("anyioseConversion")
        val anyioseName = ArtifactNames.anyioseApkFileName(report)
        val anyioseFile = if (anyioseConversion?.optString("status") == "GENERATED") {
            runCatching { AnyiOSEmuArtifactContract.validate(report, dir, anyioseName) }.getOrNull()
        } else null
        if (anyioseConversion?.optString("status") == "GENERATED" && anyioseFile == null)
            text("The Semi-Emulated APK is missing or failed its integrity check.", 13f, statusColor("FAILED"))
        report.optJSONObject("anyioseBuildProgress")?.takeIf { it.optString("status") == "FAILED" }?.let { build ->
            text("Semi-Emulated build failed: ${build.optString("message")}", 13f, statusColor("FAILED"))
        }
        if (anyioseFile != null) {
            text("Semi-Emulated · standalone AnyiOSEMU game APK", 13f, accent, true)
            text("Contains the original IPA, AnyiOSEMU, SDL and ANGLE. It starts this game directly and keeps its own private saves, so AnyiOS is not required after installation. ARM32 compatibility and gameplay still need device testing.", 12f, muted)
            button("Install ${anyioseFile.name}", true) { installArtifact(dir, anyioseFile.name) }
            button("Share ${anyioseFile.name}") { shareResultApk(dir, anyioseFile.name) }
            anyioseConversion?.optJSONObject("expansionObb")?.optString("name")?.takeIf { it.isNotBlank() }?.let { obb ->
                text("The game creates an optional OBB copy on first launch when Android allows it. This uses extra storage and may slow startup; the APK stays self-contained and private data remains authoritative.", 12f, muted)
                button("Share $obb") { shareResultExpansion(dir, obb, "anyiose") }
            }
            button("Open installed Semi-Emulated game") {
                val intent = anyioseConversion?.optString("package")?.let { packageManager.getLaunchIntentForPackage(it) }
                if (intent == null) Toast.makeText(this, "Game is not installed or not visible to Android", Toast.LENGTH_LONG).show() else startActivity(intent)
            }
        }
        if (ipaSource && File(dir, "source.bin").isFile && app.has("sha256")) {
            val deviceConvertible = report.optJSONObject("deviceRecompilation")?.let {
                it.optString("status") == "PROVEN" && it.optInt("coveragePercent", 0) == 100 && it.optInt("functionCount", 0) == 1
            } == true
            val translatedInput = File(dir, "translated-game-runtime-input.zip")
            text("Native builds the former diagnostic boot-attempt runtime. Semi-Emulated embeds AnyiOSEMU and this IPA in an independent APK; it supports ARM32 games only. Neither bypasses encryption or guarantees gameplay.", 12f, muted)
            button(if (translatedInput.isFile) "Replace Native portable-C handoff ZIP" else "Import Native portable-C handoff ZIP") {
                chooseTranslatedRuntimeInput(dir)
            }
            dangerButton("Force convert · choose runtime") {
                showForceConvertChoices(dir, gameOutputFile != null, anyioseFile != null, translatedInput.isFile)
            }
            button("Convert settings · Semi-Emulated") { showConvertSettings(dir) }
            if (deviceConvertible && hostOutputFile == null)
                button("Build proven single-routine APK") { startForceConvert(dir, true) }
            if (!deviceConvertible)
                button(if (placeholderOutputFile != null) "Rebuild preview APK" else "Build preview shell instead") { startForceConvert(dir, false) }
        }
        button("Delete shared entry") { confirmDelete(dir) }
    }

    private fun startSharedAnalysis(dir: File) {
        if (Jobs.busy) { Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show(); return }
        Jobs.begin("Analyzing shared source")
        returnToDetailAfterJob = dir
        wasBusy = true
        val started = Jobs.tryRun {
            library.analyzeShared(dir) { percent, message -> Jobs.update(percent, message) }
        }
        if (!started) {
            wasBusy = false; returnToDetailAfterJob = null
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        detail(dir, currentScroll?.scrollY ?: 0)
    }

    private fun showConvertSettings(dir: File) {
        if (Jobs.busy) { Toast.makeText(this, "Wait for the current job to finish", Toast.LENGTH_SHORT).show(); return }
        val saved = try { ConvertSettings.load(dir) } catch (error: Exception) {
            Toast.makeText(this, "Cannot read convert settings: ${error.message}", Toast.LENGTH_LONG).show(); return
        }
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (20 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
        }
        fun explanation(value: String) {
            content.addView(TextView(this).apply { text = value; textSize = 14f })
        }
        fun toggle(label: String, checked: Boolean): CheckBox = CheckBox(this).apply {
            text = label; isChecked = checked; content.addView(this)
        }
        explanation("These per-game choices are built into new Semi-Emulated APKs only. Rebuild and reinstall to apply changes. Native diagnostic and other APKs do not use them.")
        explanation("Display profile · choose one (landscape dimensions shown). Both classic profiles request 2× internal rendering; this uses extra GPU memory and time. Game-owned textures may remain at their original size.")
        val profiles = listOf("Default / game decides", "3:2 · 480 × 320 · 2× scale", "4:3 · 1024 × 768 · 2× scale")
        val profile = Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, profiles)
            setSelection(if (saved.classic320) 1 else if (saved.classic1024) 2 else 0)
            content.addView(this)
        }
        explanation("At content scale 1, full-screen landscape drawables request 960 × 640 or 2048 × 1536 pixels respectively; portrait swaps the axes. Games, layer sizes and hardware limits can affect the result.")
        val offline = toggle("Force network access off", saved.offline)
        explanation("The generated APK does not request Android's Internet permission; the emulator's guest network APIs are disabled too.")
        val performance = toggle("Request maximum performance", saved.performance)
        explanation("Best-effort Android sustained-performance, CPU affinity and performance hints. The system controls CPU/GPU clocks; this does not guarantee maximum clocks or higher FPS and may increase heat or battery use.")
        explanation("Startup rotation from portrait; the game may later request another orientation. Touch input follows the orientation.")
        val orientationNames = mutableListOf("Default / game decides", "−90°", "+90°", "180°")
        val orientationValues = mutableListOf("default", "-90", "90", "180")
        if (saved.orientation == "portrait") {
            // Preserve an existing explicit portrait choice until changed.
            orientationNames.add("Previously selected: portrait")
            orientationValues.add("portrait")
        }
        val orientation = Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, orientationNames)
            setSelection(orientationValues.indexOf(saved.orientation).coerceAtLeast(0))
            content.addView(this)
        }
        val scroll = ScrollView(this).apply { addView(content) }
        AlertDialog.Builder(this).setTitle("Convert settings").setView(scroll)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Save") { _, _ ->
                try {
                    ConvertSettings.save(dir, ConvertSettings(
                        classic320 = profile.selectedItemPosition == 1,
                        offline = offline.isChecked,
                        performance = performance.isChecked,
                        orientation = orientationValues[orientation.selectedItemPosition.coerceIn(0, orientationValues.lastIndex)],
                        classic1024 = profile.selectedItemPosition == 2,
                    ))
                    Toast.makeText(this, "Saved for the next Semi-Emulated build", Toast.LENGTH_LONG).show()
                } catch (error: Exception) {
                    Toast.makeText(this, "Could not save convert settings: ${error.message}", Toast.LENGTH_LONG).show()
                }
            }.show()
    }

    private fun showForceConvertChoices(dir: File, nativeBuilt: Boolean, semiEmulatedBuilt: Boolean, translatedInputPresent: Boolean) {
        val bundleId = runCatching { JSONObject(File(dir, "report.json").readText())
            .optJSONObject("application")?.optString("bundleId").orEmpty() }.getOrDefault("")
        val exactId = ArtifactNames.androidPackageName(bundleId, "a".repeat(64), "b".repeat(64)) == bundleId
        val identityNote = if (exactId)
            "Both variants use Android package $bundleId and its matching OBB folder. Installing one may replace the other or conflict with an existing app; back up its data first. "
        else "This iOS bundle ID cannot be used as an Android package; a safe generated package and matching OBB folder will be used instead. "
        val message = identityNote + "\n\nNative runtime builds the existing diagnostic guest-CPU boot-attempt APK. " +
            (if (translatedInputPresent) "The staged portable-C handoff will be revalidated. " else "") +
            "It does not have an Android EGL/GLES renderer.\n\n" +
            "Semi-Emulated builds a self-contained APK containing AnyiOSEMU and the IPA, launches only this game, " +
            "and uses the device's current resolution and fastest supported refresh mode. AnyiOS can be uninstalled afterwards. " +
            "Per-game Convert settings apply only to new Semi-Emulated builds. " +
            "It requests high-refresh display-link callbacks; this can speed up game logic. " +
            "It needs an unencrypted ARM32 slice; rendering, FPS and gameplay depend on the game and device." +
            (if (nativeBuilt || semiEmulatedBuilt) "\n\nSelecting an existing runtime rebuilds that APK." else "")
        AlertDialog.Builder(this).setTitle("Choose a runtime").setMessage(message)
            .setPositiveButton("Native runtime") { _, _ -> startGameRuntimeBuild(dir) }
            .setNeutralButton("Semi-Emulated runtime") { _, _ -> startAnyiOSEmuBuild(dir) }
            .setNegativeButton("Cancel", null).show()
    }

    private fun startAnyiOSEmuBuild(dir: File) {
        if (Jobs.busy) { Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show(); return }
        if (!File(dir, "source.bin").isFile) { Toast.makeText(this, "Retained IPA not found", Toast.LENGTH_LONG).show(); return }
        Jobs.begin("Building standalone Semi-Emulated game APK")
        returnToDetailAfterJob = dir
        wasBusy = true
        val started = Jobs.tryRun {
            AnyiOSEmuApkBuilder(applicationContext).build(dir) { percent, message -> Jobs.update(percent, message) }
        }
        if (!started) {
            wasBusy = false; returnToDetailAfterJob = null
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show(); return
        }
        detail(dir, currentScroll?.scrollY ?: 0)
    }

    private fun startForceConvert(dir: File, convertible: Boolean) {
        if (Jobs.busy) {
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        if (!File(dir, "source.bin").isFile) {
            Toast.makeText(this, "Retained IPA not found; nothing can be built", Toast.LENGTH_LONG).show()
            return
        }
        Jobs.begin(if (convertible) "Converting proven IPA" else "Building installable preview shell")
        returnToDetailAfterJob = dir
        wasBusy = true
        val started = Jobs.tryRun {
            if (convertible) {
                ConvertedApkBuilder(applicationContext).build(dir) { percent, message ->
                    Jobs.update(percent, message)
                }
            } else {
                PlaceholderApkBuilder(applicationContext).build(dir) { percent, message ->
                    Jobs.update(percent, message)
                }
            }
        }
        if (!started) {
            wasBusy = false
            returnToDetailAfterJob = null
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        detail(dir, currentScroll?.scrollY ?: 0)
    }

    private fun chooseTranslatedRuntimeInput(dir: File) {
        if (Jobs.busy) {
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        val picker = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            type = "application/zip"
            addCategory(Intent.CATEGORY_OPENABLE)
        }
        startActivityForResult(picker, pickerTranslatedRuntimeInput)
    }

    private fun importTranslatedRuntimeInput(uri: Uri, dir: File) {
        if (Jobs.busy) return
        Jobs.begin("Importing verified portable-C handoff")
        returnToDetailAfterJob = dir
        wasBusy = true
        val started = Jobs.tryRun {
            val destination = File(dir, "translated-game-runtime-input.zip")
            val temporary = File(dir, ".translated-game-runtime-input.importing")
            val backup = File(dir, ".translated-game-runtime-input.previous")
            temporary.delete()
            backup.delete()
            try {
                contentResolver.openInputStream(uri)?.use { input ->
                    FileOutputStream(temporary).use { output ->
                        val buffer = ByteArray(64 * 1024)
                        var total = 0L
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            total += count
                            require(total <= maxTranslatedRuntimeInputBytes) {
                                "translated runtime handoff exceeds the 1 GiB import limit"
                            }
                            output.write(buffer, 0, count)
                        }
                        output.fd.sync()
                        require(total > 0) { "selected translated runtime handoff is empty" }
                    }
                } ?: error("selected translated runtime handoff could not be opened")
                SafeZip.requireStorage(dir, temporary.length())
                ZipFile(temporary).use { archive ->
                    val names = archive.entries().asSequence().map { it.name }.toList()
                    require(names.size == 5 && names.toSet() == setOf(
                        "manifest.json", "libtranslated_game.so", "translated-game-payload.zip",
                        "android-link-report.json", "rt_report.json",
                    )) { "selected ZIP is not a translated game-runtime input bundle" }
                    val manifestEntry = archive.getEntry("manifest.json")
                        ?: error("translated handoff is missing manifest.json")
                    require(manifestEntry.size in 1..(1024L * 1024)) { "translated handoff manifest is invalid" }
                    val manifestText = archive.getInputStream(manifestEntry).bufferedReader(Charsets.UTF_8).use { reader ->
                        val output = StringBuilder()
                        val buffer = CharArray(8192)
                        var total = 0
                        while (true) {
                            val count = reader.read(buffer)
                            if (count < 0) break
                            total += count
                            require(total <= 1024 * 1024) { "translated handoff manifest exceeds its size limit" }
                            output.append(buffer, 0, count)
                        }
                        output.toString()
                    }
                    val manifest = JSONObject(manifestText)
                    require(manifest.optString("contract") == "translated-game-runtime-input-v1" &&
                        manifest.optInt("schemaVersion", 0) == 1) {
                        "selected ZIP does not use translated-game-runtime-input-v1"
                    }
                }
                if (destination.exists()) require(destination.renameTo(backup)) {
                    "existing translated handoff could not be preserved"
                }
                if (!temporary.renameTo(destination)) {
                    if (backup.isFile) backup.renameTo(destination)
                    error("translated handoff could not be saved")
                }
                backup.delete()
                Jobs.update(100, "Hash-bound portable-C handoff imported; the APK builder will revalidate it")
            } catch (error: Throwable) {
                temporary.delete()
                if (backup.isFile && !destination.exists()) backup.renameTo(destination)
                throw error
            }
        }
        if (!started) {
            wasBusy = false
            returnToDetailAfterJob = null
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        detail(dir, currentScroll?.scrollY ?: 0)
    }

    private fun startGameRuntimeBuild(dir: File) {
        if (Jobs.busy) {
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        if (!File(dir, "source.bin").isFile) {
            Toast.makeText(this, "Retained IPA not found; nothing can be built", Toast.LENGTH_LONG).show()
            return
        }
        Jobs.begin(
            if (File(dir, "translated-game-runtime-input.zip").isFile)
                "Building translated Native runtime APK"
            else
                "Building Native boot-attempt APK",
        )
        returnToDetailAfterJob = dir
        wasBusy = true
        val started = Jobs.tryRun {
            GameRuntimeApkBuilder(applicationContext).build(dir) { percent, message ->
                Jobs.update(percent, message)
            }
        }
        if (!started) {
            wasBusy = false
            returnToDetailAfterJob = null
            Toast.makeText(this, "A job is already running", Toast.LENGTH_SHORT).show()
            return
        }
        detail(dir, currentScroll?.scrollY ?: 0)
    }

    private fun shareResultApk(dir: File, name: String) {
        val file = File(dir, name)
        if (!file.isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        val uri = Uri.Builder().scheme("content").authority("${packageName}.converter.results")
            .appendPath(dir.name).appendPath(file.name).build()
        val share = Intent(Intent.ACTION_SEND).apply {
            type = "application/vnd.android.package-archive"
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = android.content.ClipData.newUri(contentResolver, file.name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(share, "Share APK result"))
    }

    private fun shareResultExpansion(dir: File, name: String, kind: String) {
        // ResultProvider validates the exact name, source binding and digest.
        // On recent Android, the user may not be able to pick Android/obb in
        // the system file picker; the standalone game provisions its own copy.
        val uri = Uri.Builder().scheme("content").authority("${packageName}.converter.results")
            .appendPath(dir.name).appendPath(name).appendQueryParameter("kind", kind).build()
        val share = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, uri)
            clipData = android.content.ClipData.newUri(contentResolver, name, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(share, "Share expansion file"))
    }

    private fun ndkNeedsHeader(name: String, bundleId: String, version: String): String {
        val stamp = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(java.util.Date())
        return "Game: ${name.ifBlank { "unknown" }}\nBundle ID: ${bundleId.ifBlank { "unknown" }}\n" +
            "Version: ${version.ifBlank { "unknown" }}\nExported: $stamp\n\n"
    }

    /** Saves the NDK needs list to Downloads straight away: no dialog, no file picker. */
    private fun saveNeedsText(text: String, name: String) {
        try {
            Toast.makeText(this, "Saved NDK needs list to ${downloadNeedsText(text, name)}", Toast.LENGTH_LONG).show()
        } catch (error: Exception) {
            Toast.makeText(this, "Could not save the NDK needs list: ${error.message}", Toast.LENGTH_LONG).show()
        }
    }

    /** Writes the NDK needs list to Downloads and returns the path it was written to. */
    private fun downloadNeedsText(text: String, name: String): String {
        val bytes = text.toByteArray(Charsets.UTF_8)
        if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, "text/plain")
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: error("MediaStore refused to create the download")
            contentResolver.openOutputStream(uri, "wt")?.use { it.write(bytes) }
                ?: error("the download could not be opened for writing")
            values.clear()
            values.put(MediaStore.Downloads.IS_PENDING, 0)
            contentResolver.update(uri, values, null, null)
            return "Downloads/$name"
        }
        // Android 8-9: the public Downloads folder needs WRITE_EXTERNAL_STORAGE, which
        // would mean a permission prompt. Fall back to the app's own Downloads folder
        // (no permission needed) when the public write is refused.
        return try {
            val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
            dir.mkdirs()
            File(dir, name).also { it.writeBytes(bytes) }.absolutePath
        } catch (error: SecurityException) {
            val dir = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: filesDir
            File(dir, name).also { it.writeBytes(bytes) }.absolutePath
        }
    }

    private fun showText(title: String, value: String) {
        val view = TextView(this).apply { text = value; setTextIsSelectable(true); textSize = 12f; typeface = Typeface.MONOSPACE; setPadding(dp(16), dp(16), dp(16), dp(16)) }
        AlertDialog.Builder(this).setTitle(title).setView(ScrollView(this).apply { addView(view) }).setPositiveButton("Close", null).show()
    }
    @Deprecated("Framework result API retained to avoid third-party dependencies")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data?.data == null) return
        val uri = data.data!!
        if (Jobs.busy) return
        val context = applicationContext
        if (requestCode == pickerIpa) {
            if (Jobs.busy) {
                Toast.makeText(this, "A job is already running; wait for it to finish first", Toast.LENGTH_LONG).show()
                return
            }
            Jobs.begin("Analyzing · ${documentDisplayName(uri)}")
            Jobs.update(0, "Starting IPA analysis; the bounded conversion proof runs automatically")
            val started = Jobs.tryRun {
                val library = Library(context)
                val (dir, report) = library.import(uri) { percent, message -> Jobs.update(percent, message) }
                returnToDetailAfterJob = dir
                val state = report.optString("state", "FAILED")
                Jobs.update(100, "$state · IPA analysis finished · APK ${report.optJSONObject("conversionProgress")?.optString("status", "NOT_BUILT") ?: "NOT_BUILT"}")
            }
            if (started) { wasBusy = true; openImportedResultAfterJob = true; home() }
        } else if (requestCode == pickerApk) {
            val dir = selected ?: return
            if (Jobs.busy) {
                Toast.makeText(this, "A job is already running; wait for it to finish first", Toast.LENGTH_LONG).show()
                return
            }
            Jobs.begin("Attaching host APK")
            returnToDetailAfterJob = dir
            val started = Jobs.tryRun { attachApk(context, uri, dir); Jobs.update("Host APK attached") }
            if (started) { wasBusy = true; detail(dir, currentScroll?.scrollY ?: 0) }
            else returnToDetailAfterJob = null
        } else if (requestCode == pickerTranslatedRuntimeInput) {
            val dir = selected ?: return
            importTranslatedRuntimeInput(uri, dir)
        }
    }
    private fun installArtifact(dir: File, name: String) {
        val file = File(dir, name)
        if (!file.isFile) {
            Toast.makeText(this, "APK result not found", Toast.LENGTH_LONG).show()
            return
        }
        if (!packageManager.canRequestPackageInstalls()) {
            pendingInstall = dir to name
            startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            return
        }
        pendingInstall = null
        // Check the artifact ourselves first: Android's installer reports every
        // failure as "app not installed", which leaves nothing to act on.
        val expectedPackage = runCatching {
            val report = JSONObject(File(dir, "report.json").readText())
            when (name) {
                ArtifactNames.anyioseApkFileName(report) -> report.optJSONObject("anyioseConversion")?.optString("package").orEmpty()
                ArtifactNames.gameApkFileName(report) -> report.optJSONObject("gameRuntimeConversion")?.optString("package").orEmpty()
                ArtifactNames.placeholderApkFileName(report) -> report.optJSONObject("placeholderConversion")?.optString("package").orEmpty()
                else -> report.optJSONObject("deviceConversion")?.optString("package").orEmpty()
                    .ifBlank { report.optJSONObject("hostConversion")?.optString("package").orEmpty() }
            }
        }.getOrDefault("")
        if (expectedPackage.isNotBlank()) {
            val audit = runCatching { InstallAudit.inspect(this, file, expectedPackage) }.getOrNull()
            if (audit != null && (!audit.installable || audit.warnings.isNotEmpty())) {
                showInstallAudit(audit) { launchInstall(dir, name) }
                return
            }
        }
        launchInstall(dir, name)
    }

    private fun showInstallAudit(audit: InstallAudit.Report, proceed: () -> Unit) {
        val lines = ArrayList<String>()
        lines += audit.blockers.map { "• $it" }
        lines += audit.warnings.map { "• $it" }
        val message = buildString {
            if (audit.blockers.isNotEmpty()) {
                append("This APK cannot be installed:\n\n").append(audit.blockers.joinToString("\n") { "• $it" })
            } else {
                append("Check before installing:\n\n").append(audit.warnings.joinToString("\n") { "• $it" })
            }
        }
        AlertDialog.Builder(this)
            .setTitle(if (audit.installable) "Before you install" else "This APK will not install")
            .setMessage(message)
            .setPositiveButton(if (audit.installable) "Install anyway" else "Close", null)
            .setNegativeButton("Cancel") { _, _ -> }
            .create()
            .apply {
                setOnShowListener {
                    getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                        dismiss()
                        if (audit.installable) proceed()
                    }
                }
            }
            .show()
    }

    private fun launchInstall(dir: File, name: String) {
        val uri = Uri.Builder().scheme("content").authority("${packageName}.converter.results")
            .appendPath(dir.name).appendPath(name).build()
        runCatching {
            startActivity(Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
        }.onFailure {
            Toast.makeText(this, "No app on this device can install APKs", Toast.LENGTH_LONG).show()
        }
    }
    companion object {
        private fun sha256(input: java.io.InputStream): String {
            val digest = java.security.MessageDigest.getInstance("SHA-256")
            val buffer = ByteArray(65536)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
            return digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
        }

        private fun exactInt(json: JSONObject, key: String): Int {
            val value = json.opt(key)
            require(value is Int || value is Long) { "$key must be an integer" }
            val number = (value as Number).toLong()
            require(number in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) { "$key is out of range" }
            return number.toInt()
        }

        private fun attachApk(context: android.content.Context, uri: Uri, dir: File) {
            val temporary = File(dir, "result.pending")
            try {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "cannot read selected APK" }
                    temporary.outputStream().use { output ->
                        val buffer = ByteArray(65536)
                        var sinceStorageCheck = 1048576L
                        while (true) {
                            val n = input.read(buffer)
                            if (n < 0) break
                            sinceStorageCheck += n
                            if (sinceStorageCheck >= 1048576L) {
                                SafeZip.requireStorage(dir, 65536L)
                                sinceStorageCheck = 0
                            }
                            output.write(buffer, 0, n)
                        }
                    }
                }
                val report = JSONObject(File(dir, "report.json").readText())
                val hash = report.getJSONObject("application").getString("sha256")
                val expectedPackage = "dev.radek.converted.p" + hash.take(20)
                val expectedName = ArtifactNames.apkFileName(report)
                val selectedAbi = context.getSharedPreferences("radek_settings", android.content.Context.MODE_PRIVATE)
                    .getString("target_abi", "auto") ?: "auto"
                val sourceSlices = report.optJSONObject("machO")?.optJSONArray("slices") ?: error("source architecture report missing")
                val sourceArchitectures = (0 until sourceSlices.length()).map { sourceSlices.getJSONObject(it).optString("architecture") }.toSet()
                val expectedAbi = when (selectedAbi) {
                    "arm64-v8a" -> {
                        require("arm64" in sourceArchitectures) { "selected ARM64 ABI has no matching IPA slice" }
                        "arm64-v8a"
                    }
                    "armeabi-v7a" -> {
                        require(sourceArchitectures.any { it in setOf("armv7s", "armv7", "armv6") }) { "selected ARM32 ABI has no matching IPA slice" }
                        "armeabi-v7a"
                    }
                    else -> when {
                        "arm64" in sourceArchitectures -> "arm64-v8a"
                        sourceArchitectures.any { it in setOf("armv7s", "armv7", "armv6") } -> "armeabi-v7a"
                        else -> error("IPA has no supported ARM32 or ARM64 slice")
                    }
                }
                var targetAbi = ""
                var generatedBytes = 0
                var conversionBackend = ""
                var recompiledFunctions = 0
                var generatedApiReplacements = 0
                var notRecompiledFunctions = 0
                var recoveredIconHash = ""
                val sourceIcon = File(dir, "icon.png")
                if (sourceIcon.isFile) {
                    recoveredIconHash = java.security.MessageDigest.getInstance("SHA-256")
                        .digest(sourceIcon.readBytes()).joinToString("") { "%02x".format(it.toInt() and 255) }
                }
                ZipFile(temporary).use { zip ->
                    val zipEntries = zip.entries().asSequence().toList()
                    // Re-importing one of our own results: the caps here have to admit
                    // everything the packagers may legitimately emit, while still
                    // refusing a bomb that claims more than the storage it came from.
                    require(zipEntries.size <= ConvertedApkBuilder.MAX_RESOURCE_FILES + 4096) { "APK has too many entries" }
                    val expansionBudget = SafeZip.ExpansionBudget()
                    zipEntries.forEach { expansionBudget.admit(it.name, it.size, it.compressedSize) }
                    val names = zipEntries.map { it.name }
                    names.forEach { SafeZip.validateName(it) }
                    require(names.size == names.toSet().size) { "APK contains duplicate paths" }
                    require(names.none { it.lowercase(java.util.Locale.ROOT).endsWith(".ipa") }) { "APK must not contain the original IPA" }
                    val entry = zip.getEntry("assets/conversion.json") ?: error("conversion provenance missing")
                    require(entry.size in 1..4194304)
                    val metadata = JSONObject(zip.getInputStream(entry).bufferedReader().use { it.readText() })
                    require(metadata.getString("package") == expectedPackage && metadata.getJSONObject("source").getString("sha256") == hash) { "APK does not match this IPA" }
                    require(metadata.optString("contract") == "complete-game-v1") { "APK is not a complete-game conversion; restricted native-entry packages are rejected" }
                    require(metadata.optString("targetAbi") in setOf("arm64-v8a", "armeabi-v7a")) { "unsupported APK ABI" }
                    val conversion = metadata.optJSONObject("conversion") ?: error("conversion details missing")
                    targetAbi = conversion.optString("targetAbi")
                    require(targetAbi in setOf("arm64-v8a", "armeabi-v7a") && metadata.optString("targetAbi") == targetAbi) { "unsupported or inconsistent converted APK ABI" }
                    require(targetAbi == expectedAbi) { "APK ABI does not follow the selected/source architecture policy (automatic mode prefers ARM64 for FAT ARM64+ARM32 input)" }
                    generatedBytes = exactInt(conversion, "outputBytes")
                    require(generatedBytes in 1..(64 * 1024 * 1024)) { "invalid generated native-code size" }
                    conversionBackend = conversion.opt("backend") as? String ?: error("conversion backend is not a string")
                    require(conversionBackend.isNotBlank()) { "conversion backend missing" }
                    val game = metadata.optJSONObject("gameConversion") ?: error("complete-game evidence missing")
                    require(game.opt("status") == "COMPLETE" && game.opt("completeGameConversion") == true) { "host did not certify a complete game conversion" }
                    val reachableFunctions = exactInt(game, "reachableSourceFunctions")
                    recompiledFunctions = exactInt(game, "recompiledReachableFunctions")
                    notRecompiledFunctions = exactInt(game, "notRecompiledReachableFunctions")
                    require(reachableFunctions > 0 && recompiledFunctions == reachableFunctions && notRecompiledFunctions == 0) { "not all reachable game functions were statically recompiled" }
                    val reachableApis = exactInt(game, "reachableApiCount")
                    generatedApiReplacements = exactInt(game, "generatedApiReplacements")
                    val nativeApiPassthroughs = exactInt(game, "nativeApiPassthroughs")
                    require(game.opt("apiCoverageComplete") == true && exactInt(game, "unimplementedReachableApiCount") == 0) { "reachable iOS APIs are not fully implemented or replaced" }
                    require(reachableApis >= 0 && generatedApiReplacements >= 0 && nativeApiPassthroughs >= 0 && reachableApis == generatedApiReplacements + nativeApiPassthroughs) { "API replacement accounting is incomplete" }
                    val replacements = game.optJSONArray("apiReplacements") ?: error("API replacement evidence missing")
                    require(replacements.length() == generatedApiReplacements) { "generated API replacement count mismatch" }
                    for (index in 0 until replacements.length()) {
                        val replacement = replacements.getJSONObject(index)
                        require(replacement.opt("codeGenerated") == true && replacement.opt("linkedIntoApk") == true && replacement.opt("reachableFromEntry") == true) { "API mapping is only a candidate, not reachable generated code" }
                        val sourceSymbol = replacement.opt("sourceSymbol") as? String ?: error("API source symbol must be a string")
                        val targetAndroidApi = replacement.opt("targetAndroidApi") as? String ?: error("target Android API must be a string")
                        require(sourceSymbol.isNotBlank() && targetAndroidApi.isNotBlank()) { "API replacement mapping incomplete" }
                        val implementationHash = replacement.opt("implementationSha256") as? String ?: error("API implementation hash must be a string")
                        require(implementationHash.matches(Regex("[0-9a-f]{64}"))) { "API replacement implementation hash missing" }
                        val implementationArtifact = replacement.opt("implementationArtifact") as? String ?: error("API implementation artifact must be a string")
                        require(implementationArtifact.isNotBlank()) { "generated API implementation path missing" }
                        val safeImplementationArtifact = SafeZip.validateName(implementationArtifact)
                        require(safeImplementationArtifact == implementationArtifact && names.contains(safeImplementationArtifact)) { "generated API implementation is not packaged" }
                        val packagedImplementation = zip.getEntry(safeImplementationArtifact) ?: error("generated API implementation is missing")
                        val packagedHash = zip.getInputStream(packagedImplementation).use { sha256(it) }
                        require(packagedHash == implementationHash) { "generated API implementation hash mismatch" }
                    }
                    require(game.opt("resourcesComplete") == true) { "game resources were not completely packaged" }
                    require(game.opt("lifecycleImplemented") == true) { "Android application lifecycle is not implemented" }
                    val inventoryValue = metadata.opt("resourceInventory")
                    require(inventoryValue == null || inventoryValue is JSONArray) { "game resource inventory is invalid" }
                    val resourceInventory = inventoryValue as? JSONArray
                    for (index in 0 until (resourceInventory?.length() ?: 0)) {
                        val resource = resourceInventory!!.getJSONObject(index)
                        val resourcePath = resource.opt("path") as? String ?: error("resource path must be a string")
                        val safeResourcePath = SafeZip.validateName(resourcePath)
                        require(safeResourcePath == resourcePath) { "resource path is not canonical" }
                        val resourceHash = resource.opt("sha256") as? String ?: error("resource hash must be a string")
                        require(resourceHash.matches(Regex("[0-9a-f]{64}"))) { "resource hash is invalid" }
                        val packagedResourceName = "assets/bundle/$safeResourcePath"
                        val packagedResource = zip.getEntry(packagedResourceName) ?: error("game resource is not packaged: $packagedResourceName")
                        val packagedResourceHash = zip.getInputStream(packagedResource).use { sha256(it) }
                        require(packagedResourceHash == resourceHash) { "game resource integrity failure: $packagedResourceName" }
                    }
                    if (recoveredIconHash.isNotBlank()) {
                        require(game.optString("sourceIconSha256") == recoveredIconHash && game.optString("launcherIconSha256") == recoveredIconHash) { "recovered IPA launcher icon was not preserved" }
                    }
                    require(names.any { it == "lib/$targetAbi/libconverted.so" }) { "converted native library missing for $targetAbi" }
                    require(names.filter { it.startsWith("lib/") && it.endsWith(".so") }.all { it.startsWith("lib/$targetAbi/") }) { "APK contains a native library for a different ABI" }
                    val appleExecutableMagics = setOf("cffaedfe", "cefaedfe", "feedface", "feedfacf", "cafebabe", "cafebabf", "bebafeca", "bfbafeca")
                    for (name in names) {
                        val archiveEntry = zip.getEntry(name) ?: continue
                        zip.getInputStream(archiveEntry).use { input ->
                            val digest = java.security.MessageDigest.getInstance("SHA-256")
                            val head = ByteArray(4)
                            var count = 0
                            while (count < head.size) {
                                val read = input.read(head, count, head.size - count)
                                if (read < 0) break
                                if (read == 0) {
                                    val next = input.read()
                                    if (next < 0) break
                                    head[count++] = next.toByte()
                                } else {
                                    count += read
                                }
                            }
                            val magic = if (count == head.size) head.joinToString("") { "%02x".format(it.toInt() and 255) } else ""
                            require(magic !in appleExecutableMagics) { "Apple executable leaked into APK" }
                            digest.update(head, 0, count)
                            val buffer = ByteArray(65536)
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                if (read == 0) {
                                    val next = input.read()
                                    if (next < 0) break
                                    digest.update(next.toByte())
                                } else {
                                    digest.update(buffer, 0, read)
                                }
                            }
                            val contentHash = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                            require(contentHash != hash) { "APK embeds the original IPA content" }
                        }
                    }
                }
                @Suppress("DEPRECATION")
                val pkg = context.packageManager.getPackageArchiveInfo(temporary.path, android.content.pm.PackageManager.GET_ACTIVITIES or android.content.pm.PackageManager.GET_SIGNATURES)
                    ?: error("Android cannot parse APK")
                require(pkg.packageName == expectedPackage && pkg.activities.orEmpty().any { it.name == "dev.radek.generated.MainActivity" }) { "Android package/entry mismatch" }
                @Suppress("DEPRECATION")
                require(!pkg.signatures.isNullOrEmpty()) { "APK signer missing" }
                val result = File(dir, expectedName)
                if (result.exists()) require(result.delete()) { "cannot replace existing host APK" }
                require(temporary.renameTo(result))
                report.put("hostConversion", JSONObject()
                    .put("status", "ATTACHED")
                    .put("completeGameConversion", true)
                    .put("artifact", result.name)
                    .put("package", expectedPackage)
                    .put("targetAbi", targetAbi)
                    .put("nativeCodeGenerated", true)
                    .put("nativeCodeBytes", generatedBytes)
                    .put("recompiledReachableFunctions", recompiledFunctions)
                    .put("generatedApiReplacements", generatedApiReplacements)
                    .put("notRecompiledReachableFunctions", notRecompiledFunctions)
                    .put("sourceIconSha256", recoveredIconHash)
                    .put("backend", conversionBackend)
                    .put("contract", "complete-game-v1")
                    .put("runtimeExecution", "NOT_TESTED")
                    .put("gamePlayability", "NOT_TESTED")
                    .put("installableAndroidPackage", true)
                    .put("completedAt", java.time.Instant.now().toString()))
                report.put("portProgress", JSONObject()
                    .put("percent", 100)
                    .put("status", "COMPLETE_CONVERSION_ATTACHED")
                    .put("basis", "Host metadata reports all reachable code and required APIs statically recompiled; runtime execution and gameplay are not tested."))
                report.put("conversionProgress", JSONObject()
                    .put("percent", 100)
                    .put("stage", "HOST_APK_VALIDATED")
                    .put("status", "ATTACHED")
                    .put("message", "A complete-game host APK passed static attachment checks; device execution is not tested."))
                Library(context).save(dir, report)
            } finally { temporary.delete() }
        }
    }
}
