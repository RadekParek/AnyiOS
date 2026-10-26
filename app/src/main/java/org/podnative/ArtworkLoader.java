package org.podnative;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.widget.ImageView;
import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Downsamples high-resolution library artwork off the UI thread. */
final class ArtworkLoader {
    private static final int CACHE_BYTES = 16 * 1024 * 1024;
    private static final ExecutorService WORKERS = Executors.newFixedThreadPool(2);
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final LruCache<String, Bitmap> CACHE =
        new LruCache<String, Bitmap>(CACHE_BYTES) {
            @Override
            protected int sizeOf(String key, Bitmap bitmap) {
                return bitmap.getByteCount();
            }
        };

    private ArtworkLoader() {}

    static void load(File file, int targetPixels, ImageView image) {
        if (file == null || !file.isFile() || targetPixels <= 0)
            return;
        String key = file.getAbsolutePath() + ":" + file.lastModified() + ":" + targetPixels;
        image.setTag(key);
        Bitmap cached = CACHE.get(key);
        if (cached != null) {
            image.setImageBitmap(cached);
            return;
        }
        WORKERS.execute(() -> {
            Bitmap bitmap = decode(file, targetPixels);
            if (bitmap != null)
                CACHE.put(key, bitmap);
            MAIN.post(() -> {
                if (bitmap != null && key.equals(image.getTag()))
                    image.setImageBitmap(bitmap);
            });
        });
    }

    private static Bitmap decode(File file, int targetPixels) {
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0)
                return null;
            int sample = 1;
            int longestEdge = Math.max(bounds.outWidth, bounds.outHeight);
            while (longestEdge / (sample * 2) >= targetPixels && sample < 512) sample *= 2;
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            return BitmapFactory.decodeFile(file.getAbsolutePath(), options);
        } catch (RuntimeException ignored) {
            return null;
        }
    }
}
