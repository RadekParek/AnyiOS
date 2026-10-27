package android.net;
public class Network {
    public final String id;
    public Network(String n) {
        id = n;
    }
    public boolean equals(Object o) {
        return o instanceof Network && id.equals(((Network) o).id);
    }
    public int hashCode() {
        return id.hashCode();
    }
}
