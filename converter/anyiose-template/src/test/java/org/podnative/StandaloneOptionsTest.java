package org.podnative;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import java.util.Arrays;
import java.util.List;
import org.junit.Test;

public class StandaloneOptionsTest {
    @Test public void everyGameRequestsMaximumFpsAndQuietErrorsWithoutChangingGuestSize() {
        List<String> arguments = Arrays.asList(StandaloneOptions.forGame("/private/data", "/private/game.ipa", 120));
        assertTrue(arguments.contains("--anyiose-data-dir=/private/data"));
        assertTrue(arguments.contains("/private/game.ipa"));
        assertTrue(arguments.contains("--allow-network-access"));
        assertTrue(arguments.contains("--device-family=auto"));
        assertTrue(arguments.contains("--fps-limit=120.000"));
        assertTrue(arguments.contains("--boost-display-link"));
        assertTrue(arguments.contains("--no-error-popup"));
        assertTrue(arguments.contains("--anyiose-silent-fatal"));
        assertFalse(arguments.stream().anyMatch(argument -> argument.startsWith("--screen-size=")));
    }

    @Test public void optInSettingsReachTheGuestWithoutChangingOtherDefaults() {
        List<String> arguments = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 90,
                true, false, true, true, "-90"));
        assertTrue(arguments.contains("--screen-size=480x320"));
        assertTrue(arguments.contains("--scale-hack=2"));
        assertTrue(arguments.contains("--no-network-access"));
        assertFalse(arguments.contains("--allow-network-access"));
        assertTrue(arguments.contains("--perf-hints"));
        assertTrue(arguments.contains("--affinity=big"));
        assertTrue(arguments.contains("--landscape-left"));
        assertFalse(arguments.contains("--anyiose-initial-portrait"));
    }

    @Test public void legacySingleProfileCallRetainsItsOptions() {
        List<String> arguments = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                true, true, false, "-90"));
        assertTrue(arguments.contains("--screen-size=480x320"));
        assertTrue(arguments.contains("--scale-hack=2"));
        assertTrue(arguments.contains("--no-network-access"));
        assertTrue(arguments.contains("--landscape-left"));
        assertFalse(arguments.contains("--screen-size=1024x768"));
    }

    @Test public void tabletProfileUsesExactlyOneTwoTimesScaleAndPreservesOtherOptions() {
        List<String> arguments = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, true, false, false, "default"));
        assertTrue(arguments.contains("--screen-size=1024x768"));
        assertTrue(arguments.contains("--scale-hack=2"));
        assertFalse(arguments.contains("--screen-size=480x320"));
        assertTrue(arguments.contains("--allow-network-access"));
        assertFalse(arguments.contains("--perf-hints"));
        assertTrue(arguments.stream().filter(x -> x.startsWith("--scale-hack=")).count() == 1);
    }

    @Test public void conflictingProfilesCannotEmitTwoSizeOrScaleArguments() {
        List<String> arguments = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                true, true, false, false, "default"));
        assertTrue(arguments.contains("--screen-size=480x320"));
        assertFalse(arguments.contains("--screen-size=1024x768"));
        assertTrue(arguments.stream().filter(x -> x.startsWith("--scale-hack=")).count() == 1);
    }

    @Test public void portraitAndUnknownRotationDoNotForceLandscape() {
        List<String> portrait = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, false, false, false, "portrait"));
        assertTrue(portrait.contains("--anyiose-initial-portrait"));
        assertFalse(portrait.contains("--landscape-left"));
        assertFalse(portrait.contains("--screen-size=480x320"));
        List<String> unknown = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, false, false, false, "invalid"));
        assertFalse(unknown.contains("--landscape-left"));
        assertFalse(unknown.contains("--anyiose-initial-portrait"));
    }

    @Test public void degreeChoicesSelectCoherentGuestOrientation() {
        List<String> minus = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, false, false, false, "-90"));
        List<String> plus = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, false, false, false, "90"));
        List<String> half = Arrays.asList(StandaloneOptions.forGame("/data", "/game", 60,
                false, false, false, false, "180"));
        assertTrue(minus.contains("--landscape-left"));
        assertTrue(plus.contains("--landscape-right"));
        assertTrue(half.contains("--upside-down"));
        assertFalse(plus.contains("--present-rotate=90")); // Do not rotate pixels independently of touches.
    }

    @Test public void missingPayloadAlsoUsesTheQuietHostPolicy() {
        List<String> arguments = Arrays.asList(StandaloneOptions.noGame());
        assertTrue(arguments.contains("--help"));
        assertTrue(arguments.contains("--no-error-popup"));
        assertTrue(arguments.contains("--anyiose-silent-fatal"));
    }
}
