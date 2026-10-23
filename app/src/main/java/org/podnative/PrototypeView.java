package org.podnative;

import android.content.Context;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Consumer;

// All backend operations, including teardown, run on one owner thread.
final class PrototypeView extends SurfaceView implements SurfaceHolder.Callback {
    private volatile boolean running;
    private java.util.List<java.io.File> images = java.util.Collections.emptyList();
    private byte[] guestFrame;
    private int guestFrameWidth, guestFrameHeight;
    private Runnable completed;
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
    PrototypeView(Context context, java.util.List<java.io.File> images, Consumer<String> status,
        Runnable completed) {
        this(context, 60, 1080, status);
        this.images = new java.util.ArrayList<>(
            images.subList(0, Math.min(LaunchImages.MAX_IMAGES, images.size())));
        this.completed = completed;
    }
    PrototypeView(Context context, int width, int height, byte[] rgba, int fps, int shortEdge,
        Consumer<String> status) {
        this(context, fps, shortEdge, status);
        if (width <= 0 || height <= 0 || width > 2048 || height > 2048 || rgba == null
            || rgba.length != width * height * 4)
            throw new IllegalArgumentException("Invalid guest framebuffer pixels");
        guestFrameWidth = width;
        guestFrameHeight = height;
        guestFrame = rgba.clone();
    }
    private void upload(java.io.File file) {
        android.graphics.Bitmap bitmap = android.graphics.BitmapFactory.decodeFile(file.getPath());
        if (bitmap == null)
            throw new IllegalStateException("Launch artwork could not be decoded");
        try {
            int w = bitmap.getWidth(), h = bitmap.getHeight();
            if (w > 2048 || h > 2048)
                throw new IllegalStateException("Launch artwork is too large");
            int[] argb = new int[w * h];
            bitmap.getPixels(argb, 0, w, 0, 0, w, h);
            byte[] rgba = new byte[w * h * 4];
            for (int i = 0; i < argb.length; i++) {
                rgba[i * 4] = (byte) (argb[i] >> 16);
                rgba[i * 4 + 1] = (byte) (argb[i] >> 8);
                rgba[i * 4 + 2] = (byte) argb[i];
                rgba[i * 4 + 3] = (byte) (argb[i] >>> 24);
            }
            NativeBridge.image(rgba, w, h);
        } finally {
            bitmap.recycle();
        }
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
                int imageIndex = 0;
                long imagePresented = 0;
                if (guestFrame != null)
                    NativeBridge.image(guestFrame, guestFrameWidth, guestFrameHeight);
                else if (!images.isEmpty())
                    upload(images.get(0));
                while (running) {
                    if (!images.isEmpty() && imagePresented != 0
                        && System.nanoTime() - imagePresented
                            >= LaunchImages.DISPLAY_MILLIS * 1_000_000L) {
                        imageIndex++;
                        if (imageIndex >= images.size()) {
                            running = false;
                            if (completed != null)
                                post(completed);
                            break;
                        }
                        upload(images.get(imageIndex));
                        imagePresented = 0;
                    }
                    long before = System.nanoTime();
                    NativeBridge.frame((before - start) / 1_000_000_000f);
                    if (!images.isEmpty() && imagePresented == 0)
                        imagePresented = System.nanoTime();
                    if (first) {
                        Diagnostics.record("FIRST_PRESENT",
                            (guestFrame != null        ? "captured guest framebuffer"
                                    : images.isEmpty() ? "textured cube"
                                                       : "imported launch artwork")
                                + " draw and eglSwapBuffers returned successfully; visual confirmation still required"
                                + (guestFrame != null
                                        ? "; bounded guest execution preceded this readback"
                                        : "; not guest execution"));
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
