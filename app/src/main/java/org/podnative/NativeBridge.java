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
    static native long[] arm32Slice(byte[] header, long fileLength);
}
