package org.podnative;

import android.view.Surface;

final class NativeBridge {
    static {
        System.loadLibrary("podnative");
    }
    private NativeBridge() {}
    static native String start(Surface surface, int width, int height);
    static native void frame(float seconds);
    static native void image(byte[] rgba, int width, int height);
    static native void stop();
    static native String inspect(byte[] bytes);
    static native String cpuProbe();
    static native String attemptLaunch(byte[] bytes, String bundlePath, String containerPath);
    // Settings ▸ "Verbose logging": names every service the guest calls, every unwind
    // decision and every GL error in the launch report. Off by default.
    static native void setVerbose(boolean on);
    // Settings ▸ "Native OpenGL ES": resolve GL against this device's own driver first and
    // keep the bundled ANGLE as the fallback. Off by default, so every device gets the same
    // driver. The driver is resolved once per launch, so this has to be set before it.
    static native void setNativeGles(boolean on);
    static native long[] arm32Slice(byte[] header, long fileLength);
}
