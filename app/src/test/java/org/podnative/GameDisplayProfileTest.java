package org.podnative;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class GameDisplayProfileTest {
    @Test public void defaultAndUnknownProfilesMatchTheStandaloneDisplayPath() {
        assertEquals("--device-family=auto", GameDisplayProfile.argument(GameDisplayProfile.AUTO));
        assertEquals("--device-family=auto", GameDisplayProfile.argument(-1));
    }

    @Test public void classicProfilesUseTheirOriginalLogicalScreens() {
        assertEquals("--device-family=iphone-3gs", GameDisplayProfile.argument(GameDisplayProfile.CLASSIC_PHONE));
        assertEquals("--device-family=iphone-5", GameDisplayProfile.argument(GameDisplayProfile.TALL_PHONE));
        assertEquals("--device-family=ipad-2", GameDisplayProfile.argument(GameDisplayProfile.TABLET));
    }
}
