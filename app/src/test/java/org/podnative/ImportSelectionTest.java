package org.podnative;

import static org.junit.Assert.assertEquals;

import android.app.Application;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import java.util.Arrays;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class ImportSelectionTest {
    @Test
    public void collectsMultipleDocumentPickerUrisWithoutDuplicatingThePrimaryUri() {
        Uri first = Uri.parse("content://files.test/document/game-a.ipa");
        Uri second = Uri.parse("content://files.test/document/game-b.ipa");
        Intent result = new Intent().setData(first);
        ClipData selection = new ClipData("IPA files", new String[] {"application/octet-stream"},
            new ClipData.Item(first));
        selection.addItem(new ClipData.Item(second));
        result.setClipData(selection);

        assertEquals(Arrays.asList(first, second), MainActivity.selectedFiles(result));
    }
}
