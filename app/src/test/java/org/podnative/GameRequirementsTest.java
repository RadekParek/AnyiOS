package org.podnative;

import static org.junit.Assert.*;

import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.Test;

public class GameRequirementsTest {
    @Test
    public void readsBothDeviceFamiliesAndRequiredSensors() throws Exception {
        String xml = "<plist><dict>"
            + "<key>MinimumOSVersion</key><string>5.0</string>"
            + "<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>"
            + "<key>UIRequiredDeviceCapabilities</key><array><string>gyroscope</string>"
            + "<string>accelerometer</string><string>magnetometer</string></array></dict></plist>";
        GameRequirements details = GameRequirements.fromPlist(
            Plist.read(xml.getBytes(StandardCharsets.UTF_8)));
        assertEquals("5.0", details.minimumIos);
        assertEquals("iPhone, iPod touch, iPad", details.deviceFamilies);
        assertEquals("Gyroscope, Accelerometer, Magnetometer", details.required);
        assertEquals("", details.prohibited);
    }

    @Test
    public void falseRequirementsAreProhibitedNotRequired() {
        Map<String, Object> features = new LinkedHashMap<>();
        features.put("accelerometer", true);
        features.put("telephony", false);
        features.put("gyroscope", false);
        GameRequirements details = GameRequirements.fromPlist(
            Map.of("UIDeviceFamily", 2L, "UIRequiredDeviceCapabilities", features));
        assertEquals("Not specified", details.minimumIos);
        assertEquals("iPad", details.deviceFamilies);
        assertEquals("Accelerometer", details.required);
        assertEquals("Telephony, Gyroscope", details.prohibited);
        assertTrue(details.description().contains("Must not be present: Telephony, Gyroscope"));
    }

    @Test
    public void missingRequirementsDoNotClaimThatOptionalSensorsAreUnused() {
        GameRequirements details = GameRequirements.fromPlist(Map.of());
        assertEquals("iPhone, iPod touch (iOS default)", details.deviceFamilies);
        assertEquals("None declared", details.required);
        assertTrue(details.description().contains("optional features may not be listed"));
        assertEquals("Unavailable", GameRequirements.unavailable().minimumIos);
    }

    @Test
    public void malformedDeclarationsAreNotPresentedAsNoRequirements() {
        GameRequirements bad = GameRequirements.fromPlist(Map.of("UIRequiredDeviceCapabilities", "gyro"));
        assertEquals("Unavailable (invalid declaration)", bad.required);
        GameRequirements partial = GameRequirements.fromPlist(Map.of("UIRequiredDeviceCapabilities",
            Map.of("gyroscope", true, "accelerometer", "yes")));
        assertTrue(partial.required.startsWith("Gyroscope"));
        assertTrue(partial.required.contains("could not be shown"));
    }

    @Test
    public void sanitizesUnknownCapabilitiesAndBoundsTheList() {
        GameRequirements details = GameRequirements.fromPlist(Map.of(
            "UIDeviceFamily", List.of(1L, 1L, 2L, 9L),
            "UIRequiredDeviceCapabilities", List.of("foo-bar", "microphone", "foo-bar")));
        assertEquals("iPhone, iPod touch, iPad, Device family 9", details.deviceFamilies);
        assertEquals("foo bar, Microphone", details.required);
    }
}
