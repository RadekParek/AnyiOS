package android.net;
public class NetworkCapabilities {
    public static final int NET_CAPABILITY_NOT_SUSPENDED = 21, TRANSPORT_WIFI = 1,
                            TRANSPORT_CELLULAR = 0;
    private final boolean w, c, s;
    public NetworkCapabilities(boolean w, boolean c, boolean s) {
        this.w = w;
        this.c = c;
        this.s = s;
    }
    public boolean hasCapability(int t) {
        return s && t == NET_CAPABILITY_NOT_SUSPENDED;
    }
    public boolean hasTransport(int t) {
        return (w && t == TRANSPORT_WIFI) || (c && t == TRANSPORT_CELLULAR);
    }
}
