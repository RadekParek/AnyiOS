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
        Log.i("PodNative", line);
        LOG.append(line).append('\n');
        if (LOG.length() > 200000)
            LOG.delete(0, LOG.length() - 150000);
    }
    static synchronized String export() {
        return "Framework foundations: CF bit vectors/retain/release, CG affine math. Not yet bound to guest imports.\nGraphics: embedded ANGLE/Vulkan; launch artwork presentation is not guest rendering.\nPodNative "
            + BuildConfig.VERSION_NAME + " revision=" + BuildConfig.SOURCE_REVISION
            + "\nDevice=" + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL
            + " Android=" + android.os.Build.VERSION.RELEASE + "\n" + LOG;
    }
}
