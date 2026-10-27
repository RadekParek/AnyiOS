package android.content;
public class Context {
    public static final String CONNECTIVITY_SERVICE = "connectivity";
    private final Object svc;
    public Context(Object svc) {
        this.svc = svc;
    }
    public Object getSystemService(String name) {
        return svc;
    }
}
