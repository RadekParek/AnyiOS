package org.podnative;

import android.app.Application;
import android.util.Log;

/** Close the self-contained game rather than displaying an app crash screen. */
public final class StandaloneApplication extends Application {
    @Override public void onCreate() {
        super.onCreate();
        Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
            try {
                Log.e("AnyiOSEMU", "Standalone game stopped on " + thread.getName(), failure);
            } finally {
                // Do not delegate to Android's Java crash dialog. The system can
                // still report failures that occur before this handler is installed.
                android.os.Process.killProcess(android.os.Process.myPid());
                System.exit(1);
            }
        });
    }
}
