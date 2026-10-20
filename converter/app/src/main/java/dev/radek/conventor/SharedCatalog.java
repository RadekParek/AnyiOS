package dev.radek.conventor;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/** Cross-module job ownership and publication. Source bytes have exactly one owner. */
public final class SharedCatalog {
    private static final AtomicBoolean working = new AtomicBoolean();
    public interface Callbacks {
        void sourceReady(File directory, String originalName) throws Exception;
        void changed();
    }
    private static volatile Callbacks callbacks;
    private SharedCatalog() {}
    public static void install(Callbacks value) {
        callbacks = value;
    }
    public static boolean tryBegin() {
        return working.compareAndSet(false, true);
    }
    public static boolean isBusy() {
        return working.get();
    }
    public static void end() {
        working.set(false);
    }
    public static void publish(File directory, String originalName) throws Exception {
        Callbacks value = callbacks;
        if (value != null)
            value.sourceReady(directory, originalName);
    }
    public static void changed() {
        Callbacks value = callbacks;
        if (value != null)
            value.changed();
    }
}
