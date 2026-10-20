package org.podnative;

import android.content.Context;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Consumer;

// All backend operations, including teardown, run on one owner thread.
final class PrototypeView extends SurfaceView implements SurfaceHolder.Callback {
    private volatile boolean running;
    private Thread worker;
    private final Consumer<String> status;
    private final int fps;
    private final int shortEdge;
    PrototypeView(Context context, int fps, int shortEdge, Consumer<String> status) {
        super(context);
        this.fps = fps;
        this.shortEdge = shortEdge;
        this.status = status;
        getHolder().addCallback(this);
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { /* Wait for dimensions. */ }
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        halt();
        float scale = Math.min(1f, shortEdge / (float) Math.min(width, height));
        int w = Math.max(1, Math.round(width * scale));
        int h = Math.max(1, Math.round(height * scale));
        running = true;
        worker = new Thread(() -> {
            long start = System.nanoTime(), deadline = start, sample = start;
            long frames = 0, renderNanos = 0, maxFrame = 0;
            boolean first = true;
            boolean initialized = false;
            try {
                String info = NativeBridge.start(holder.getSurface(), w, h);
                initialized = true;
                Diagnostics.record(
                    "BACKEND", info + "\nrender size=" + w + "x" + h + " cap=" + fps);
                post(() -> status.accept(info));
                while (running) {
                    long before = System.nanoTime();
                    NativeBridge.frame((before - start) / 1_000_000_000f);
                    if (first) {
                        Diagnostics.record("FIRST_PRESENT",
                            "textured cube draw and eglSwapBuffers returned successfully; visual "
                                + "confirmation still required");
                        first = false;
                    }
                    long duration = System.nanoTime() - before;
                    renderNanos += duration;
                    maxFrame = Math.max(maxFrame, duration);
                    frames++;
                    long now = System.nanoTime();
                    if (now - sample >= 5_000_000_000L) {
                        Diagnostics.record("PROTOTYPE_METRICS",
                            "presented=" + frames + " intervalNs=" + (now - sample)
                                + " avgDrawAndSwapNs=" + renderNanos / frames
                                + " maxDrawAndSwapNs=" + maxFrame + " javaHeapBytes="
                                + (Runtime.getRuntime().totalMemory()
                                    - Runtime.getRuntime().freeMemory())
                                + "; not guest FPS or GPU time");
                        sample = now;
                        frames = 0;
                        renderNanos = 0;
                        maxFrame = 0;
                    }
                    deadline += 1_000_000_000L / fps;
                    if (deadline < now)
                        deadline = now;
                    LockSupport.parkNanos(Math.max(0, deadline - System.nanoTime()));
                }
            } catch (RuntimeException | LinkageError e) {
                Diagnostics.record("GRAPHICS_FAILURE", e.toString());
                post(()
                         -> status.accept("Graphics stopped: " + e.getMessage()
                             + "\nExport logs for diagnosis. No fallback selected."));
            } finally {
                if (initialized)
                    NativeBridge.stop();
                Diagnostics.record("SURFACE", "backend released on owner thread");
            }
        }, "PodNative-graphics");
        worker.start();
    }
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        halt();
    }
    void halt() {
        running = false;
        if (worker != null) {
            LockSupport.unpark(worker);
            boolean interrupted = false;
            while (worker.isAlive()) {
                try {
                    worker.join();
                } catch (InterruptedException e) {
                    interrupted = true;
                }
            }
            worker = null;
            if (interrupted)
                Thread.currentThread().interrupt();
        }
    }
}
