package dev.radek.conventor

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class ArtifactNamesTest {
    @Test fun exactBundleIdsBecomeAndroidPackagesAndInvalidIdsUseSafeFallback() {
        val hash = "a".repeat(64)
        val signer = "b".repeat(64)
        val exact = "com.lego.riseofthesnakes"
        assertEquals(exact, ArtifactNames.androidPackageName(exact, hash, signer))
        assertEquals("main.1.$exact.obb", ArtifactNames.obbFileName(exact, 1))
        for (invalid in listOf("", "com..game", "com.game-name", "com.1game", "com.gáme", "single", "com.game/../x")) {
            val fallback = ArtifactNames.androidPackageName(invalid, hash, signer)
            assertEquals("dev.radek.gameruntime.p${hash.take(20)}${signer.take(8)}", fallback)
        }
        // Existing native APKs retain the old sanitizing result for validation.
        assertEquals("com.game.name", ArtifactNames.legacyAndroidPackageName("com.game-name", hash, signer))
    }

    @Test fun outputNameUsesSanitizedPickedIpaBasename() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game.apk", ArtifactNames.apkFileName(report))
    }

    @Test fun fallsBackSafelyWhenOriginalNameIsMissing() {
        val report = JSONObject().put("application", JSONObject().put("name", "bad:game"))
        assertEquals("bad_game.apk", ArtifactNames.apkFileName(report))
    }

    @Test fun previewArtifactHasAnExplicitlySeparateFilenameWithoutPlaceholderWording() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game-preview.apk", ArtifactNames.placeholderApkFileName(report))
        assertNotEquals(ArtifactNames.apkFileName(report), ArtifactNames.placeholderApkFileName(report))
        for (name in listOf(ArtifactNames.apkFileName(report), ArtifactNames.placeholderApkFileName(report))) {
            org.junit.Assert.assertFalse(
                "user-visible artifact names must not say placeholder: $name",
                name.lowercase().contains("placeholder"),
            )
        }
    }

    @Test fun gameRuntimeArtifactHasAnExplicitlySeparateFilename() {
        val report = JSONObject().put("source", JSONObject().put("originalName", "../My Game.ipa"))
        assertEquals("My Game-game.apk", ArtifactNames.gameApkFileName(report))
        assertNotEquals(ArtifactNames.apkFileName(report), ArtifactNames.gameApkFileName(report))
        assertNotEquals(ArtifactNames.placeholderApkFileName(report), ArtifactNames.gameApkFileName(report))
    }
}
