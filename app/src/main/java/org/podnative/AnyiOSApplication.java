package org.podnative;

import android.app.Application;
import dev.radek.conventor.SharedCatalog;
import java.io.File;

public final class AnyiOSApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        GameLibrary library = GameLibrary.get(this);
        SharedCatalog.install(new SharedCatalog.Callbacks() {
            @Override
            public void sourceReady(File directory, String originalName) throws Exception {
                library.publishSharedSource(directory, originalName);
            }
            @Override
            public void changed() {
                library.reloadSharedEntries();
            }
        });
    }
}
