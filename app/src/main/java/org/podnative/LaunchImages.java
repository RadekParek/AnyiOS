package org.podnative;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Launch artwork only, not frames rendered by guest code. */
final class LaunchImages {
    static final int MAX_IMAGES = 3;
    static final long DISPLAY_MILLIS = 900;
    private LaunchImages() {}
    static List<String> candidates(Map<String, Object> info, Set<String> available) {
        List<String> bases = new ArrayList<>();
        Object single = info.get("UILaunchImageFile");
        if (single instanceof String)
            bases.add((String) single);
        Object entries = info.get("UILaunchImages");
        if (entries instanceof List)
            for (Object item : (List<?>) entries) {
                if (item instanceof Map) {
                Object name = ((Map<?, ?>) item).get("UILaunchImageName");
                if (name instanceof String)
                    bases.add((String) name);
                }
            }
        List<String> choices = new ArrayList<>();
        for (String name : bases) {
            String stem = name.replaceFirst("(?i)\\.png$", "");
            choices.add(stem + "@3x.png");
            choices.add(stem + "@2x.png");
            choices.add(stem + ".png");
        }
        List<String> fallback = new ArrayList<>();
        for (String name : available)
            if (name.matches("(?i)(Default|LaunchImage|Splash)[^/]*\\.png"))
                fallback.add(name);
        fallback.sort(Comparator.naturalOrder());
        choices.addAll(fallback);
        Map<String, String> families = new LinkedHashMap<>();
        for (String name : choices) {
            try {
                LibraryRules.fileName(name);
            } catch (java.io.IOException e) {
                continue;
            }
            if (!available.contains(name))
                continue;
            String family = name.replaceAll("@[23]x|~iphone|~ipad|-568h", "");
            if (!families.containsKey(family))
                families.put(family, name);
        }
        return new ArrayList<>(families.values());
    }
}
