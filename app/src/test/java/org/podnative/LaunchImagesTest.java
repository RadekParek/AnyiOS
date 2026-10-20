package org.podnative;

import static org.junit.Assert.*;

import java.util.List;
import java.util.Map;
import java.util.Set;
import org.junit.Test;

public class LaunchImagesTest {
    @Test
    public void selectsDeclaredImageBeforeFallbackAndDeduplicatesDensity() {
        var info = Map.<String, Object>of("UILaunchImageFile", "Default");
        var candidates = LaunchImages.candidates(
            info, Set.of("Default.png", "Default@2x.png", "SplashA.png", "SplashB.png"));
        assertEquals(List.of("Default@2x.png", "SplashA.png", "SplashB.png"), candidates);
        assertEquals(3, LaunchImages.MAX_IMAGES);
        assertEquals(900, LaunchImages.DISPLAY_MILLIS);
    }
    @Test
    public void rejectsPathsAndNonLaunchArtwork() {
        var info = Map.<String, Object>of("UILaunchImageFile", "../Secrets", "UILaunchImages",
            List.of(Map.of("UILaunchImageName", "SplashBrand")));
        assertEquals(List.of("SplashBrand.png"),
            LaunchImages.candidates(info,
                Set.of("../Secrets.png", "SplashBrand.png", "Icon.png", "assets/Default.png")));
    }
}
