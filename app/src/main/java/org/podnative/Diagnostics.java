package org.podnative;

import android.util.Log;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class Diagnostics {
    private static final StringBuilder LOG = new StringBuilder();
    private Diagnostics() {}
    static synchronized void record(String phase, String message) {
        String line =
            new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSZ", Locale.ROOT).format(new Date())
            + " [" + phase + "] " + message;
        Log.i("AnyiOS", line);
        LOG.append(line).append('\n');
        if (LOG.length() > 200000)
            LOG.delete(0, LOG.length() - 150000);
    }
    static synchronized String export() {
        return "Runtime: guest heap, C runtime, CoreGraphics geometry, iOS ARMv7 base-ABI call boundary, checked dyld export resolution and Objective-C message dispatch are bound to guest imports. ARMv6 and ARMv7 images are accepted and select matching instruction semantics. UIKit/EAGL/audio remain unimplemented and fail explicitly.\nGraphics: embedded ANGLE, Vulkan preferred and OpenGL ES used only when Vulkan is refused; the backend in use is named in the Graphics panel. Launch artwork presentation is not guest rendering.\nAnyiOS "
            + BuildConfig.VERSION_NAME + " revision=" + BuildConfig.SOURCE_REVISION
            + "\nDevice=" + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL
            + " Android=" + android.os.Build.VERSION.RELEASE + "\n" + LOG;
    }
}
