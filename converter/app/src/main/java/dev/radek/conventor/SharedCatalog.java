package dev.radek.conventor;

import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;

/** Cross-module job ownership and publication. Source bytes have exactly one owner. */
public final class SharedCatalog {
    private static final AtomicBoolean working = new AtomicBoolean();
    public interface Callbacks {
        void sourceReady(File directory, String originalName) throws Exception;
        void changed();
    }
    private static volatile Callbacks callbacks;
    private static final CopyOnWriteArrayList<Runnable> views = new CopyOnWriteArrayList<>();
    private SharedCatalog() {}
    public static void install(Callbacks value) {
        callbacks = value;
    }
    public static boolean tryBegin() {
        boolean acquired = working.compareAndSet(false, true);
        if (acquired)
            notifyViews();
        return acquired;
    }
    public static boolean isBusy() {
        return working.get();
    }
    public static void end() {
        if (working.getAndSet(false))
            notifyViews();
    }
    public static void publish(File directory, String originalName) throws Exception {
        Callbacks value = callbacks;
        if (value != null)
            value.sourceReady(directory, originalName);
    }
    public static void observe(Runnable view) {
        views.addIfAbsent(view);
    }
    public static void unobserve(Runnable view) {
        views.remove(view);
    }
    public static void notifyViews() {
        if (!views.isEmpty())
            new Handler(Looper.getMainLooper()).post(() -> {
                for (Runnable view : views) view.run();
            });
    }
    public static void changed() {
        Callbacks value = callbacks;
        if (value != null)
            value.changed();
        notifyViews();
    }
}
