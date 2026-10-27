package org.anyiose.android;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class NetworkRouteTrackerTest {
    @Test public void routeFollowsConnectedWifiMobileAndAppRestrictions() {
        NetworkRouteTracker<String> tracker = new NetworkRouteTracker<>();
        assertEquals(NetworkRouteTracker.NONE, tracker.snapshot(null, NetworkRouteTracker.UNKNOWN));
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.available("wifi"));
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.capabilities("wifi", NetworkRouteTracker.WIFI));
        assertEquals(NetworkRouteTracker.WIFI, tracker.blocked("wifi", false));
        assertEquals(NetworkRouteTracker.NONE, tracker.blocked("wifi", true));
        assertEquals(NetworkRouteTracker.WIFI, tracker.blocked("wifi", false));
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.available("mobile"));
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.lost("wifi")); // stale handover event
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.capabilities("mobile", NetworkRouteTracker.CELLULAR));
        assertEquals(NetworkRouteTracker.CELLULAR, tracker.blocked("mobile", false));
        assertEquals(NetworkRouteTracker.OTHER, tracker.capabilities("mobile", NetworkRouteTracker.OTHER));
        assertEquals(NetworkRouteTracker.NONE, tracker.capabilities("mobile", NetworkRouteTracker.NONE)); // suspended
        assertEquals(NetworkRouteTracker.NONE, tracker.lost("mobile"));
    }

    @Test public void snapshotsAndUnrelatedEventsDoNotInventAnActiveConnection() {
        NetworkRouteTracker<String> tracker = new NetworkRouteTracker<>();
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.value());
        assertEquals(NetworkRouteTracker.NONE, tracker.snapshot(null, NetworkRouteTracker.WIFI));
        assertEquals(NetworkRouteTracker.NONE, tracker.capabilities("old", NetworkRouteTracker.WIFI));
        assertEquals(NetworkRouteTracker.NONE, tracker.blocked("old", false));
        assertEquals(NetworkRouteTracker.WIFI, tracker.snapshot("wifi", NetworkRouteTracker.WIFI));
        assertEquals(NetworkRouteTracker.WIFI, tracker.lost("old"));
        assertEquals(NetworkRouteTracker.UNKNOWN, tracker.capabilities("wifi", NetworkRouteTracker.UNKNOWN));
    }
}
