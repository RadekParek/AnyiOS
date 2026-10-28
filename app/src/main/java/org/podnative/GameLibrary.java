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
import android.webkit.MimeTypeMap;
import dev.radek.conventor.SharedCatalog;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
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
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.json.JSONObject;

/** App-scoped asynchronous worker for the durable app-private game library and sandboxes. */
final class GameLibrary {
    private static final long MAX_SANDBOX_EXPORT_BYTES = 4L * 1024 * 1024 * 1024;
    private static final int MAX_SANDBOX_EXPORT_FILES = 30000;
    private static final int MAX_SANDBOX_EXPORT_DEPTH = 32;
    // Retain artwork at four times the original 512-pixel library source cap.
    private static final int COVER_MAX_EDGE = 2048;
    private static GameLibrary instance;
    static synchronized GameLibrary get(Context context) {
        if (instance == null)
            instance = new GameLibrary(context.getApplicationContext());
        return instance;
    }
    static final class LaunchResult {
        final String report;
        final int frameWidth, frameHeight;
        final byte[] guestFrame;
        LaunchResult(String report, int frameWidth, int frameHeight, byte[] guestFrame) {
            this.report = report;
            this.frameWidth = frameWidth;
            this.frameHeight = frameHeight;
            this.guestFrame = guestFrame;
        }
        boolean hasFrame() {
            return guestFrame != null && frameWidth > 0 && frameHeight > 0
                && guestFrame.length == frameWidth * frameHeight * 4;
        }
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
            File cover = new File(directory, "cover.png");
            return cover.isFile() ? cover : new File(directory, "icon.png");
        }
        boolean canLaunchInAnyiOSEMU() {
            if ((!kind.equals("IPA") && !kind.equals("Folder")) || !blockReason.isEmpty()
                || status.equals("Encrypted") || status.equals("Unsupported")
                || status.equals("Invalid"))
                return false;
            String architectureName = architecture.toUpperCase(java.util.Locale.ROOT);
            // Universal display labels list ARM64 first but remain launchable via ARM32.
            if (architectureName.contains("ARM32 SELECTED"))
                return true;
            int separator = architectureName.indexOf(' ');
            String architectureFamily =
                separator < 0 ? architectureName : architectureName.substring(0, separator);
            return architectureFamily.equals("ARM32") || architectureFamily.equals("ARMV5TEJ")
                || architectureFamily.equals("ARMV6") || architectureFamily.equals("ARMV7")
                || architectureFamily.equals("ARMV7F") || architectureFamily.equals("ARMV7S");
        }
        /** Only positive slice labels count; explanatory text can say "no ARM64". */
        String architectureChip() {
            return GameArchitecture.chip(architecture);
        }
        File source() {
            return new File(directory, kind.equals("Folder") ? "bundle" : "source.bin");
        }
        /** Resolve the actual .app root retained inside an imported folder, if it was nested. */
        File anyioseSource() {
            File source = source();
            if (kind.equals("Folder") && !executable.isEmpty()) {
                try {
                    File root = source.getCanonicalFile();
                    File executableFile = new File(source, executable).getCanonicalFile();
                    if (executableFile.toPath().startsWith(root.toPath())) {
                        File bundle = executableFile.getParentFile();
                        if (bundle != null && new File(bundle, "Info.plist").isFile())
                            return bundle;
                    }
                } catch (IOException ignored) { /* Runtime preparation reports the missing path. */
                }
            }
            return source;
        }
        File anyioseDataDirectory() {
            return new File(directory, "sandbox/AnyiOSEMU");
        }
    }
    private final Context context;
    private final File root;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Runnable> observers = new ArrayList<>();
    private List<Game> games = new ArrayList<>();
    volatile boolean busy, cancelled, cancellable, launchCancelled;
    volatile long revision;
    volatile String progress = "", notice = "";
    volatile boolean error;
    private long copied, lastProgress, nextSpaceCheck;
    private int files;
    private GameLibrary(Context context) {
        this.context = context;
        root = new File(context.getFilesDir(), "games");
        busy = true;
        SharedCatalog.tryBegin();
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
                    int flags = (permission.isReadPermission()
                                        ? android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                                        : 0)
                        | (permission.isWritePermission()
                                ? android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                                : 0);
                    if (flags == 0)
                        continue;
                    try {
                        context.getContentResolver().releasePersistableUriPermission(
                            permission.getUri(), flags);
                    } catch (Exception ignored) {
                        // Provider may revoke an old grant itself.
                    }
                }
                refresh();
            } catch (Exception e) {
                error = true;
                notice = e.getMessage();
            } finally {
                SharedCatalog.end();
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
        SharedCatalog.notifyViews();
        main.post(() -> {
            List<Runnable> copy;
            synchronized (this) {
                copy = new ArrayList<>(observers);
            }
            for (Runnable observer : copy) observer.run();
        });
    }
    /** Converter publishes a completed source copy; no second IPA copy is made. */
    void publishSharedSource(File directory, String originalName) throws Exception {
        if (!directory.getCanonicalFile().getParentFile().equals(root.getCanonicalFile())
            || !directory.getName().matches("game-[0-9a-f-]{36}"))
            throw new IOException("Invalid shared source directory");
        File source = new File(directory, "source.bin");
        ensureSandbox(directory);
        GameInspector.Result result = GameInspector.analyze(source, originalName, directory);
        JSONObject metadata = new JSONObject();
        metadata.put("title", result.title);
        metadata.put("version", result.version);
        metadata.put("bundleId", result.bundleId);
        metadata.put("architecture", result.architecture);
        metadata.put("executable", result.executable);
        metadata.put("status", result.status);
        metadata.put("blockReason", result.blockReason);
        metadata.put("report", result.report);
        metadata.put("kind", "IPA");
        metadata.put("bytes", source.length());
        metadata.put("importedAt", System.currentTimeMillis());
        metadata.put("originalName", originalName);
        writeMetadata(directory, metadata);
        saveCover(directory, result.icon);
        refresh();
        changed();
    }
    void reloadSharedEntries() {
        worker.execute(() -> {
            refresh();
            changed();
        });
    }
    /** Parse the existing private IPA/app asynchronously; old imports need no re-import. */
    void loadRequirements(Game game, java.util.function.Consumer<GameRequirements> callback) {
        worker.execute(() -> {
            GameRequirements requirements;
            try {
                requirements = GameInspector.readRequirements(game.source(), game.kind);
            } catch (Exception e) {
                Diagnostics.record("GAME_REQUIREMENTS", e.toString());
                requirements = GameRequirements.unavailable();
            }
            try {
                GameEngineDetector logEvidence = new GameEngineDetector();
                logEvidence.observeRuntimeLog(LatestGameLog.preview(
                    game.anyioseDataDirectory(), LatestGameLog.PREVIEW_BYTES));
                requirements = requirements.withEngine(
                    GameEngineDetector.merge(requirements.gameEngine, logEvidence.result()));
            } catch (IOException | SecurityException ignored) { /* Optional local evidence. */
            }
            GameRequirements result = requirements;
            main.post(() -> callback.accept(result));
        });
    }

    /** Read the real latest per-game runtime file, even if its activity never returned. */
    void loadLatestLog(Game game, java.util.function.Consumer<String> callback) {
        worker.execute(() -> {
            String text;
            try {
                text =
                    LatestGameLog.preview(game.anyioseDataDirectory(), LatestGameLog.PREVIEW_BYTES);
            } catch (IOException e) {
                text = "Could not read the game log: " + e.getMessage();
            }
            String result = text;
            main.post(() -> callback.accept(result));
        });
    }

    void dismissNotice() {
        notice = "";
        error = false;
        changed();
    }
    void cancel() {
        cancelled = true;
        progress = "Cancelling operation…";
        changed();
    }
    void cancelLaunch() {
        launchCancelled = true;
        NativeBridge.cancelGuestLaunch();
    }
    private void checkCancelled() {
        if (cancelled)
            throw new CancellationException(
                "Operation cancelled. Your original files were not changed.");
    }
    private synchronized boolean begin(String message) {
        if (busy || !SharedCatalog.tryBegin()) {
            notice = "Wait for the current Emulator or Converter job to finish.";
            changed();
            return false;
        }
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
    private static final class SandboxPaths {
        final File root, documents, bundleCache, bundleOverlay, anyiose;
        SandboxPaths(File root) {
            this.root = root;
            documents = new File(root, "Documents");
            bundleCache = new File(root, "ZipCache");
            bundleOverlay = new File(root, "BundleOverrides");
            anyiose = new File(root, "AnyiOSEMU");
        }
    }
    private static void ensurePrivateDirectory(File root, File directory) throws IOException {
        if (java.nio.file.Files.isSymbolicLink(directory.toPath()))
            throw new IOException("Sandbox path refused a symbolic link");
        mkdir(directory);
        if (!directory.getCanonicalFile().toPath().startsWith(root.getCanonicalFile().toPath()))
            throw new IOException("Sandbox directory escaped its private root");
    }
    private SandboxPaths ensureSandbox(File gameDirectory) throws IOException {
        File sandbox = new File(gameDirectory, "sandbox");
        ensurePrivateDirectory(gameDirectory, sandbox);
        SandboxPaths paths = new SandboxPaths(sandbox);
        ensurePrivateDirectory(sandbox, paths.documents);
        ensurePrivateDirectory(sandbox, paths.bundleCache);
        ensurePrivateDirectory(sandbox, paths.bundleOverlay);
        ensurePrivateDirectory(sandbox, paths.anyiose);
        File legacyContainer = new File(gameDirectory, "container");
        if (java.nio.file.Files.isSymbolicLink(legacyContainer.toPath()))
            throw new IOException("Legacy game data refused a symbolic link");
        if (legacyContainer.isDirectory()) {
            mergeLegacyContainer(legacyContainer, paths.documents);
            deleteTree(legacyContainer);
        }
        return paths;
    }
    private static void mergeLegacyContainer(File source, File destination) throws IOException {
        mkdir(destination);
        File[] children = source.listFiles();
        if (children == null)
            throw new IOException("Could not read the previous game-data folder");
        for (File child : children) {
            if (java.nio.file.Files.isSymbolicLink(child.toPath()))
                throw new IOException("Legacy game data refused a symbolic link");
            File target = new File(destination, child.getName());
            if (java.nio.file.Files.isSymbolicLink(target.toPath()))
                throw new IOException("Existing game data refused a symbolic link");
            if (child.isDirectory()) {
                if (target.exists() && !target.isDirectory())
                    throw new IOException("Legacy game data conflicts with a sandbox file");
                if (!target.exists()) {
                    if (!child.renameTo(target))
                        mergeLegacyContainer(child, target);
                } else {
                    mergeLegacyContainer(child, target);
                }
            } else {
                if (target.exists()) {
                    if (!target.isFile())
                        throw new IOException(
                            "Legacy game data conflicts with a sandbox directory");
                    if (sameFileContents(child, target))
                        continue;
                    target =
                        new File(destination, child.getName() + ".legacy-" + UUID.randomUUID());
                }
                if (!child.renameTo(target)) {
                    try {
                        java.nio.file.Files.copy(child.toPath(), target.toPath());
                    } catch (java.nio.file.FileAlreadyExistsException ignored) {
                        // A concurrent/partial migration already published this copy.
                    }
                }
            }
        }
    }
    private static boolean sameFileContents(File first, File second) throws IOException {
        if (first.length() != second.length())
            return false;
        try (InputStream a = new FileInputStream(first);
            InputStream b = new FileInputStream(second)) {
            byte[] left = new byte[65536], right = new byte[65536];
            while (true) {
                int n = a.read(left), m = b.read(right);
                if (n != m)
                    return false;
                if (n < 0)
                    return true;
                for (int i = 0; i < n; i++)
                    if (left[i] != right[i])
                        return false;
            }
        }
    }
    private void copy(InputStream in, File destination) throws IOException {
        if (in == null)
            throw new IOException("The selected provider did not open the file");
        if (++files > 30000)
            throw new IOException("Import exceeds 30,000 files");
        checkCancelled();
        try (FileOutputStream out = new FileOutputStream(destination)) {
            byte[] buffer = new byte[262144];
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
    void importFiles(List<Uri> sources) {
        if (sources == null || sources.isEmpty() || !begin("Importing selected game files…"))
            return;
        cancellable = true;
        worker.execute(() -> {
            int imported = 0, failed = 0;
            StringBuilder failures = new StringBuilder();
            Set<String> visited = new HashSet<>();
            try {
                for (Uri source : sources) {
                    if (source == null || !visited.add(source.toString()))
                        continue;
                    checkCancelled();
                    try {
                        importOne(source, null, false);
                        imported++;
                    } catch (CancellationException e) {
                        throw e;
                    } catch (Exception | LinkageError e) {
                        failed++;
                        if (failed <= 20)
                            failures.append("\n")
                                .append(displayName(source))
                                .append(": ")
                                .append(e.getMessage() == null ? e.toString() : e.getMessage());
                        Diagnostics.record("BATCH_IMPORT", source + ": " + e);
                    }
                }
                notice = imported + " of " + visited.size() + " selected files imported; " + failed
                    + " failed." + failures;
                error = failed > 0;
            } catch (CancellationException e) {
                notice = "Batch cancelled. " + imported + " completed entries were kept.";
            } finally {
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    void importFolder(Uri uri) {
        importSource(uri, null, true);
    }
    void importIpaFolder(Uri tree) {
        if (!begin("Finding IPA files in the selected folder…"))
            return;
        cancellable = true;
        worker.execute(() -> {
            int imported = 0, failed = 0;
            StringBuilder failures = new StringBuilder();
            try {
                List<Uri> sources = new ArrayList<>();
                scanIpas(
                    tree, DocumentsContract.getTreeDocumentId(tree), 0, new HashSet<>(), sources);
                for (Uri source : sources) {
                    checkCancelled();
                    try {
                        importOne(source, null, false);
                        imported++;
                    } catch (CancellationException e) {
                        throw e;
                    } catch (Exception | LinkageError e) {
                        failed++;
                        if (failed <= 20)
                            failures.append("\n")
                                .append(displayName(source))
                                .append(": ")
                                .append(e.getMessage() == null ? e.toString() : e.getMessage());
                        Diagnostics.record("BATCH_IMPORT", source + ": " + e);
                    }
                }
                notice = sources.isEmpty()
                    ? "No IPA files found. For an extracted .app bundle, use Import extracted app folder."
                    : imported + " separate entries imported; " + failed + " failed." + failures;
                error = failed > 0;
            } catch (CancellationException e) {
                notice = "Batch cancelled. " + imported + " completed entries were kept.";
            } catch (Exception e) {
                error = true;
                String detail = e.getMessage() == null ? e.toString() : e.getMessage();
                notice = "Folder search failed: " + detail
                    + ". If Android denied this location, choose an accessible subfolder under Documents; "
                    + "the folder picker cannot grant the storage root, Downloads root, Android/data "
                    + "or Android/obb. For individual IPAs in Downloads, use Import IPA file(s). "
                    + imported + " completed entries were kept.";
            } finally {
                try {
                    context.getContentResolver().releasePersistableUriPermission(
                        tree, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException ignored) { /* Temporary grant. */
                }
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    private void scanIpas(Uri tree, String id, int depth, Set<String> visited, List<Uri> found)
        throws IOException {
        checkCancelled();
        if (depth > 32 || !visited.add(id) || visited.size() > 30000)
            throw new IOException(
                "Folder exceeds safe traversal depth/document count or contains a cycle");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        try (Cursor cursor = context.getContentResolver().query(children,
                 new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                     DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                     DocumentsContract.Document.COLUMN_MIME_TYPE},
                 null, null, null)) {
            if (cursor == null)
                throw new IOException("Provider could not list folder");
            while (cursor.moveToNext()) {
                checkCancelled();
                String child = cursor.getString(0), name = cursor.getString(1),
                       mime = cursor.getString(2);
                if (child == null || name == null)
                    throw new IOException("Provider returned an invalid document");
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    scanIpas(tree, child, depth + 1, visited, found);
                } else {
                    if (!visited.add(child) || visited.size() > 30000)
                        throw new IOException("Duplicate/oversized document tree");
                    if (name.toLowerCase(java.util.Locale.ROOT).endsWith(".ipa"))
                        found.add(DocumentsContract.buildDocumentUriUsingTree(tree, child));
                }
            }
        }
    }
    void importLegacy(File file) {
        importSource(null, file, false);
    }
    private void importSource(Uri uri, File local, boolean tree) {
        if (!begin("Preparing import…"))
            return;
        cancellable = true;
        worker.execute(() -> {
            try {
                importOne(uri, local, tree);
            } catch (CancellationException e) {
                notice = e.getMessage();
            } catch (Exception | LinkageError e) {
                error = true;
                String detail = e.getMessage() == null ? e.toString() : e.getMessage();
                notice = "Import could not finish: " + detail;
                if (tree)
                    notice +=
                        ". If Android denied this folder, choose an accessible normal subfolder; "
                        + "the picker blocks the storage root, Downloads root, Android/data and Android/obb.";
                Diagnostics.record("LIBRARY_ERROR", e.toString());
            } finally {
                if (tree && uri != null)
                    try {
                        context.getContentResolver().releasePersistableUriPermission(
                            uri, android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (SecurityException ignored) { /* Transient grants need no release. */
                    }
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    private File importOne(Uri uri, File local, boolean tree) throws Exception {
        File pending = null;
        long before = copied;
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
            ensureSandbox(pending);
            JSONObject metadata = new JSONObject();
            metadata.put("title", result.title);
            metadata.put("originalName", name);
            metadata.put("version", result.version);
            metadata.put("bundleId", result.bundleId);
            metadata.put("architecture", result.architecture);
            metadata.put("executable", result.executable);
            metadata.put("status", result.status);
            metadata.put("blockReason", result.blockReason);
            metadata.put("report", result.report);
            metadata.put("kind", tree ? "Folder" : result.kind);
            metadata.put("bytes", copied - before);
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
                notice += " Its executable is encrypted and cannot run; no DRM bypass is provided.";
            Diagnostics.record("LIBRARY_IMPORT",
                result.kind + " / " + result.architecture + " / " + result.status);

            return published;
        } finally {
            if (pending != null)
                try {
                    deleteTree(pending);
                } catch (IOException e) {
                    Diagnostics.record("STORAGE_CLEANUP", e.toString());
                }
        }
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
                    if (!new File(directory, "cover.png").isFile()
                        && new File(directory, "icon.png").isFile()) {
                        try (InputStream icon =
                                 new FileInputStream(new File(directory, "icon.png"))) {
                            saveCover(directory, GameInspector.bounded(icon, 8 * 1024 * 1024));
                        } catch (Exception e) {
                            Diagnostics.record("SHARED_ICON", e.toString());
                        }
                    }
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
    void refreshGame(Game game) {
        refreshGame(game, null);
    }
    void refreshGame(Game game, Runnable completed) {
        if (!begin("Refreshing app details and artwork…")) {
            if (completed != null)
                main.post(completed);
            return;
        }
        worker.execute(() -> {
            File scratch = new File(root, ".pending-refresh-" + UUID.randomUUID());
            try {
                mkdir(scratch);
                File source =
                    new File(game.directory, game.kind.equals("Folder") ? "bundle" : "source.bin");
                GameInspector.Result result = GameInspector.analyze(source, game.title, scratch);
                JSONObject data = readMetadata(game.directory);
                // Keep the user's custom title, original import time and source copy.
                data.put("version", result.version);
                data.put("bundleId", result.bundleId);
                data.put("architecture", result.architecture);
                data.put("status", result.status);
                data.put("blockReason", result.blockReason);
                data.put("report", result.report);
                data.put("executable", result.executable);
                writeMetadata(game.directory, data);
                saveCover(game.directory, result.icon);
                refresh();
                notice =
                    "Details refreshed without recopying the game. Unsupported artwork may still use a placeholder.";
            } catch (Exception | LinkageError e) {
                error = true;
                notice = "Could not refresh details: " + e.getMessage();
            } finally {
                try {
                    if (scratch.exists())
                        deleteTree(scratch);
                } catch (IOException e) {
                    Diagnostics.record("REFRESH_CLEANUP", e.toString());
                }
                SharedCatalog.end();
                busy = false;
                progress = "";
                changed();
                if (completed != null)
                    main.post(completed);
            }
        });
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
                SharedCatalog.end();
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
                ensureSandbox(game.directory);
                File source =
                    new File(game.directory, game.kind.equals("Folder") ? "bundle" : "source.bin");
                GameInspector.Result result = GameInspector.analyze(source, game.title, scratch);
                saveCover(game.directory, result.icon);
                for (int i = 0; i < result.splashImages.size(); i++) {
                    if (images.size() >= LaunchImages.MAX_IMAGES)
                        break;
                    try {
                        byte[] encoded = result.splashImages.get(i);
                        BitmapFactory.Options options = new BitmapFactory.Options();
                        options.inJustDecodeBounds = true;
                        encoded = PngArtwork.normalize(encoded);
                        BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
                        if (options.outWidth <= 0 || options.outHeight <= 0
                            || options.outWidth > 8192 || options.outHeight > 8192)
                            continue;
                        options.inSampleSize = 1;
                        while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize
                            > 1024)
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
                    } catch (IOException | RuntimeException e) {
                        Diagnostics.record("LAUNCH_ARTWORK_IMAGE", e.toString());
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
                SharedCatalog.end();
                busy = false;
                progress = "";
                changed();
                main.post(() -> callback.accept(images));
            }
        });
    }
    private File prepareRuntimeIpaSource(File gameDirectory, File source) throws IOException {
        if (!source.isFile() || !source.canRead()
            || java.nio.file.Files.isSymbolicLink(source.toPath()))
            throw new IOException("The private IPA source is not a readable regular file");

        File alias = new File(gameDirectory, "AnyiOSEMU_source.ipa");
        File marker = new File(gameDirectory, ".AnyiOSEMU_source.ipa.meta");
        File temporary = new File(gameDirectory, ".AnyiOSEMU_source.ipa.pending");
        if (java.nio.file.Files.isSymbolicLink(alias.toPath())
            || java.nio.file.Files.isSymbolicLink(marker.toPath())
            || java.nio.file.Files.isSymbolicLink(temporary.toPath()))
            throw new IOException("A symbolic link was found in the private IPA launch paths");
        if (alias.exists() && !alias.isFile())
            throw new IOException("The emulator IPA alias is not a regular file");
        if (marker.exists() && !marker.isFile())
            throw new IOException("The emulator IPA cache marker is not a regular file");
        if (temporary.exists() && !temporary.isFile())
            throw new IOException("The temporary emulator IPA path is not a regular file");

        String fingerprint = source.length() + "\n" + source.lastModified() + "\n";
        if (alias.isFile()) {
            if (java.nio.file.Files.isSameFile(alias.toPath(), source.toPath()))
                return alias;
            if (alias.length() == source.length()
                && (runtimeIpaMarkerMatches(marker, fingerprint)
                    || alias.lastModified() == source.lastModified()))
                return alias;
        }
        java.nio.file.Files.deleteIfExists(alias.toPath());
        java.nio.file.Files.deleteIfExists(marker.toPath());
        java.nio.file.Files.deleteIfExists(temporary.toPath());

        try {
            java.nio.file.Files.createLink(alias.toPath(), source.toPath());
            return alias;
        } catch (IOException | UnsupportedOperationException | SecurityException e) {
            Diagnostics.record("ANYIOSEMU_SOURCE_LINK", e.toString());
            // Some Android filesystems/ROMs forbid hard links inside app-private storage.
        }

        final long expected = source.length();
        final long[] lastUpdate = {0};
        return copyRuntimeIpaAlias(source, alias, temporary, marker, gameDirectory.getUsableSpace(),
            this::checkCancelled, bytes -> {
                long now = android.os.SystemClock.elapsedRealtime();
                if (now - lastUpdate[0] < 350 && bytes < expected)
                    return;
                lastUpdate[0] = now;
                progress = "Copying IPA for Emulator · " + formatBytes(bytes) + " / "
                    + formatBytes(expected);
                changed();
            });
    }

    static File copyRuntimeIpaAlias(File source, File alias, File temporary, File marker,
        long usableSpace, Runnable checkCancelled, java.util.function.LongConsumer onProgress)
        throws IOException {
        final long expected = source.length();
        final long sourceModified = source.lastModified();
        final long reserve = 32L * 1024 * 1024;
        final long required =
            expected > Long.MAX_VALUE - reserve ? Long.MAX_VALUE : Math.max(0, expected) + reserve;
        if (expected < 0 || usableSpace < required) {
            throw new IOException("Android could not reuse the IPA without making another copy. "
                + "The fallback copy needs " + formatBytes(required) + " free; only "
                + formatBytes(Math.max(0, usableSpace))
                + " is available. Free device storage and try again.");
        }
        if (java.nio.file.Files.isSymbolicLink(alias.toPath())
            || java.nio.file.Files.isSymbolicLink(temporary.toPath())
            || java.nio.file.Files.isSymbolicLink(marker.toPath()))
            throw new IOException("A symbolic link was found in the private IPA copy paths");
        java.nio.file.Files.deleteIfExists(alias.toPath());
        java.nio.file.Files.deleteIfExists(temporary.toPath());
        java.nio.file.Files.deleteIfExists(marker.toPath());

        long total = 0;
        long nextSpaceCheck = 0;
        try {
            try (InputStream input = new FileInputStream(source);
                FileOutputStream output = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[262144];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    checkCancelled.run();
                    if (total >= nextSpaceCheck) {
                        long available = temporary.getParentFile().getUsableSpace();
                        if (available < count + reserve)
                            throw new IOException("Free device storage fell below the 32 MiB "
                                + "safety reserve. Clear storage and retry.");
                        nextSpaceCheck = total + 4L * 1024 * 1024;
                    }
                    output.write(buffer, 0, count);
                    total += count;
                    onProgress.accept(total);
                }
                output.getFD().sync();
            }
            if (total != expected || source.length() != expected
                || source.lastModified() != sourceModified)
                throw new IOException("The IPA changed while its emulator copy was being made");
            try {
                if (!temporary.setLastModified(sourceModified))
                    Diagnostics.record(
                        "ANYIOSEMU_SOURCE_TIMESTAMP", "Could not preserve IPA timestamp");
            } catch (SecurityException e) {
                Diagnostics.record("ANYIOSEMU_SOURCE_TIMESTAMP", e.toString());
            }
            checkCancelled.run();
            if (!temporary.renameTo(alias))
                throw new IOException("Could not publish the private emulator IPA copy");
            try {
                writeRuntimeIpaMarker(marker, expected + "\n" + sourceModified + "\n");
            } catch (IOException e) {
                Diagnostics.record("ANYIOSEMU_SOURCE_MARKER", e.toString());
            }
            return alias;
        } catch (IOException e) {
            throw new IOException("Could not make an Emulator-readable IPA copy. Check available "
                    + "device storage and retry. " + e.getMessage(),
                e);
        } finally {
            if (temporary.exists() && !temporary.delete())
                Diagnostics.record("ANYIOSEMU_SOURCE_CLEANUP", "Temporary IPA copy remains");
        }
    }

    private static boolean runtimeIpaMarkerMatches(File marker, String fingerprint) {
        if (!marker.isFile())
            return false;
        try (InputStream input = new AtomicFile(marker).openRead()) {
            return fingerprint.equals(
                new String(GameInspector.bounded(input, 4096), StandardCharsets.UTF_8));
        } catch (Exception e) {
            return false;
        }
    }

    private static void writeRuntimeIpaMarker(File marker, String fingerprint) throws IOException {
        AtomicFile atomic = new AtomicFile(marker);
        FileOutputStream output = null;
        try {
            output = atomic.startWrite();
            output.write(fingerprint.getBytes(StandardCharsets.UTF_8));
            atomic.finishWrite(output);
        } catch (IOException e) {
            if (output != null)
                atomic.failWrite(output);
            throw e;
        }
    }

    /** Prepares the runtime IPA path, preferring a hard link and copying only when required. */
    void prepareAnyiOSEmu(Game game, java.util.function.Consumer<File> callback) {
        if (!game.canLaunchInAnyiOSEMU()) {
            main.post(() -> callback.accept(null));
            return;
        }
        if (!begin("Preparing the private AnyiOSEMU session…")) {
            main.post(() -> callback.accept(null));
            return;
        }
        cancellable = true;
        worker.execute(() -> {
            File runtimeSource = null;
            try {
                File canonicalRoot = root.getCanonicalFile();
                File canonicalGame = game.directory.getCanonicalFile();
                if (!canonicalGame.getParentFile().equals(canonicalRoot))
                    throw new IOException("Invalid library entry");
                checkCancelled();
                SandboxPaths sandbox = ensureSandbox(game.directory);
                File source = game.anyioseSource();
                if (!source.exists() || !source.canRead()
                    || !source.getCanonicalFile().toPath().startsWith(canonicalGame.toPath()))
                    throw new IOException("The private game source is missing or unreadable");
                if (game.kind.equals("IPA")) {
                    // Prefer a same-filesystem hard link; some Android devices forbid links in
                    // app-private storage, so copy only as a checked, cancellable fallback.
                    runtimeSource = prepareRuntimeIpaSource(game.directory, source);
                } else {
                    if (!source.isDirectory())
                        throw new IOException("The extracted .app bundle is missing");
                    runtimeSource = source;
                }
                ensurePrivateDirectory(sandbox.root, sandbox.anyiose);
                checkCancelled();
                LatestGameLog.prepare(sandbox.anyiose);
            } catch (CancellationException e) {
                runtimeSource = null;
                error = false;
                notice = "Emulator preparation cancelled. Your stored game file was not changed.";
            } catch (Exception | LinkageError e) {
                runtimeSource = null;
                error = true;
                notice = "Could not prepare AnyiOSEMU: "
                    + (e.getMessage() == null ? e.toString() : e.getMessage());
                Diagnostics.record("ANYIOSEMU_PREPARE_ERROR", e.toString());
            } finally {
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
                File ready = runtimeSource;
                main.post(() -> callback.accept(ready));
            }
        });
    }
    private static boolean bundleCacheValid(
        File bundle, File marker, String fingerprint, String executable) {
        try {
            if (!bundle.isDirectory() || !new File(bundle, "Info.plist").isFile()
                || !new File(bundle, executable).isFile()
                || !bundle.getCanonicalFile().getParentFile().equals(
                    bundle.getParentFile().getCanonicalFile()))
                return false;
            try (InputStream in = new AtomicFile(marker).openRead()) {
                return fingerprint.equals(
                    new String(GameInspector.bounded(in, 4096), StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            return false;
        }
    }
    /** Persists a launch-start or completed AnyiOSEMU runtime log with the shared game entry. */
    void recordAnyiOSEmuReport(String gameId, String report) {
        if (gameId == null || !gameId.matches("game-[0-9a-f-]{36}") || report == null)
            return;
        worker.execute(() -> {
            try {
                File directory = new File(root, gameId);
                if (!directory.getCanonicalFile().getParentFile().equals(root.getCanonicalFile())
                    || !directory.isDirectory())
                    throw new IOException("The game entry no longer exists");
                String bounded = report.length() > 100000
                    ? report.substring(0, 100000) + "\nReport shortened."
                    : report;
                String saved = "Recorded at " + new java.util.Date()
                    + "\nBuild=" + BuildConfig.SOURCE_REVISION + "\n" + bounded;
                JSONObject data = readMetadata(directory);
                data.put("lastLaunchReport", saved);
                writeMetadata(directory, data);
                refresh();
                boolean pending = report.contains("outcome pending");
                boolean completed = report.contains("AnyiOSEMU finished with status 0");
                error = report.contains("AnyiOSEMU errored:") || report.contains("PANIC in thread")
                    || report.contains("FATAL: native host crash")
                    || report.contains("FATAL host exception") || report.contains("FATAL SIGNAL")
                    || report.contains("Runtime log unavailable:")
                    || report.matches("(?s).*AnyiOSEMU finished with status (?!0\\b)[0-9]+.*");
                notice = pending
                    ? "AnyiOSEMU started. Its report will be updated when the session closes."
                    : error ? "AnyiOSEMU recorded an error. Open the latest log for details."
                    : completed
                    ? "AnyiOSEMU exited normally; that is not proof of visible rendering or gameplay."
                    : "AnyiOSEMU session ended. The diagnostic log was saved; visual results remain unverified.";
                Diagnostics.record("ANYIOSEMU_SESSION", gameId + " · " + notice);
            } catch (Exception e) {
                error = true;
                notice = "Could not save AnyiOSEMU report: " + e.getMessage();
                Diagnostics.record("ANYIOSEMU_REPORT_ERROR", e.toString());
            } finally {
                changed();
            }
        });
    }
    void attemptLaunch(Game game) {
        attemptLaunch(game, null);
    }
    /** Runs the guest until it stops; {@code onOutcome} receives the result on the UI thread. */
    boolean attemptLaunch(Game game, java.util.function.Consumer<LaunchResult> onOutcome) {
        if (!begin("Opening the game with the native loader…"))
            return false;
        launchCancelled = false;
        worker.execute(() -> {
            File scratch = new File(root, ".pending-launch-" + UUID.randomUUID());
            String outcome;
            byte[] guestFrame = null;
            int frameWidth = 0, frameHeight = 0;
            try {
                if (!game.directory.getCanonicalFile().getParentFile().equals(
                        root.getCanonicalFile()))
                    throw new IOException("Invalid library entry");
                mkdir(scratch);
                if (launchCancelled)
                    throw new CancellationException("Guest startup cancelled before analysis");
                File source =
                    new File(game.directory, game.kind.equals("Folder") ? "bundle" : "source.bin");
                SandboxPaths sandbox = ensureSandbox(game.directory);
                File bundleRoot = game.kind.equals("Folder") ? source : null;
                File bundleArchiveCache = null, bundleOverlay = null;
                if (game.kind.equals("IPA")) {
                    File legacyBundle = new File(game.directory, "bundle");
                    File legacyMarker = new File(game.directory, "bundle-cache");
                    String fingerprint = source.length() + "\\n" + source.lastModified() + "\\n"
                        + LibraryRules.fileName(game.executable);
                    if (bundleCacheValid(
                            legacyBundle, legacyMarker, fingerprint, game.executable)) {
                        // Keep saves/caches created by earlier versions alive. New imports skip
                        // this eager bundle and use the ZIP-backed read-through mount instead.
                        bundleRoot = legacyBundle;
                    } else {
                        bundleArchiveCache = sandbox.bundleCache;
                        bundleOverlay = sandbox.bundleOverlay;
                    }
                }
                if (launchCancelled)
                    throw new CancellationException(
                        "Guest startup cancelled before guest analysis");
                GameInspector.Result result = GameInspector.analyze(source, game.title, scratch,
                    true, bundleRoot, sandbox.documents, bundleArchiveCache, bundleOverlay);
                outcome = result.launchOutcome;
                guestFrame = result.guestFrame;
                frameWidth = result.guestFrameWidth;
                frameHeight = result.guestFrameHeight;
                if (outcome.isEmpty())
                    throw new IOException("No native launch result was returned");
            } catch (CancellationException e) {
                outcome = "LAUNCH_CANCELLED: " + e.getMessage() + "\nGuest code was not executed.";
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
                boolean captured = guestFrame != null && frameWidth > 0 && frameHeight > 0
                    && guestFrame.length == frameWidth * frameHeight * 4;
                boolean wasCancelled =
                    outcome.toLowerCase(java.util.Locale.ROOT).contains("cancel");
                error = !captured && !wasCancelled;
                notice = captured
                    ? "Guest framebuffer captured; check the displayed frame. Device appearance still needs visual review."
                    : wasCancelled
                    ? "Guest startup cancelled; temporary launch files were removed."
                    : "Startup stopped safely. The diagnostic report names the exact blocker.";
                if (onOutcome != null) {
                    LaunchResult delivered =
                        new LaunchResult(outcome, frameWidth, frameHeight, guestFrame);
                    main.post(() -> onOutcome.accept(delivered));
                }
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
                SharedCatalog.end();
                busy = false;
                progress = "";
                changed();
            }
        });
        return true;
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
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    void exportSandbox(Game game, Uri destination) {
        if (!begin("Preparing sandbox export…"))
            return;
        cancellable = true;
        worker.execute(() -> {
            try {
                if (!game.directory.getCanonicalFile().getParentFile().equals(
                        root.getCanonicalFile()))
                    throw new IOException("Invalid library entry");
                SandboxPaths sandbox = ensureSandbox(game.directory);
                OutputStream raw = context.getContentResolver().openOutputStream(destination, "wt");
                if (raw == null)
                    throw new IOException("The selected provider did not open the ZIP destination");
                try (ZipOutputStream zip = new ZipOutputStream(raw)) {
                    addSandboxDirectory(sandbox.root, sandbox.documents, "Documents/", zip, 0);
                    addSandboxDirectory(
                        sandbox.root, sandbox.bundleOverlay, "BundleOverrides/", zip, 0);
                    addSandboxDirectory(sandbox.root, sandbox.anyiose, "AnyiOSEMU/", zip, 0, true);
                    File importedBundle = new File(game.directory, "bundle");
                    if (game.kind.equals("Folder") || importedBundle.isDirectory())
                        addSandboxDirectory(game.directory, importedBundle, "Bundle/", zip, 0);
                }
                notice = "Game-data ZIP exported. It contains Documents, bundle overrides and "
                    + "AnyiOSEMU per-game data; rebuildable IPA/PVRTC caches are excluded. An "
                    + "imported .app copy is included when applicable.";
                error = false;
                Diagnostics.record("SANDBOX_EXPORT", game.title + " · " + formatBytes(copied));
            } catch (CancellationException e) {
                notice = "Sandbox export cancelled.";
            } catch (Exception e) {
                error = true;
                notice = "Sandbox export failed: " + e.getMessage();
                Diagnostics.record("SANDBOX_EXPORT_ERROR", e.toString());
            } finally {
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }
    /** Copy browsable per-game data into a user-selected shared Documents tree. */
    void exportSandboxFolder(Game game, Uri destinationTree) {
        if (!begin("Preparing folder export…"))
            return;
        boolean persistableGrant = false;
        if (destinationTree != null) {
            try {
                context.getContentResolver().takePersistableUriPermission(destinationTree,
                    android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                persistableGrant = true;
            } catch (Exception ignored) {
                // Some providers grant access only for the current activity/process session.
            }
        }
        final boolean releasePersistableGrant = persistableGrant;
        cancellable = true;
        worker.execute(() -> {
            ArrayList<Uri> createdDocuments = new ArrayList<>();
            try {
                if (destinationTree == null || destinationTree.getAuthority() == null)
                    throw new IOException("The selected folder is unavailable");
                if ((context.getPackageName() + ".provider").equals(destinationTree.getAuthority()))
                    throw new IOException(
                        "Choose a shared folder, not AnyiOSEMU’s private data provider");
                if (!game.directory.getCanonicalFile().getParentFile().equals(
                        root.getCanonicalFile()))
                    throw new IOException("Invalid library entry");
                SandboxPaths sandbox = ensureSandbox(game.directory);
                String rootId = DocumentsContract.getTreeDocumentId(destinationTree);
                Uri treeRoot = DocumentsContract.buildDocumentUriUsingTree(destinationTree, rootId);
                Uri anyiosDirectory =
                    findOrCreateDirectory(destinationTree, treeRoot, "org.anyios");
                String folderName = exportFolderName(game);
                Uri exportRoot = createExportDocument(destinationTree, anyiosDirectory,
                    DocumentsContract.Document.MIME_TYPE_DIR, folderName, createdDocuments);

                Uri documents = createExportDocument(destinationTree, exportRoot,
                    DocumentsContract.Document.MIME_TYPE_DIR, "Documents", createdDocuments);
                copySandboxDirectoryToFolder(sandbox.root, sandbox.documents, documents,
                    destinationTree, "Documents/", 0, false, createdDocuments);

                Uri overrides = createExportDocument(destinationTree, exportRoot,
                    DocumentsContract.Document.MIME_TYPE_DIR, "BundleOverrides", createdDocuments);
                copySandboxDirectoryToFolder(sandbox.root, sandbox.bundleOverlay, overrides,
                    destinationTree, "BundleOverrides/", 0, false, createdDocuments);

                Uri runtime = createExportDocument(destinationTree, exportRoot,
                    DocumentsContract.Document.MIME_TYPE_DIR, "AnyiOSEMU", createdDocuments);
                copySandboxDirectoryToFolder(sandbox.root, sandbox.anyiose, runtime,
                    destinationTree, "AnyiOSEMU/", 0, true, createdDocuments);

                File importedBundle = new File(game.directory, "bundle");
                if (game.kind.equals("Folder") || importedBundle.isDirectory()) {
                    Uri bundle = createExportDocument(destinationTree, exportRoot,
                        DocumentsContract.Document.MIME_TYPE_DIR, "Bundle", createdDocuments);
                    copySandboxDirectoryToFolder(game.directory, importedBundle, bundle,
                        destinationTree, "Bundle/", 0, false, createdDocuments);
                }
                notice = "Game data copied to org.anyios/" + folderName
                    + " in the selected location. Rebuildable IPA/PVRTC caches were excluded.";
                error = false;
                Diagnostics.record(
                    "SANDBOX_FOLDER_EXPORT", game.title + " · " + formatBytes(copied));
            } catch (CancellationException e) {
                deleteCreatedDocuments(createdDocuments);
                notice = "Folder export cancelled.";
            } catch (Exception e) {
                deleteCreatedDocuments(createdDocuments);
                error = true;
                notice = "Folder export failed: " + e.getMessage();
                Diagnostics.record("SANDBOX_FOLDER_EXPORT_ERROR", e.toString());
            } finally {
                if (releasePersistableGrant && destinationTree != null) {
                    try {
                        context.getContentResolver().releasePersistableUriPermission(
                            destinationTree,
                            android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    } catch (Exception ignored) {
                        // The provider may already have revoked this one-operation grant.
                    }
                }
                SharedCatalog.end();
                busy = false;
                cancellable = false;
                progress = "";
                changed();
            }
        });
    }

    private Uri findOrCreateDirectory(Uri tree, Uri parent, String name) throws IOException {
        String parentId = DocumentsContract.getDocumentId(parent);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] projection = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor =
                 context.getContentResolver().query(children, projection, null, null, null)) {
            if (cursor != null) {
                int idColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
                int nameColumn =
                    cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
                int mimeColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE);
                while (cursor.moveToNext()) {
                    if (idColumn >= 0 && nameColumn >= 0 && mimeColumn >= 0
                        && name.equals(cursor.getString(nameColumn))
                        && DocumentsContract.Document.MIME_TYPE_DIR.equals(
                            cursor.getString(mimeColumn)))
                        return DocumentsContract.buildDocumentUriUsingTree(
                            tree, cursor.getString(idColumn));
                }
            }
        } catch (Exception ignored) {
            // A provider may allow document creation without supporting child queries.
        }
        Uri created;
        try {
            created = DocumentsContract.createDocument(context.getContentResolver(), parent,
                DocumentsContract.Document.MIME_TYPE_DIR, name);
        } catch (Exception e) {
            throw new IOException("The selected location cannot create the org.anyios folder", e);
        }
        if (created == null)
            throw new IOException("The selected location did not create the org.anyios folder");
        return DocumentsContract.buildDocumentUriUsingTree(
            tree, DocumentsContract.getDocumentId(created));
    }

    private Uri createExportDocument(Uri tree, Uri parent, String mimeType, String name,
        ArrayList<Uri> createdDocuments) throws IOException {
        try {
            Uri created = DocumentsContract.createDocument(
                context.getContentResolver(), parent, mimeType, name);
            if (created == null)
                throw new IOException("The selected location refused to create " + name);
            Uri treeDocument = DocumentsContract.buildDocumentUriUsingTree(
                tree, DocumentsContract.getDocumentId(created));
            createdDocuments.add(treeDocument);
            return treeDocument;
        } catch (Exception e) {
            if (e instanceof IOException)
                throw (IOException) e;
            throw new IOException("The selected location refused to create " + name, e);
        }
    }

    private void copySandboxDirectoryToFolder(File sandboxRoot, File directory, Uri destination,
        Uri tree, String relativePath, int depth, boolean excludeRuntimeCaches,
        ArrayList<Uri> createdDocuments) throws Exception {
        checkCancelled();
        if (depth > MAX_SANDBOX_EXPORT_DEPTH || ++files > MAX_SANDBOX_EXPORT_FILES)
            throw new IOException("Game data exceeds the safe depth or 30,000-entry export limit");
        if (java.nio.file.Files.isSymbolicLink(directory.toPath()))
            throw new IOException("Folder export refused a symbolic-link directory");
        File canonicalRoot = sandboxRoot.getCanonicalFile();
        File canonicalDirectory = directory.getCanonicalFile();
        if (!canonicalDirectory.toPath().startsWith(canonicalRoot.toPath()))
            throw new IOException("Folder export directory escaped its private root");
        File[] children = directory.listFiles();
        if (children == null)
            throw new IOException("Could not read a game-data directory");
        java.util.Arrays.sort(children, Comparator.comparing(File::getName));
        byte[] buffer = new byte[65536];
        for (File child : children) {
            checkCancelled();
            if (excludeRuntimeCaches && child.getName().equals("AnyiOSEMU_pvrtc_cache"))
                continue;
            if (java.nio.file.Files.isSymbolicLink(child.toPath()))
                throw new IOException("Folder export refused a symbolic link");
            String name = relativePath + child.getName();
            if (child.isDirectory()) {
                Uri childDestination = createExportDocument(tree, destination,
                    DocumentsContract.Document.MIME_TYPE_DIR, child.getName(), createdDocuments);
                copySandboxDirectoryToFolder(sandboxRoot, child, childDestination, tree, name + "/",
                    depth + 1, excludeRuntimeCaches, createdDocuments);
                continue;
            }
            if (!child.isFile())
                continue;
            if (++files > MAX_SANDBOX_EXPORT_FILES)
                throw new IOException("Game data exceeds the 30,000-file export limit");
            File canonicalChild = child.getCanonicalFile();
            if (!canonicalChild.toPath().startsWith(canonicalRoot.toPath()))
                throw new IOException("Folder export file escaped its private root");
            String extension = child.getName().contains(".")
                ? child.getName()
                      .substring(child.getName().lastIndexOf('.') + 1)
                      .toLowerCase(java.util.Locale.ROOT)
                : "";
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
            if (mime == null)
                mime = "application/octet-stream";
            Uri output =
                createExportDocument(tree, destination, mime, child.getName(), createdDocuments);
            try (InputStream in = new FileInputStream(canonicalChild);
                OutputStream out = context.getContentResolver().openOutputStream(output, "wt")) {
                if (out == null)
                    throw new IOException("The selected provider did not open " + name);
                int count;
                while ((count = in.read(buffer)) != -1) {
                    checkCancelled();
                    copied = LibraryRules.addCopiedBytes(copied, count);
                    if (copied > MAX_SANDBOX_EXPORT_BYTES)
                        throw new IOException("Game-data export exceeds the 4 GiB safety limit");
                    out.write(buffer, 0, count);
                    tick(name);
                }
            }
        }
    }

    private void deleteCreatedDocuments(ArrayList<Uri> createdDocuments) {
        for (int i = createdDocuments.size() - 1; i >= 0; i--) {
            try {
                DocumentsContract.deleteDocument(
                    context.getContentResolver(), createdDocuments.get(i));
            } catch (Exception ignored) {
                // A provider may not support deleting partial exports; report the failure below.
            }
        }
    }

    private static String exportFolderName(Game game) {
        String safeTitle =
            game.title.replaceAll("[^A-Za-z0-9._-]+", "_").replaceAll("^\\.+|\\.+$", "");
        if (safeTitle.isEmpty())
            safeTitle = "game";
        if (safeTitle.length() > 64)
            safeTitle = safeTitle.substring(0, 64);
        return "AnyiOSEMU-" + safeTitle + "-" + UUID.randomUUID().toString().substring(0, 8);
    }

    private void addSandboxDirectory(File sandboxRoot, File directory, String archivePath,
        ZipOutputStream zip, int depth) throws IOException {
        addSandboxDirectory(sandboxRoot, directory, archivePath, zip, depth, false);
    }
    private void addSandboxDirectory(File sandboxRoot, File directory, String archivePath,
        ZipOutputStream zip, int depth, boolean excludeRuntimeCaches) throws IOException {
        checkCancelled();
        if (depth > MAX_SANDBOX_EXPORT_DEPTH || ++files > MAX_SANDBOX_EXPORT_FILES)
            throw new IOException("Game data exceeds the safe depth or 30,000-entry export limit");
        if (java.nio.file.Files.isSymbolicLink(directory.toPath()))
            throw new IOException("Sandbox export refused a symbolic-link directory");
        File canonicalRoot = sandboxRoot.getCanonicalFile();
        File canonicalDirectory = directory.getCanonicalFile();
        if (!canonicalDirectory.toPath().startsWith(canonicalRoot.toPath()))
            throw new IOException("Sandbox directory escaped its private root");
        zip.putNextEntry(new ZipEntry(archivePath));
        zip.closeEntry();
        File[] children = directory.listFiles();
        if (children == null)
            throw new IOException("Could not read a sandbox directory");
        java.util.Arrays.sort(children, Comparator.comparing(File::getName));
        byte[] buffer = new byte[65536];
        for (File child : children) {
            checkCancelled();
            if (excludeRuntimeCaches && child.getName().equals("AnyiOSEMU_pvrtc_cache"))
                continue;
            if (java.nio.file.Files.isSymbolicLink(child.toPath()))
                throw new IOException("Sandbox export refused a symbolic link");
            if (child.isDirectory()) {
                addSandboxDirectory(sandboxRoot, child, archivePath + child.getName() + "/", zip,
                    depth + 1, excludeRuntimeCaches);
                continue;
            }
            if (!child.isFile())
                continue;
            if (++files > MAX_SANDBOX_EXPORT_FILES)
                throw new IOException("Game data exceeds the 30,000-file export limit");
            File canonicalChild = child.getCanonicalFile();
            if (!canonicalChild.toPath().startsWith(canonicalRoot.toPath()))
                throw new IOException("Sandbox file escaped its private root");
            String name = archivePath + child.getName();
            zip.putNextEntry(new ZipEntry(name));
            try (InputStream in = new FileInputStream(canonicalChild)) {
                int count;
                while ((count = in.read(buffer)) != -1) {
                    checkCancelled();
                    copied = LibraryRules.addCopiedBytes(copied, count);
                    if (copied > MAX_SANDBOX_EXPORT_BYTES)
                        throw new IOException("Sandbox export exceeds the 4 GiB safety limit");
                    zip.write(buffer, 0, count);
                    tick(name);
                }
            }
            zip.closeEntry();
        }
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
                SharedCatalog.end();
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
    static void saveCover(File directory, byte[] encoded) {
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
            while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize
                > COVER_MAX_EDGE)
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
