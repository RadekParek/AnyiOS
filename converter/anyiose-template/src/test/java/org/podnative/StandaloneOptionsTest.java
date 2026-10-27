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
        assertTrue(arguments.contains("--device-family=auto"));
        assertTrue(arguments.contains("--fps-limit=120.000"));
        assertTrue(arguments.contains("--boost-display-link"));
        assertTrue(arguments.contains("--no-error-popup"));
        assertTrue(arguments.contains("--anyiose-silent-fatal"));
        assertFalse(arguments.stream().anyMatch(argument -> argument.startsWith("--screen-size=")));
    }

    @Test public void missingPayloadAlsoUsesTheQuietHostPolicy() {
        List<String> arguments = Arrays.asList(StandaloneOptions.noGame());
        assertTrue(arguments.contains("--help"));
        assertTrue(arguments.contains("--no-error-popup"));
        assertTrue(arguments.contains("--anyiose-silent-fatal"));
    }
}
