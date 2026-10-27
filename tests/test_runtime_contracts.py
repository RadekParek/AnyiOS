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
        self.assertIn('MessageDigest.getInstance("SHA-256")', source)
        self.assertIn("Game payload failed its integrity check", source)
        self.assertIn("pending.delete()", source)

    def test_only_standalone_uses_quiet_error_policy(self):
        options = read(TEMPLATE / "java/org/podnative/StandaloneOptions.java")
        activity = read(TEMPLATE / "java/org/podnative/StandaloneGameActivity.java")
        application = read(TEMPLATE / "java/org/podnative/StandaloneApplication.java")
        sdl = read(ENGINE / "android/app/src/main/java/org/libsdl/app/SDLActivity.java")
        native = read(ENGINE / "src/lib.rs")
        handler = read(ENGINE / "src/crash_handler.rs")
        self.assertIn('"--boost-display-link"', options)
        self.assertIn('"--no-error-popup"', options)
        self.assertIn('"--anyiose-silent-fatal"', options)
        self.assertIn("shouldShowStartupErrors()", activity)
        self.assertIn("protected boolean shouldShowStartupErrors()", sdl)
        self.assertIn("setDefaultUncaughtExceptionHandler", application)
        self.assertIn('argument == "--anyiose-silent-fatal"', native)
        self.assertIn("SILENT_ANDROID_FATAL", handler)
        self.assertIn("libc::_exit(128 + sig)", handler)
        # Guest dialogs are separate from SDL startup and emulator error dialogs.
        self.assertNotIn("messageboxShowMessageBox", activity)

    def test_main_emulator_defaults_match_standalone_display_geometry(self):
        activity = read(ROOT / "app/src/main/java/org/podnative/AnyiOSEmuActivity.java")
        profile = read(ROOT / "app/src/main/java/org/podnative/GameDisplayProfile.java")
        settings = read(ROOT / "app/src/main/java/org/podnative/MainActivity.java")
        # An older "resolution" preference used to affect a different graphics
        # test. Never let that saved value silently force the new SDL buffer.
        self.assertIn('settings.getInt("presentationResolution", 0)', activity)
        self.assertNotIn('settings.getInt("resolution",', activity)
        self.assertIn('settings.getInt("dynamicResolution", 100)', activity)
        self.assertIn("resolutionLimit > 0 || dynamicMinimum < 100", activity)
        self.assertNotIn('"--screen-size=', activity)
        self.assertIn('return "--device-family=auto"', profile)
        self.assertIn('"Maximize FPS"', settings)
        self.assertIn("Some games may run faster", settings)


if __name__ == "__main__":
    unittest.main()
