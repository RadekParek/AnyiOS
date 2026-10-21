package org.podnative;

import static org.junit.Assert.*;

import android.app.Application;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import dev.radek.conventor.SharedCatalog;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowContentResolver;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class BatchImportTest {
    private GameLibrary library;
    private ExecutorService worker;
    private Documents documents;
    public static class Documents extends ContentProvider {
        File ipa;
        boolean cycle;
        @Override
        public boolean onCreate() {
            return true;
        }
        @Override
        public Cursor query(
            Uri uri, String[] projection, String selection, String[] args, String sort) {
            MatrixCursor result = new MatrixCursor(projection);
            String id = DocumentsContract.getDocumentId(uri);
            String[] children = "root".equals(id) ? new String[] {"bad", "a", "nested", "text"}
                                                  : new String[] {cycle ? "root" : "b"};
            if (!"children".equals(uri.getLastPathSegment()))
                children = new String[] {id};
            for (String child : children) {
                boolean directory = child.equals("nested") || child.equals("root");
                String name = directory    ? child
                    : child.equals("text") ? "ignore.txt"
                                           : child + (child.equals("b") ? ".IPA" : ".ipa");
                Object[] row = new Object[projection.length];
                for (int i = 0; i < projection.length; i++) {
                    if (projection[i].equals(DocumentsContract.Document.COLUMN_DOCUMENT_ID))
                        row[i] = child;
                    else if (projection[i].equals(OpenableColumns.DISPLAY_NAME))
                        row[i] = name;
                    else if (projection[i].equals(DocumentsContract.Document.COLUMN_MIME_TYPE))
                        row[i] = directory ? DocumentsContract.Document.MIME_TYPE_DIR
                                           : "application/octet-stream";
                    else if (projection[i].equals(OpenableColumns.SIZE))
                        row[i] = ipa.length();
                }
                result.addRow(row);
            }
            return result;
        }
        @Override
        public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
            if (DocumentsContract.getDocumentId(uri).equals("bad"))
                throw new FileNotFoundException("Synthetic provider read failure");
            return ParcelFileDescriptor.open(ipa, ParcelFileDescriptor.MODE_READ_ONLY);
        }
        @Override
        public String getType(Uri uri) {
            return "application/octet-stream";
        }
        @Override
        public Uri insert(Uri uri, ContentValues values) {
            throw new UnsupportedOperationException();
        }
        @Override
        public int delete(Uri uri, String selection, String[] args) {
            throw new UnsupportedOperationException();
        }
        @Override
        public int update(Uri uri, ContentValues values, String selection, String[] args) {
            throw new UnsupportedOperationException();
        }
    }
    @Before
    public void setUp() throws Exception {
        SharedCatalog.end();
        SharedCatalog.install(null);
        var context = RuntimeEnvironment.getApplication();
        documents = new Documents();
        ProviderInfo info = new ProviderInfo();
        info.authority = "batch.test";
        documents.attachInfo(context, info);
        ShadowContentResolver.registerProviderInternal(info.authority, documents);
        documents.ipa = new File(context.getCacheDir(), "owned-test.ipa");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(documents.ipa))) {
            zip.putNextEntry(new ZipEntry("Payload/Test.app/Info.plist"));
            zip.write(
                "<plist><dict><key>CFBundleExecutable</key><string>Game</string><key>CFBundleDisplayName</key><string>Owned test</string></dict></plist>"
                    .getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            byte[] arm64 = new byte[32];
            arm64[0] = (byte) 0xcf;
            arm64[1] = (byte) 0xfa;
            arm64[2] = (byte) 0xed;
            arm64[3] = (byte) 0xfe;
            arm64[4] = 12;
            arm64[7] = 1;
            zip.putNextEntry(new ZipEntry("Payload/Test.app/Game"));
            zip.write(arm64);
            zip.closeEntry();
        }
        library = GameLibrary.get(context);
        var field = GameLibrary.class.getDeclaredField("worker");
        field.setAccessible(true);
        worker = (ExecutorService) field.get(library);
        drain();
    }
    private void drain() throws Exception {
        worker.submit(() -> {}).get(30, TimeUnit.SECONDS);
    }
    @After
    public void tearDown() throws Exception {
        worker.shutdownNow();
        var field = GameLibrary.class.getDeclaredField("instance");
        field.setAccessible(true);
        field.set(null, null);
        SharedCatalog.end();
    }
    @Test
    public void recursiveBatchRetainsTwoIndependentSourcesAndContinuesAfterOneFailure()
        throws Exception {
        library.importIpaFolder(DocumentsContract.buildTreeDocumentUri("batch.test", "root"));
        drain();
        assertEquals(2, library.games().size());
        assertTrue(
            library.notice, library.notice.contains("2 separate entries imported; 1 failed"));
        byte[] original = Files.readAllBytes(documents.ipa.toPath());
        for (var game : library.games()) {
            assertEquals("IPA", game.kind);
            assertEquals(original.length, game.bytes);
            assertEquals("Unsupported", game.status);
            assertArrayEquals(
                original, Files.readAllBytes(new File(game.directory, "source.bin").toPath()));
        }
        assertFalse(SharedCatalog.isBusy());
    }
    @Test
    public void cyclicProviderCannotPublishAnyPartiallyScannedBatch() throws Exception {
        documents.cycle = true;
        library.importIpaFolder(DocumentsContract.buildTreeDocumentUri("batch.test", "root"));
        drain();
        assertTrue(library.notice, library.notice.contains("cycle"));
        assertTrue(library.games().isEmpty());
        assertFalse(SharedCatalog.isBusy());
    }
}
