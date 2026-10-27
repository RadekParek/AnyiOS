package org.podnative;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/** The app's declared iOS requirements, not a prediction of emulator compatibility. */
final class GameRequirements {
    final String minimumIos, deviceFamilies, required, prohibited, gameEngine;

    private GameRequirements(
        String minimumIos, String deviceFamilies, String required, String prohibited) {
        this(minimumIos, deviceFamilies, required, prohibited, "Unknown");
    }

    private GameRequirements(String minimumIos, String deviceFamilies, String required,
        String prohibited, String gameEngine) {
        this.gameEngine = gameEngine;
        this.minimumIos = minimumIos;
        this.deviceFamilies = deviceFamilies;
        this.required = required;
        this.prohibited = prohibited;
    }

    static GameRequirements unavailable() {
        return new GameRequirements("Unavailable", "Unavailable", "Unavailable", "");
    }

    GameRequirements withEngine(String engine) {
        return new GameRequirements(minimumIos, deviceFamilies, required, prohibited, engine);
    }

    // Apple's archived Info.plist reference documents UIDeviceFamily (including its
    // iPhone/iPod default) and required vs. prohibited capability dictionaries:
    // https://developer.apple.com/library/archive/documentation/General/Reference/InfoPlistKeyReference/Articles/iPhoneOSKeys.html
    static GameRequirements fromPlist(Map<String, Object> info) {
        String minimum = LibraryRules.metadataText(info.get("MinimumOSVersion"), "Not specified");
        Object family = info.get("UIDeviceFamily");
        Set<String> families = new LinkedHashSet<>();
        if (family == null)
            families.add("iPhone, iPod touch (iOS default)");
        else if (family instanceof List<?>)
            for (Object item : (List<?>) family) addFamily(families, item);
        else
            addFamily(families, family);
        String deviceFamilies = families.isEmpty() ? "Not specified" : String.join(", ", families);

        Object capabilities = info.get("UIRequiredDeviceCapabilities");
        Set<String> required = new LinkedHashSet<>(), prohibited = new LinkedHashSet<>();
        boolean incomplete = false;
        if (capabilities instanceof List<?>) {
            for (Object item : (List<?>) capabilities) {
                if (!(item instanceof String) || ((String) item).trim().isEmpty())
                    incomplete = true;
                else if (required.size() >= 32)
                    incomplete = true;
                else
                    addCapability(required, item);
            }
        } else if (capabilities instanceof Map<?, ?>) {
            for (Map.Entry<?, ?> entry : ((Map<?, ?>) capabilities).entrySet()) {
                if (!(entry.getKey() instanceof String)
                    || ((String) entry.getKey()).trim().isEmpty())
                    incomplete = true;
                else if (Boolean.TRUE.equals(entry.getValue()) && required.size() < 32)
                    addCapability(required, entry.getKey());
                else if (Boolean.FALSE.equals(entry.getValue()) && prohibited.size() < 32)
                    addCapability(prohibited, entry.getKey());
                else
                    incomplete = true;
            }
        } else if (capabilities != null) {
            incomplete = true;
        }
        String requirements = required.isEmpty() ? "None declared" : String.join(", ", required);
        if (incomplete)
            requirements = required.isEmpty()
                ? "Unavailable (invalid declaration)"
                : requirements + " (some declarations could not be shown)";
        return new GameRequirements(
            minimum, deviceFamilies, requirements, String.join(", ", prohibited));
    }

    private static void addFamily(Set<String> families, Object value) {
        if (!(value instanceof Number) || families.size() >= 16)
            return;
        long family = ((Number) value).longValue();
        if (family == 1)
            families.add("iPhone, iPod touch");
        else if (family == 2)
            families.add("iPad");
        else
            families.add("Device family " + family);
    }

    private static void addCapability(Set<String> capabilities, Object value) {
        String key = LibraryRules.metadataText(value, "").toLowerCase(Locale.ROOT);
        if (key.isEmpty() || capabilities.size() >= 32)
            return;
        String label;
        switch (key) {
            case "accelerometer":
                label = "Accelerometer";
                break;
            case "gyroscope":
                label = "Gyroscope";
                break;
            case "magnetometer":
                label = "Magnetometer";
                break;
            case "gps":
                label = "GPS";
                break;
            case "location-services":
                label = "Location services";
                break;
            case "still-camera":
                label = "Camera";
                break;
            case "front-facing-camera":
                label = "Front camera";
                break;
            case "video-camera":
                label = "Video camera";
                break;
            case "camera-flash":
                label = "Camera flash";
                break;
            case "microphone":
                label = "Microphone";
                break;
            case "telephony":
                label = "Telephony";
                break;
            case "wifi":
                label = "Wi-Fi";
                break;
            case "bluetooth-le":
                label = "Bluetooth LE";
                break;
            case "armv6":
                label = "ARMv6";
                break;
            case "armv7":
                label = "ARMv7";
                break;
            case "arm64":
                label = "ARM64";
                break;
            case "opengles-1":
                label = "OpenGL ES 1";
                break;
            case "opengles-2":
                label = "OpenGL ES 2";
                break;
            default:
                label = key.replace('-', ' ');
                break;
        }
        capabilities.add(label);
    }

    String description() {
        List<String> lines = new ArrayList<>();
        lines.add("Game engine: " + gameEngine);
        lines.add("Runtime: AnyiOSEMU");
        lines.add("Minimum iOS version: " + minimumIos);
        lines.add("Designed for: " + deviceFamilies);
        lines.add("Required capabilities: " + required);
        if (!prohibited.isEmpty())
            lines.add("Must not be present: " + prohibited);
        lines.add("Requirements are declared by the app; optional features may not be listed.");
        lines.add("Game engine is a best-effort identification, not a compatibility guarantee.");
        return String.join("\n", lines);
    }
}
