package org.podnative;

import android.view.Surface;

final class NativeBridge {
    static {
        System.loadLibrary("podnative");
    }
    private NativeBridge() {}
    static native String start(Surface surface, int width, int height);
    static native void frame(float seconds);
    static native void stop();
    static native String inspect(byte[] bytes);
}
