package org.podnative;

import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

/** App-scoped storage worker: imports survive Activity recreation, not process termination. */
final class GameLibrary {
    private static GameLibrary instance;
    static synchronized GameLibrary get(Context context) {
        if (instance == null)
            instance = new GameLibrary(context.getApplicationContext());
        return instance;
    }
    static final class Game {
        final File directory;
        final String id, title, version, bundleId, kind, architecture, status, report, executable;
        final long bytes, importedAt;
        final String launchReport, blockReason;
        Game(File directory, JSONObject data) throws Exception {
            this.directory = directory;
            id = directory.getName();
            title = LibraryRules.metadataText(data.opt("title"), "Untitled game");
            version = data.optString("version", "Unknown version");
            bundleId = data.optString("bundleId");
            kind = data.getString("kind");
            if (!kind.equals("IPA") && !kind.equals("Folder") && !kind.equals("Executable"))
                throw new IOException("Unknown library source kind");
            architecture = data.optString("architecture", "ARM32");
            status = data.optString("status", "Imported");
            report = data.optString("report");
            executable = data.optString("executable");
            launchReport = data.optString("lastLaunchReport");
            blockReason = data.optString("blockReason");
            bytes = data.optLong("bytes");
            importedAt = data.optLong("importedAt");
        }
        File cover() {
            return new File(directory, "cover.png");
        }
    }
    private final Context context;
    private final File root;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Runnable> observers = new ArrayList<>();
    private List<Game> games = new ArrayList<>();
    volatile boolean busy, cancelled, cancellable;
    volatile long revision;
    volatile String progress = "", notice = "";
    volatile boolean error;
    private long copied, lastProgress, nextSpaceCheck;
    private int files;
    private GameLibrary(Context context) {
        this.context = context;
        root = new File(context.getFilesDir(), "games");
        busy = true;
        worker.execute(() -> {
            try {
                mkdir(root);
                File[] entries = root.listFiles();
                if (entries != null)
                    for (File file : entries)
                        if (file.getName().startsWith(".pending-"))
                            deleteTree(file);
                // No continued URI access is needed after a completed private copy.
                for (var permission : context.getContentResolver().getPersistedUriPermissions()) {
                    try {
                        context.getContentResolver().releasePersistableUriPermission(
                            permission.getUri(),
                            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (
                        SecurityException ignored) { /* Provider may revoke an old grant itself. */
                    }
                }
                refresh();
            } catch (Exception e) {
                error = true;
                notice = e.getMessage();
            } finally {
                busy = false;
                changed();
            }
        });
    }
    synchronized List<Game> games() {
        return new ArrayList<>(games);
    }
    synchronized void observe(Runnable callback) {
        observers.add(callback);
    }
    synchronized void unobserve(Runnable callback) {
        observers.remove(callback);
    }
    private void changed() {
        main.post(() -> {
            List<Runnable> copy;
            synchronized (this) {
                copy = new ArrayList<>(observers);
            }
            for (Runnable observer : copy) observer.run();
        });
    }
    void dismissNotice() {
        notice = "";
        error = false;
        changed();
    }
    void cancel() {
        cancelled = true;
        progress = "Cancelling import…";
        changed();
    }
    private void checkCancelled() {
        if (cancelled)
            throw new CancellationException(
                "Import cancelled. Your original files were not changed.");
    }
    private synchronized boolean begin(String message) {
        if (busy)
            return false;
        busy = true;
        cancelled = false;
        progress = message;
        notice = "";
        error = false;
        copied = 0;
        files = 0;
        lastProgress = 0;
        nextSpaceCheck = 0;
        cancellable = false;
        changed();
        return true;
    }
    private void tick(String filename) {
        long now = android.os.SystemClock.elapsedRealtime();
        if (now - lastProgress < 350)
            return;
        lastProgress = now;
        progress = "Copying " + files + " files · " + formatBytes(copied) + "\n" + filename;
        changed();
    }
    private static void mkdir(File directory) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("Could not create private game storage");
    }
    private void copy(InputStream in, File destination) throws IOException {
        if (in == null)
            throw new IOException("The selected provider did not open the file");
        if (++files > 30000)
            throw new IOException("Import exceeds 30,000 files");
        checkCancelled();
        try (FileOutputStream out = new FileOutputStream(destination)) {
            byte[] buffer = new byte[65536];
            int count;
            while ((count = in.read(buffer)) != -1) {
                checkCancelled();
                copied = LibraryRules.addCopiedBytes(copied, count);
                if (copied >= nextSpaceCheck) {
                    if (destination.getParentFile().getUsableSpace() < count + 64L * 1024 * 1024)
                        throw new IOException(
                            "Not enough storage. Free space on this device and try again");
                    nextSpaceCheck = copied + 4L * 1024 * 1024;
                }
                out.write(buffer, 0, count);
                tick(destination.getName());
            }
        }
    }
    private String displayName(Uri uri) {
        try (Cursor cursor = context.getContentResolver().query(
                 uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                return LibraryRules.metadataText(cursor.getString(0), "Imported game");
        } catch (Exception ignored) { /* Provider names are optional. */
        }
        return "Imported game";
    }
    void importFile(Uri uri) {
        importSource(uri, null, false);
    }
    void importFolder(Uri uri) {
        importSource(uri, null, true);
    }
    void importLegacy(File file) {
        importSource(null, file, false);
    }
    private void importSource(Uri uri, File local, boolean tree) {
        if (!begin("Preparing import…"))
            return;
        cancellable = true;
        worker.execute(() -> {
            File pending = null;
            try {
                pending = new File(root, ".pending-" + UUID.randomUUID());
                mkdir(pending);
                String name;
                File source;
                if (tree) {
                    String document = DocumentsContract.getTreeDocumentId(uri);
                    Uri rootUri = DocumentsContract.buildDocumentUriUsingTree(uri, document);
                    name = documentName(rootUri);
                    source = new File(pending, "bundle");
                    mkdir(source);
                    copyTree(uri, document, source, 0, new HashSet<>());
                } else {
                    name = local != null ? local.getName() : displayName(uri);
                    source = new File(pending, "source.bin");
                    try (InputStream in = local != null
                            ? new FileInputStream(local)
                            : context.getContentResolver().openInputStream(uri)) {
                        copy(in, source);
                    }
                }
                checkCancelled();
                progress = "Reading app details and validating ARM32…";
                changed();
                GameInspector.Result result = GameInspector.analyze(source, name, pending);
                checkCancelled();
                JSONObject metadata = new JSONObject();
                metadata.put("title", result.title);
                metadata.put("version", result.version);
                metadata.put("bundleId", result.bundleId);
                metadata.put("architecture", result.architecture);
                metadata.put("executable", result.executable);
                metadata.put("status", result.status);
                metadata.put("blockReason", result.blockReason);
                metadata.put("report", result.report);
                metadata.put("kind", tree ? "Folder" : result.kind);
                metadata.put("bytes", copied);
                metadata.put("importedAt", System.currentTimeMillis());
                writeMetadata(pending, metadata);
                saveCover(pending, result.icon);
                checkCancelled();
                File published = new File(root, "game-" + UUID.randomUUID());
                if (!pending.renameTo(published))
                    throw new IOException(
                        "Could not finish the private import. Try again after freeing storage");
                pending = null;
                refresh();
                notice = result.title + " added to your library.";
                if (!result.blockReason.isEmpty())
                    notice += " Cannot launch: " + result.blockReason;
                if (result.status.equals("Encrypted"))
                    notice +=
                        " Its executable is encrypted and cannot run; no DRM bypass is provided.";
                Diagnostics.record("LIBRARY_IMPORT",
                    result.kind + " / " + result.architecture + " / " + result.status);
            } catch (CancellationException e) {
                notice = e.getMessage();
            } catch (Exception | LinkageError e) {
                error = true;
                notice = "Import could not finish: " + e.getMessage();
                Diagnostics.record("LIBRARY_ERROR", e.toString());
            } finally {
                if (pending != null)
                    try {
                        deleteTree(pending);
                    } catch (IOException e) {
                        Diagnostics.record("STORAGE_CLEANUP", e.toString());
                    }
                if (tree && uri != null)
                    try {
                        context.getContentResolver().releasePersistableUriPermission(
                            uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (SecurityException ignored) { /* Transient grants need no release. */
                    }
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    private String documentName(Uri document) throws IOException {
        try (Cursor cursor = context.getContentResolver().query(document,
                 new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst())
                throw new IOException("The folder is no longer available. Select it again");
            return LibraryRules.fileName(cursor.getString(0));
        }
    }
    private void copyTree(Uri tree, String id, File target, int depth, Set<String> visited)
        throws IOException {
        checkCancelled();
        if (depth > 32 || !visited.add(id))
            throw new IOException("Folder is too deeply nested or contains a document cycle");
        if (visited.size() > 30000)
            throw new IOException("Folder exceeds 30,000 document entries");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor =
                 context.getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null)
                throw new IOException("The storage provider could not list this folder");
            Set<String> names = new HashSet<>();
            while (cursor.moveToNext()) {
                checkCancelled();
                String child = cursor.getString(0),
                       name = LibraryRules.fileName(cursor.getString(1));
                if (!names.add(name))
                    throw new IOException("Folder provider returned duplicate names: " + name);
                File destination = new File(target, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    mkdir(destination);
                    copyTree(tree, child, destination, depth + 1, visited);
                } else {
                    if (!visited.add(child) || visited.size() > 30000)
                        throw new IOException("Duplicate/oversized document tree");
                    Uri file = DocumentsContract.buildDocumentUriUsingTree(tree, child);
                    try (InputStream in = context.getContentResolver().openInputStream(file)) {
                        copy(in, destination);
                    }
                }
            }
        }
    }
    private static void writeMetadata(File directory, JSONObject metadata) throws IOException {
        AtomicFile atomic = new AtomicFile(new File(directory, "game.json"));
        FileOutputStream out = null;
        try {
            out = atomic.startWrite();
            byte[] bytes = metadata.toString().getBytes(StandardCharsets.UTF_8);
            if (bytes.length > 1024 * 1024)
                throw new IOException("Library metadata exceeds its storage limit");
            out.write(bytes);
            atomic.finishWrite(out);
        } catch (IOException e) {
            if (out != null)
                atomic.failWrite(out);
            throw e;
        }
    }
    private static JSONObject readMetadata(File directory) throws Exception {
        AtomicFile atomic = new AtomicFile(new File(directory, "game.json"));
        try (InputStream in = atomic.openRead()) {
            return new JSONObject(
                new String(GameInspector.bounded(in, 1024 * 1024), StandardCharsets.UTF_8));
        }
    }
    private void refresh() {
        List<Game> loaded = new ArrayList<>();
        File[] entries = root.listFiles();
        if (entries != null)
            for (File directory : entries) {
                if (!directory.isDirectory() || !directory.getName().matches("game-[0-9a-f-]{36}"))
                    continue;
                try {
                    loaded.add(new Game(directory, readMetadata(directory)));
                } catch (Exception e) {
                    Diagnostics.record(
                        "LIBRARY_INDEX", "Unreadable entry retained: " + directory.getName());
                }
            }
        loaded.sort(Comparator.comparing(g -> g.title.toLowerCase(java.util.Locale.ROOT)));
        synchronized (this) {
            games = loaded;
            revision++;
        }
    }
    void runCpuProbe() {
        if (!begin("Running bounded ARM/Thumb CPU self-test…"))
            return;
        worker.execute(() -> {
            try {
                String result = NativeBridge.cpuProbe();
                Diagnostics.record("APP_UID_CPU_PROBE", result);
                notice = result;
                error = !result.startsWith("CPU_SELF_TEST_PASS:");
            } catch (Exception | LinkageError e) {
                notice = "CPU self-test failed: " + e.toString();
                Diagnostics.record("APP_UID_CPU_PROBE", notice);
                error = true;
            } finally {
                busy = false;
                progress = "";
                changed();
            }
        });
    }
    void preparePresentation(Game game, java.util.function.Consumer<List<File>> callback) {
        if (!begin("Preparing launch artwork…"))
            return;
        worker.execute(() -> {
            File scratch = new File(root, ".pending-art-" + UUID.randomUUID());
            List<File> images = new ArrayList<>();
            try {
                mkdir(scratch);
                File source =
                    new File(game.directory, game.kind.equals("Folder") ? "bundle" : "source.bin");
                GameInspector.Result result = GameInspector.analyze(source, game.title, scratch);
                for (int i = 0; i < result.splashImages.size(); i++) {
                    byte[] encoded = result.splashImages.get(i);
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inJustDecodeBounds = true;
                    encoded = PngArtwork.normalize(encoded);
                    BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
                    if (options.outWidth <= 0 || options.outHeight <= 0 || options.outWidth > 8192
                        || options.outHeight > 8192)
                        continue;
                    options.inSampleSize = 1;
                    while (
                        Math.max(options.outWidth, options.outHeight) / options.inSampleSize > 1024)
                        options.inSampleSize *= 2;
                    options.inJustDecodeBounds = false;
                    Bitmap bitmap =
                        BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
                    if (bitmap == null)
                        continue;
                    File image = new File(game.directory, "launch-" + i + ".png");
                    try {
                        try (FileOutputStream out = new FileOutputStream(image)) {
                            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, out))
                                throw new IOException("Could not normalize launch artwork");
                        }
                        images.add(image);
                    } finally {
                        bitmap.recycle();
                    }
                }
            } catch (Exception | LinkageError e) {
                Diagnostics.record("LAUNCH_ARTWORK", e.toString());
            } finally {
                try {
                    if (scratch.exists())
                        deleteTree(scratch);
                } catch (IOException e) {
                    Diagnostics.record("LAUNCH_CLEANUP", e.toString());
                }
                busy = false;
                progress = "";
                changed();
                main.post(() -> callback.accept(images));
            }
        });
    }
    void attemptLaunch(Game game) {
        if (!begin("Checking game startup…"))
            return;
        worker.execute(() -> {
            File scratch = new File(root, ".pending-launch-" + UUID.randomUUID());
            String outcome;
            try {
                if (!game.directory.getCanonicalFile().getParentFile().equals(
                        root.getCanonicalFile()))
                    throw new IOException("Invalid library entry");
                mkdir(scratch);
                File source =
                    new File(game.directory, game.kind.equals("Folder") ? "bundle" : "source.bin");
                GameInspector.Result result =
                    GameInspector.analyze(source, game.title, scratch, true);
                outcome = result.launchOutcome;
                if (outcome.isEmpty())
                    throw new IOException("No native launch result was returned");
            } catch (Exception | LinkageError e) {
                outcome = "LAUNCH_BLOCKED: " + e.toString() + "\nGuest code was not executed.";
            }
            try {
                if (outcome.length() > 16000)
                    outcome = outcome.substring(0, 16000) + "\nLaunch report shortened.";
                outcome = "Attempt at " + new java.util.Date()
                    + "\nBuild=" + BuildConfig.SOURCE_REVISION + "\n" + outcome;
                Diagnostics.record("LAUNCH_ATTEMPT", game.title + "\n" + outcome);
                JSONObject data = readMetadata(game.directory);
                data.put("lastLaunchReport", outcome);
                writeMetadata(game.directory, data);
                refresh();
                error = true;
                notice =
                    "Startup stopped safely. Open the game’s latest launch report for the blocker.";
            } catch (Exception e) {
                error = true;
                notice = "Could not save launch report: " + e.getMessage() + ". See Diagnostics.";
            } finally {
                try {
                    if (scratch.exists())
                        deleteTree(scratch);
                } catch (IOException e) {
                    Diagnostics.record("LAUNCH_CLEANUP", e.toString());
                }
                busy = false;
                progress = "";
                changed();
            }
        });
    }
    void rename(Game game, String title) {
        if (!begin("Updating game…"))
            return;
        worker.execute(() -> {
            try {
                JSONObject data = readMetadata(game.directory);
                data.put("title", LibraryRules.metadataText(title, game.title));
                writeMetadata(game.directory, data);
                refresh();
            } catch (Exception e) {
                error = true;
                notice = "Could not rename game: " + e.getMessage();
            } finally {
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    void remove(Game game) {
        if (!begin("Removing local copy…"))
            return;
        worker.execute(() -> {
            try {
                if (!game.directory.getCanonicalFile().getParentFile().equals(
                        root.getCanonicalFile()))
                    throw new IOException("Invalid library entry");
                deleteTree(game.directory);
                refresh();
                notice = "Removed from this device. Original files were not changed.";
            } catch (Exception e) {
                error = true;
                notice = "Could not remove game: " + e.getMessage();
                refresh();
            } finally {
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    private static void deleteTree(File file) throws IOException {
        File[] children = file.listFiles();
        if (children != null)
            for (File child : children) deleteTree(child);
        if (file.exists() && !file.delete())
            throw new IOException("Could not remove private temporary file");
    }
    private static void saveCover(File directory, byte[] encoded) {
        if (encoded == null)
            return;
        try {
            encoded = PngArtwork.normalize(encoded);
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
            if (options.outWidth <= 0 || options.outHeight <= 0 || options.outWidth > 8192
                || options.outHeight > 8192)
                return;
            options.inSampleSize = 1;
            while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize > 256)
                options.inSampleSize *= 2;
            options.inJustDecodeBounds = false;
            Bitmap image = BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
            if (image != null)
                try {
                    try (FileOutputStream out =
                             new FileOutputStream(new File(directory, "cover.png"))) {
                        image.compress(Bitmap.CompressFormat.PNG, 100, out);
                    }
                } finally {
                    image.recycle();
                }
        } catch (Exception e) {
            Diagnostics.record("COVER", "Optional artwork could not be decoded");
        }
    }
    static String formatBytes(long bytes) {
        if (bytes >= 1024L * 1024 * 1024)
            return String.format(java.util.Locale.ROOT, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
        if (bytes >= 1024 * 1024)
            return String.format(java.util.Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024));
        return String.format(java.util.Locale.ROOT, "%.0f KB", bytes / 1024.0);
    }
}
