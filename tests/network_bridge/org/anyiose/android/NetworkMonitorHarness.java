package org.anyiose.android;
import android.content.Context;
import android.net.*;
public class NetworkMonitorHarness {
    private static native int nativeGetRoute();
    static void check(int want) {
        int got = nativeGetRoute();
        if (want != got)
            throw new AssertionError("expected " + want + " got " + got);
    }
    public static void main(String[] a) {
        System.load(a[0]);
        Network w = new Network("wifi"), c = new Network("cell");
        NetworkCapabilities wifi = new NetworkCapabilities(true, false, true),
                            cell = new NetworkCapabilities(false, true, true);
        ConnectivityManager m = new ConnectivityManager();
        m.active = w;
        m.caps = wifi;
        NetworkRouteMonitor one = new NetworkRouteMonitor(new Context(m));
        one.start();
        check(1);
        ConnectivityManager.NetworkCallback cb = m.callbacks.get(0);
        cb.onLost(w);
        check(0);
        cb.onAvailable(c);
        check(-1);
        cb.onCapabilitiesChanged(c, cell);
        check(-1);
        cb.onBlockedStatusChanged(c, false);
        check(2);
        cb.onLost(w);
        check(2);
        cb.onBlockedStatusChanged(c, true);
        check(0);
        cb.onBlockedStatusChanged(c, false);
        check(2);
        cb.onCapabilitiesChanged(c, new NetworkCapabilities(false, true, false));
        check(0);
        m.active = w;
        m.caps = wifi;
        NetworkRouteMonitor two = new NetworkRouteMonitor(new Context(m));
        two.start();
        check(1);
        one.stop();
        check(1);
        cb.onLost(w);
        check(1);
        ConnectivityManager.NetworkCallback fresh = m.callbacks.get(0);
        fresh.onLost(w);
        check(0);
        two.stop();
        check(-1);
        fresh.onCapabilitiesChanged(w, wifi);
        check(-1);
        m.active = null;
        m.caps = wifi;
        NetworkRouteMonitor disconnected = new NetworkRouteMonitor(new Context(m));
        disconnected.start();
        check(0);
        disconnected.stop();
        NetworkRouteMonitor missing = new NetworkRouteMonitor(new Context(null));
        missing.start();
        check(-1);
        missing.stop();
        System.out.println(
            "PASS JNI network monitor: snapshot, handover, restrictions, suspension, and lifecycle");
    }
}
