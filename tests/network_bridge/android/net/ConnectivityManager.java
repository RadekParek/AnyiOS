package android.net;
import android.os.Handler;
import java.util.*;
public class ConnectivityManager {
    public static class NetworkCallback {
        public void onAvailable(Network n) {}
        public void onCapabilitiesChanged(Network n, NetworkCapabilities c) {}
        public void onBlockedStatusChanged(Network n, boolean b) {}
        public void onLost(Network n) {}
    }
    public final List<NetworkCallback> callbacks = new ArrayList<>();
    public Network active;
    public NetworkCapabilities caps;
    public void registerDefaultNetworkCallback(NetworkCallback c, Handler h) {
        callbacks.add(c);
    }
    public Network getActiveNetwork() {
        return active;
    }
    public NetworkCapabilities getNetworkCapabilities(Network n) {
        return caps;
    }
    public void unregisterNetworkCallback(NetworkCallback c) {
        callbacks.remove(c);
    }
}
