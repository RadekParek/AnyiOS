"""Fast checks for standalone launch policy and the shared display defaults.

These inspect integration wiring; real game rendering still needs a device.
"""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "converter/anyiose-template/src/main"
ENGINE = ROOT / "third_party/anyiose-engine"
ANDROID = "{http://schemas.android.com/apk/res/android}"


def read(path):
    return path.read_text(encoding="utf-8")


class RuntimeContractTests(unittest.TestCase):
    def test_previous_reviewed_engine_patch_can_be_upgraded(self):
        script = read(ROOT / "scripts/prepare-anyiose-runtime.sh")
        self.assertIn(
            "f5d7d9c61b5115864c3f0c27a4be3e4ff3338506b3cae65f896736c36bd3b814",
            script,
        )

    def test_standalone_launcher_is_transparent_and_extracts_off_thread(self):
        manifest = ET.parse(TEMPLATE / "AndroidManifest.xml").getroot()
        application = manifest.find("application")
        self.assertEqual(
            application.get(ANDROID + "name"), "org.podnative.StandaloneApplication"
        )
        launcher = next(
            item
            for item in application.findall("activity")
            if item.get(ANDROID + "name") == "org.podnative.StandaloneLauncherActivity"
        )
        self.assertEqual(
            launcher.get(ANDROID + "theme"),
            "@android:style/Theme.Translucent.NoTitleBar",
        )
        source = read(TEMPLATE / "java/org/podnative/StandaloneLauncherActivity.java")
        self.assertNotIn("setContentView", source)
        self.assertNotIn("Preparing your game", source)
        self.assertNotIn("Could not prepare the game:", source)
        self.assertIn("new Thread(", source)
        self.assertIn("synchronized (SETUP_LOCK)", source)
        storage = read(TEMPLATE / "java/org/podnative/StandaloneGameStorage.java")
        self.assertIn('MessageDigest.getInstance("SHA-256")', storage)
        self.assertIn("Game payload failed its integrity check", storage)
        self.assertIn("pending.delete()", storage)
        self.assertIn('return "main.1." + packageName + ".obb"', storage)
        self.assertIn('new File(privateDirectory, "game.ipa")', storage)
        self.assertIn("StandaloneGameStorage.prepare", source)

    def test_expansion_versions_match_templates_and_native_assets_stay_embedded(self):
        names = read(
            ROOT / "converter/app/src/main/java/dev/radek/conventor/ArtifactNames.kt"
        )
        for constant, template in (
            ("ANYIOSE_VERSION_CODE", "anyiose-template"),
            ("GAME_RUNTIME_VERSION_CODE", "gameruntime-template"),
            ("CONVERTED_VERSION_CODE", "converted-template"),
        ):
            self.assertIn(f"const val {constant} = 1", names)
            self.assertIn(
                "versionCode = 1", read(ROOT / f"converter/{template}/build.gradle.kts")
            )
        builder = read(
            ROOT
            / "converter/app/src/main/java/dev/radek/conventor/GameRuntimeApkBuilder.kt"
        )
        self.assertNotIn("OBB_ONLY_PAYLOAD_BYTES", builder)
        self.assertIn('Entry.stream("assets/bundle/$relative", payload', builder)
        converted = read(
            ROOT
            / "converter/app/src/main/java/dev/radek/conventor/ConvertedApkBuilder.kt"
        )
        for source in (builder, converted):
            self.assertLess(
                source.index("finalized = true"),
                source.index("OptionalExpansionPublisher.publish("),
            )
            self.assertIn('conversion.remove("expansionObb")', source)

    def test_scale_up_transition_is_only_on_emulator_launch_and_respects_reduced_motion(
        self,
    ):
        emulator = read(ROOT / "app/src/main/java/org/podnative/MainActivity.java")
        launch = emulator.split("private void launchAnyiOSEMU(", 1)[1].split(
            "private void showPreparationError(", 1
        )[0]
        self.assertIn("ValueAnimator.areAnimatorsEnabled()", launch)
        self.assertIn("ActivityOptions.makeScaleUpAnimation", launch)
        self.assertIn("anchor.isAttachedToWindow()", launch)
        self.assertIn("overridePendingTransition(0, 0)", launch)
        for name in ("StandaloneLauncherActivity.java", "StandaloneGameActivity.java"):
            self.assertNotIn(
                "makeScaleUpAnimation", read(TEMPLATE / "java/org/podnative" / name)
            )

    def test_scale_hack_is_wired_to_context_queries_and_multisample_targets(self):
        options = read(TEMPLATE / "java/org/podnative/StandaloneOptions.java")
        self.assertIn('"--screen-size=480x320", "--scale-hack=2"', options)
        self.assertIn('"--screen-size=1024x768", "--scale-hack=2"', options)
        self.assertNotIn('"--scale-hack=4"', options)
        path = ENGINE / "src/frameworks/opengles/gles_guest.rs"
        if not path.is_file():
            self.skipTest("pinned engine is not initialized in the host-only suite")
        source = read(path)
        eagl = read(ENGINE / "src/frameworks/opengles/eagl.rs")
        self.assertIn("!env.options.explicit_screen_size", eagl)
        self.assertIn(
            "drawable_storage_size(width, height, contents_scale, scale_hack)", eagl
        )
        self.assertIn(
            "drawable_storage_size(1024.0, 768.0, 1.0, env.options.scale_hack.get())",
            eagl,
        )
        for name in ("glGetIntegerv", "glGetFloatv", "glGetFixedv"):
            body = source.split(f"fn {name}(", 1)[1].split("\nfn ", 1)[0]
            self.assertIn("query_divisor", body)
        blit = source.split("fn glBlitFramebuffer(", 1)[1].split("\nfn ", 1)[0]
        self.assertIn("scaled_read", blit)
        self.assertIn(".scaled((dst_x0, dst_y0, dst_x1, dst_y1), factor)", blit)
        msaa = source.split("fn glRenderbufferStorageMultisample(", 1)[1].split(
            "\nfn ", 1
        )[0]
        self.assertIn("width.saturating_mul(factor)", msaa)
        layered = source.split("fn glFramebufferTextureLayer(", 1)[1].split("\nfn ", 1)[
            0
        ]
        self.assertIn("scale_framebuffer.attach_texture", layered)

    def test_only_standalone_uses_quiet_error_policy(self):
        options = read(TEMPLATE / "java/org/podnative/StandaloneOptions.java")
        activity = read(TEMPLATE / "java/org/podnative/StandaloneGameActivity.java")
        application = read(TEMPLATE / "java/org/podnative/StandaloneApplication.java")
        self.assertIn('"--boost-display-link"', options)
        self.assertIn('"--no-error-popup"', options)
        self.assertIn('"--anyiose-silent-fatal"', options)
        self.assertIn("shouldShowStartupErrors()", activity)
        self.assertIn("setDefaultUncaughtExceptionHandler", application)
        # Guest dialogs are separate from SDL startup and emulator error dialogs.
        self.assertNotIn("messageboxShowMessageBox", activity)

    def test_patched_engine_honors_standalone_quiet_policy(self):
        sdl_path = ENGINE / "android/app/src/main/java/org/libsdl/app/SDLActivity.java"
        if not sdl_path.is_file():
            self.skipTest("pinned engine is not initialized in the host-only suite")
        sdl = read(sdl_path)
        native = read(ENGINE / "src/lib.rs")
        handler = read(ENGINE / "src/crash_handler.rs")
        self.assertIn("protected boolean shouldShowStartupErrors()", sdl)
        self.assertIn('argument == "--anyiose-silent-fatal"', native)
        self.assertIn("SILENT_ANDROID_FATAL", handler)
        self.assertIn("libc::_exit(128 + sig)", handler)

    def test_overlay_jni_receives_the_drawable_used_for_mapping(self):
        java_path = (
            ENGINE / "android/app/src/main/java/org/touchhle/android/MainActivity.java"
        )
        if not java_path.is_file():
            self.skipTest("pinned engine is not initialized in the host-only suite")
        java = read(java_path)
        native = read(ENGINE / "src/android_web_view.rs")
        window = read(ENGINE / "src/window.rs")
        self.assertIn('"showWebOverlay", "(Ljava/lang/String;IIIIII)I"', native)
        self.assertIn('"setWebOverlayBounds", "(IIIIIII)V"', native)
        self.assertIn("final int drawableWidth, final int drawableHeight", java)
        self.assertIn(
            "overlayGeometry(x, y, w, h, drawableWidth, drawableHeight)", java
        )
        self.assertIn("self.viewport_for_drawable(drawable_size)", window)
        self.assertNotIn("mSurface.getHolder().getSurfaceFrame()", java)

    def test_resolution_only_skips_cpu_and_memory_hud_sampling(self):
        activity = read(ROOT / "app/src/main/java/org/podnative/AnyiOSEmuActivity.java")
        update = activity.split("private void updatePerformanceHud()", 1)[1].split(
            "protected void onResume()", 1
        )[0]
        self.assertIn("fpsReader.poll()", update)
        self.assertLess(
            update.index("if (performanceHud == null)"),
            update.index("Debug.getMemoryInfo"),
        )
        self.assertLess(
            update.index("if (performanceHud == null)"),
            update.index("Process.getElapsedCpuTime"),
        )

    def test_main_emulator_defaults_and_manual_display_geometry(self):
        activity = read(ROOT / "app/src/main/java/org/podnative/AnyiOSEmuActivity.java")
        profile = read(ROOT / "app/src/main/java/org/podnative/GameDisplayProfile.java")
        settings = read(ROOT / "app/src/main/java/org/podnative/MainActivity.java")
        resolution = read(
            ROOT / "app/src/main/java/org/podnative/DisplayResolution.java"
        )
        self.assertIn("settings.getInt(DisplayResolution.PREFERENCE, 0)", activity)
        self.assertIn('settingRow(display, "Resolution"', settings)
        self.assertIn("DisplayResolution.label(", settings)
        self.assertIn('" · Native"', resolution)
        self.assertNotIn('settingRow(display, "Game layout"', settings)
        self.assertIn("GameDisplayProfile.argument(GameDisplayProfile.AUTO)", activity)
        self.assertIn('settings.getInt("dynamicResolution", 100)', activity)
        self.assertIn("surfaceShortEdge > 0 || dynamicMinimum < 100", activity)
        self.assertIn("new SurfaceResolution(mSurface, surfaceShortEdge)", activity)
        self.assertNotIn('"--screen-size=', activity)
        self.assertIn('return "--device-family=auto"', profile)
        self.assertIn('"Match device Hz"', settings)
        self.assertIn('prefs.getBoolean("boostDisplayLink", true)', settings)
        self.assertIn('settings.getBoolean("boostDisplayLink", true)', activity)
        self.assertIn("Some games may run faster", settings)
        self.assertIn("higher FPS is not guaranteed", settings)
        # Even an old persisted showConsole=true or a stale launch extra cannot
        # create a badge: neither preference nor overlay is consulted anymore.
        self.assertNotIn("showConsole", activity)
        self.assertNotIn("showSessionBadge", activity)
        self.assertNotIn("EXTRA_SHOW_CONSOLE", activity)
        self.assertNotIn("Session badge", settings)
        self.assertNotIn('getBoolean("showConsole"', settings)
        self.assertNotIn('putBoolean("showConsole"', settings)
        self.assertIn('"--anyiose-quiet-game-info"', activity)
        standalone = read(TEMPLATE / "java/org/podnative/StandaloneOptions.java")
        self.assertNotIn('"--anyiose-quiet-game-info"', standalone)

    def test_game_details_use_declared_requirements_and_real_log(self):
        activity = read(ROOT / "app/src/main/java/org/podnative/MainActivity.java")
        library = read(ROOT / "app/src/main/java/org/podnative/GameLibrary.java")
        self.assertIn('"Launch with AnyiOS"', activity)
        self.assertIn('"Show latest log"', activity)
        self.assertIn("library.loadRequirements(game", activity)
        self.assertIn("library.loadLatestLog(game", activity)
        self.assertIn("LatestGameLog.copy(", activity)
        self.assertIn("LatestGameLog.preview(", library)
        self.assertIn("LatestGameLog.prepare(", library)

    def test_network_access_and_route_monitor_are_shared_by_both_hosts(self):
        activity = read(ROOT / "app/src/main/java/org/podnative/AnyiOSEmuActivity.java")
        options = read(TEMPLATE / "java/org/podnative/StandaloneOptions.java")
        self.assertIn('"--allow-network-access"', activity)
        self.assertIn('"--allow-network-access"', options)
        for manifest_path in [
            ROOT / "app/src/main/AndroidManifest.xml",
            TEMPLATE / "AndroidManifest.xml",
        ]:
            manifest = ET.parse(manifest_path).getroot()
            permissions = {
                item.get(ANDROID + "name")
                for item in manifest.findall("uses-permission")
            }
            self.assertIn("android.permission.INTERNET", permissions)
            self.assertIn("android.permission.ACCESS_NETWORK_STATE", permissions)
        path = (
            ENGINE
            / "android/app/src/main/java/org/touchhle/android/NetworkRouteMonitor.java"
        )
        if not path.is_file():
            self.skipTest("pinned engine is not initialized in the host-only suite")
        java = read(path)
        host = read(
            ENGINE / "android/app/src/main/java/org/touchhle/android/MainActivity.java"
        )
        native = read(ENGINE / "src/android_network.rs")
        reachability = read(
            ENGINE / "src/frameworks/system_configuration/sc_network_reachability.rs"
        )
        self.assertIn("registerDefaultNetworkCallback", java)
        self.assertIn("onBlockedStatusChanged", java)
        self.assertIn("networkRouteMonitor.start()", host)
        self.assertIn("networkRouteMonitor.stop()", host)
        self.assertIn(
            "Java_org_anyiose_android_NetworkRouteMonitor_nativeSetNetworkRoute", native
        )
        self.assertIn("crate::android_network::route()", reachability)
        self.assertIn("_anyiose_pollReachability", reachability)
        self.assertIn("timer invalidate", reachability)

    def test_real_magnetometer_and_guest_logging_are_wired(self):
        path = ENGINE / "src/frameworks/core_motion.rs"
        if not path.is_file():
            self.skipTest("pinned engine is not initialized in the host-only suite")
        core_motion = read(path)
        window = read(ENGINE / "src/window.rs")
        self.assertIn("window.has_magnetometer()", core_motion)
        self.assertIn("window.start_magnetometer()", core_motion)
        self.assertIn("window.stop_magnetometer()", core_motion)
        self.assertIn("w.get_magnetic_field()", core_motion)
        self.assertIn("on_parent_stack_in_coroutine", core_motion)
        self.assertIn("SDL_SensorGetDataWithTimestamp", window)
        self.assertIn("magnetometer_clients", window)
        for path in [
            "src/libc/posix_io.rs",
            "src/libc/stdio.rs",
            "src/libc/stdio/printf.rs",
        ]:
            self.assertIn("write_guest_output", read(ENGINE / path))


if __name__ == "__main__":
    unittest.main()
