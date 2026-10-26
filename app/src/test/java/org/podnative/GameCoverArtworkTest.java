package org.podnative;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.ByteArrayOutputStream;
import java.io.File;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class GameCoverArtworkTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void preservesFourTimesCoverPixels() throws Exception {
        Bitmap source = Bitmap.createBitmap(4096, 4, Bitmap.Config.ARGB_8888);
        ByteArrayOutputStream encoded = new ByteArrayOutputStream();
        try {
            assertTrue(source.compress(Bitmap.CompressFormat.PNG, 100, encoded));
        } finally {
            source.recycle();
        }

        GameLibrary.saveCover(temporary.getRoot(), encoded.toByteArray());

        File cover = new File(temporary.getRoot(), "cover.png");
        assertTrue(cover.isFile());
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(cover.getAbsolutePath(), bounds);
        assertEquals(2048, bounds.outWidth);
        assertEquals(2, bounds.outHeight);
    }
}
