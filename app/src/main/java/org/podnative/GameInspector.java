package org.podnative;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Reads metadata and the selected ARM32 slice; complete source archives/resources are retained. */
final class GameInspector {
    private static final int MAX_EXECUTABLE = 64 * 1024 * 1024;
    private static final int MAX_ARTWORK_BYTES = 16 * 1024 * 1024;
    private static final long MAX_CONTAINER = 512L * 1024 * 1024;
    private static final AtomicBoolean guestFrameErrorReported = new AtomicBoolean();
    static final class Result {
        String kind = "Executable";
        String title, version = "Unknown version", bundleId = "", executable = "", report = "";
        String architecture = "Unknown", status = "Imported", metadataWarning = "",
               blockReason = "";
        byte[] icon;
        final List<byte[]> splashImages = new ArrayList<>();
        boolean launchAttempt;
        String launchOutcome = "";
        int guestFrameWidth, guestFrameHeight;
        byte[] guestFrame;
        File bundleRoot, containerRoot, bundleArchive, bundleCacheRoot, bundleOverlayRoot;
        String bundleArchivePrefix = "";
    }
    static final class GuestFrame {
        final int width, height, generation, rgbaOffset;
        final byte[] rgba;
        GuestFrame(int width, int height, int generation, byte[] rgba, int rgbaOffset) {
            this.width = width;
            this.height = height;
            this.generation = generation;
            this.rgba = rgba;
            this.rgbaOffset = rgbaOffset;
        }
    }
    private GameInspector() {}
    static byte[] bounded(InputStream in, int limit) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        int read;
        while ((read = in.read(buffer)) != -1) {
            if (out.size() > limit - read)
                throw new IOException("File exceeds the " + limit / 1024 + " KiB safety limit");
            out.write(buffer, 0, read);
        }
        return out.toByteArray();
    }
    static String inspect(File source) throws IOException {
        return analyze(source, source.getName(), source.getParentFile()).report;
    }
    static Result analyze(File source, String displayName, File scratch) throws IOException {
        return analyze(source, displayName, scratch, false);
    }
    static Result analyze(File source, String displayName, File scratch, boolean launchAttempt)
        throws IOException {
        return analyze(source, displayName, scratch, launchAttempt, null, null);
    }
    /**
     * @param bundleRoot extracted .app directory mounted at the guest's /var/mobile, when one
     *     already exists from an older release or the user imported an extracted folder.
     * @param containerRoot writable directory that becomes the guest's /var/mobile/Documents.
     * @param bundleCacheRoot on-demand materialized IPA resources, used only for ZIP-backed apps.
     * @param bundleOverlayRoot copy-on-write bundle files created by guest code.
     */
    static Result analyze(File source, String displayName, File scratch, boolean launchAttempt,
        File bundleRoot, File containerRoot) throws IOException {
        return analyze(
            source, displayName, scratch, launchAttempt, bundleRoot, containerRoot, null, null);
    }
    static Result analyze(File source, String displayName, File scratch, boolean launchAttempt,
        File bundleRoot, File containerRoot, File bundleCacheRoot, File bundleOverlayRoot)
        throws IOException {
        Result result = new Result();
        result.launchAttempt = launchAttempt;
        result.bundleRoot = bundleRoot;
        result.containerRoot = containerRoot;
        result.bundleCacheRoot = bundleCacheRoot;
        result.bundleOverlayRoot = bundleOverlayRoot;
        result.title = LibraryRules.title(displayName);
        if (displayName.toLowerCase(java.util.Locale.ROOT).endsWith(".ipa"))
            result.kind = "IPA";
        try {
            if (source.isDirectory())
                analyzeFolder(source, result);
            else {
                boolean archive;
                try (RandomAccessFile input = new RandomAccessFile(source, "r")) {
                    archive = input.length() >= 4 && input.readInt() == 0x504b0304;
                }
                if (archive)
                    analyzeArchive(source, scratch, result);
                else {
                    inspectExecutable(source, result);
                    result.executable = displayName;
                    if (!result.status.equals("Encrypted"))
                        result.status = "Executable only";
                    result.report +=
                        "\nResources: executable-only import; app bundle assets are not present.";
                }
            }
        } catch (IOException | IllegalStateException e) {
            result.blockReason = e.getMessage() == null ? e.toString() : e.getMessage();
            result.status = result.architecture.equals("ARM64")
                    || result.architecture.equals("ARM64_32")
                    || result.architecture.equals("64-bit")
                    || result.blockReason.contains("FAT_NO_ARM32")
                    || result.blockReason.contains("MACHO_CPU_SUBTYPE")
                ? "Unsupported"
                : "Invalid";
            result.report += "\nIMPORT_RETAINED: Original private copy preserved. Cannot launch: "
                + result.blockReason;
            result.launchOutcome =
                "LAUNCH_BLOCKED: " + result.blockReason + "\nGuest code was not executed.";
        }
        if (!result.metadataWarning.isEmpty())
            result.report += "\nMetadata note: " + result.metadataWarning;
        if (result.report.length() > 128000)
            result.report = result.report.substring(0, 128000)
                + "\nReport output limited to 128,000 characters.";
        return result;
    }
    private static Map<String, Object> metadata(byte[] data, Result result) {
        try {
            return Plist.read(data);
        } catch (IOException e) {
            result.metadataWarning = e.getMessage();
            return Collections.emptyMap();
        }
    }
    private static void applyMetadata(Map<String, Object> info, Result r) {
        r.title = LibraryRules.metadataText(info.get("CFBundleDisplayName"),
            LibraryRules.metadataText(info.get("CFBundleName"), r.title));
        r.bundleId = LibraryRules.metadataText(info.get("CFBundleIdentifier"), "");
        String version = LibraryRules.metadataText(info.get("CFBundleShortVersionString"), "");
        String build = LibraryRules.metadataText(info.get("CFBundleVersion"), "");
        r.version = !version.isEmpty()
            ? version + (!build.isEmpty() && !build.equals(version) ? " (" + build + ")" : "")
            : build.isEmpty() ? "Unknown version"
                              : build;
    }
    private static String topLevelAppRoot(String entry) {
        if (!entry.startsWith("Payload/"))
            return null;
        int appEnd = entry.indexOf('/', "Payload/".length());
        if (appEnd < 0 || appEnd == "Payload/".length())
            return null;
        String appName = entry.substring("Payload/".length(), appEnd);
        return appName.endsWith(".app") && appName.length() > 4 && appEnd + 1 < entry.length()
            ? entry.substring(0, appEnd + 1)
            : null;
    }
    static final class ArchiveEntries {
        final String bundlePrefix;
        final Set<String> names;
        ArchiveEntries(String bundlePrefix, Set<String> names) {
            this.bundlePrefix = bundlePrefix;
            this.names = names;
        }
    }
    static ArchiveEntries listArchiveEntries(ZipFile zip) throws IOException {
        Set<String> names = new HashSet<>();
        Set<String> bundles = new LinkedHashSet<>();
        Enumeration<? extends ZipEntry> entries = zip.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            String name = entry.getName();
            if (!names.add(name))
                throw new IOException("IPA contains duplicate entry names");
            if (names.size() > 30000)
                throw new IOException("IPA exceeds the 30,000-entry limit");
            if (!entry.isDirectory()) {
                String appRoot = topLevelAppRoot(name);
                if (appRoot != null)
                    bundles.add(appRoot);
            }
        }
        if (bundles.size() != 1)
            throw new IOException(
                "Choose an IPA with one top-level app in Payload, or import the exact .app folder");
        return new ArchiveEntries(bundles.iterator().next(), names);
    }
    private static boolean executableMagic(InputStream in) throws IOException {
        byte[] b = new byte[4];
        int n = 0, read;
        while (n < 4 && (read = in.read(b, n, 4 - n)) > 0) n += read;
        if (n != 4)
            return false;
        long magic = 0;
        for (byte value : b) magic = magic << 8 | value & 255;
        return magic == 0xcffaedfeL || magic == 0xcefaedfeL || magic == 0xcafebabeL
            || magic == 0xbebafecaL || magic == 0xcafebabfL || magic == 0xbfbafecaL;
    }
    private static void inspectExecutable(File executable, Result result) throws IOException {
        try (RandomAccessFile file = new RandomAccessFile(executable, "r")) {
            if (file.length() > MAX_CONTAINER)
                throw new IOException(
                    "Executable container exceeds 512 MiB; import a smaller supported ARM32 bundle");
            byte[] header = new byte[(int) Math.min(4096, file.length())];
            file.readFully(header);
            if (header.length >= 8 && (header[0] & 255) == 0xcf && (header[1] & 255) == 0xfa
                && (header[2] & 255) == 0xed && (header[3] & 255) == 0xfe) {
                int cpuType = (header[4] & 255) | (header[5] & 255) << 8 | (header[6] & 255) << 16
                    | (header[7] & 255) << 24;
                result.architecture = cpuType == 0x0100000c ? "ARM64"
                    : cpuType == 0x0200000c                 ? "ARM64_32"
                                                            : "64-bit";
                throw new IOException(result.architecture
                    + " executable retained, but this ARM32 runtime cannot execute it");
            }
            long[] slice;
            try {
                slice = NativeBridge.arm32Slice(header, file.length());
            } catch (IllegalStateException e) {
                if (e.getMessage() != null && e.getMessage().contains("FAT_NO_ARM32"))
                    result.architecture = unsupportedUniversalArchitecture(e.getMessage());
                throw new IOException(e.getMessage(), e);
            }
            if (slice[1] > MAX_EXECUTABLE)
                throw new IOException("Selected ARM32 slice exceeds the 64 MiB inspection limit");
            byte[] bytes = new byte[(int) slice[1]];
            file.seek(slice[0]);
            file.readFully(bytes);
            String report = NativeBridge.inspect(bytes);
            if (!report.startsWith("INSPECTED_ARM32:") && !report.startsWith("MACHO_ENCRYPTED:"))
                throw new IOException("Executable validation failed: " + report);
            if (bytes.length < 28 || (bytes[0] & 255) != 0xce || (bytes[1] & 255) != 0xfa
                || (bytes[2] & 255) != 0xed || (bytes[3] & 255) != 0xfe)
                throw new IOException("Selected slice is not a little-endian 32-bit Mach-O");
            long subtype = (bytes[8] & 255L) | (bytes[9] & 255L) << 8 | (bytes[10] & 255L) << 16
                | (bytes[11] & 255L) << 24;
            if (subtype != slice[2])
                throw new IOException("Architecture table and ARM32 slice subtype disagree");
            result.architecture = (subtype & 0xffffff) == 9 ? "ARMv7"
                : (subtype & 0xffffff) == 6                 ? "ARMv6"
                : (subtype & 0xffffff) == 11                ? "ARMv7s"
                : (subtype & 0xffffff) == 10                ? "ARMv7f"
                                                            : "ARM32";
            if (slice[3] != 0) {
                String selectedArchitecture = result.architecture;
                String slices = universalArchitectureSlices(slice);
                if (slice[4] != 0 || slice[5] != 0)
                    result.architecture =
                        slices + " universal (ARM32 selected: " + selectedArchitecture + ")";
                else
                    result.architecture =
                        selectedArchitecture + " · ARM32 universal (no ARM64 slice detected)";
            } else {
                result.architecture += " · ARM32";
            }
            if (report.startsWith("MACHO_ENCRYPTED:"))
                result.status = "Encrypted";
            if (result.launchAttempt) {
                boolean archiveMount = result.bundleRoot == null && result.bundleArchive != null
                    && result.bundleCacheRoot != null && result.bundleOverlayRoot != null
                    && result.containerRoot != null;
                result.launchOutcome = NativeBridge.attemptLaunch(bytes,
                    result.bundleRoot == null ? "" : result.bundleRoot.getAbsolutePath(),
                    result.containerRoot == null ? "" : result.containerRoot.getAbsolutePath(),
                    archiveMount ? result.bundleArchive.getAbsolutePath() : "",
                    archiveMount ? result.bundleArchivePrefix : "",
                    archiveMount ? result.bundleCacheRoot.getAbsolutePath() : "",
                    archiveMount ? result.bundleOverlayRoot.getAbsolutePath() : "");
                takeGuestFrame(result);
                report += "\n\n" + result.launchOutcome;
            }
            String container = slice[3] == 0
                ? "thin ARM32"
                : "universal " + universalArchitectureSlices(slice) + "; ARM32 selected"
                    + (slice[4] == 0 && slice[5] == 0 ? "; no ARM64 slice detected" : "");
            result.report = "Container: " + container + "\nSlice offset=" + slice[0]
                + " size=" + slice[1] + " bytes\n\n" + report;
        }
    }
    private static void takeGuestFrame(Result result) {
        GuestFrame frame = pollGuestFrame(-1);
        if (frame == null)
            return;
        result.guestFrameWidth = frame.width;
        result.guestFrameHeight = frame.height;
        result.guestFrame =
            java.util.Arrays.copyOfRange(frame.rgba, frame.rgbaOffset, frame.rgba.length);
    }
    static GuestFrame pollGuestFrame(int afterGeneration) {
        try {
            byte[] packed = NativeBridge.takeGuestFrame(afterGeneration);
            if (packed == null || packed.length < 12)
                return null;
            int width = readFrameInt(packed, 0);
            int height = readFrameInt(packed, 4);
            int generation = readFrameInt(packed, 8);
            long expected = 12L + (long) width * height * 4;
            if (width < 1 || height < 1 || width > 2048 || height > 2048 || generation <= 0
                || expected != packed.length) {
                reportGuestFrameError("ignored invalid frame dimensions/length");
                return null;
            }
            return new GuestFrame(width, height, generation, packed, 12);
        } catch (RuntimeException | LinkageError e) {
            reportGuestFrameError("readback transfer failed: " + e);
            return null;
        }
    }
    static void resetGuestFrameReporting() {
        guestFrameErrorReported.set(false);
    }
    private static void reportGuestFrameError(String message) {
        if (guestFrameErrorReported.compareAndSet(false, true))
            Diagnostics.record("GUEST_FRAME", message);
    }
    private static int readFrameInt(byte[] bytes, int offset) {
        return (bytes[offset] & 255) | (bytes[offset + 1] & 255) << 8
            | (bytes[offset + 2] & 255) << 16 | (bytes[offset + 3] & 255) << 24;
    }
    private static void analyzeArchive(File archive, File scratch, Result result)
        throws IOException {
        result.kind = "IPA";
        try (ZipFile zip = new ZipFile(archive)) {
            ArchiveEntries archiveEntries = listArchiveEntries(zip);
            Set<String> names = archiveEntries.names;
            String root = archiveEntries.bundlePrefix;
            result.bundleArchive = archive;
            result.bundleArchivePrefix = root;
            Map<String, Object> info = Collections.emptyMap();
            ZipEntry plist = zip.getEntry(root + "Info.plist");
            if (plist != null)
                try (InputStream in = zip.getInputStream(plist)) {
                    info = metadata(bounded(in, 1024 * 1024), result);
                }
            applyMetadata(info, result);
            Set<String> artwork = new HashSet<>();
            for (String name : names)
                if (name.startsWith(root))
                    artwork.add(name.substring(root.length()));
            for (String name : LaunchImages.candidates(info, artwork)) {
                if (result.splashImages.size() == 8)
                    break;
                ZipEntry image = zip.getEntry(root + name);
                if (image == null || image.isDirectory())
                    continue;
                try (InputStream in = zip.getInputStream(image)) {
                    result.splashImages.add(bounded(in, 4 * 1024 * 1024));
                } catch (IOException ignored) { /* Optional, bounded artwork. */
                }
            }

            for (String candidate : iconCandidates(info)) {
                ZipEntry icon = zip.getEntry(root + candidate);
                if (icon == null || icon.isDirectory())
                    continue;
                try (InputStream in = zip.getInputStream(icon)) {
                    result.icon = bounded(in, MAX_ARTWORK_BYTES);
                    break;
                } catch (
                    IOException ignored) { /* Optional artwork never makes the executable valid. */
                }
            }
            String declared = LibraryRules.metadataText(info.get("CFBundleExecutable"), "");
            String chosen = null;
            if (!declared.isEmpty()) {
                LibraryRules.fileName(declared);
                if (zip.getEntry(root + declared) == null)
                    throw new IOException(
                        "Info.plist names an executable that is missing from the IPA");
                chosen = root + declared;
            } else {
                for (String name : names) {
                    if (!name.startsWith(root) || name.substring(root.length()).contains("/"))
                        continue;
                    ZipEntry entry = zip.getEntry(name);
                    if (entry.isDirectory())
                        continue;
                    try (InputStream in = zip.getInputStream(entry)) {
                        if (executableMagic(in)) {
                            if (chosen != null)
                                throw new IOException(
                                    "Multiple executable candidates; select a bundle with valid CFBundleExecutable metadata");
                            chosen = name;
                        }
                    }
                }
            }
            if (chosen == null)
                throw new IOException("No supported Mach-O executable found inside the app bundle");
            File temporary = File.createTempFile("inspect-", ".macho", scratch);
            try {
                try (InputStream in = zip.getInputStream(zip.getEntry(chosen));
                    FileOutputStream out = new FileOutputStream(temporary)) {
                    byte[] buffer = new byte[65536];
                    long total = 0;
                    int n;
                    while ((n = in.read(buffer)) != -1) {
                        total += n;
                        if (total > MAX_CONTAINER)
                            throw new IOException("Executable container exceeds 512 MiB");
                        out.write(buffer, 0, n);
                    }
                }
                inspectExecutable(temporary, result);
            } finally {
                if (!temporary.delete())
                    Diagnostics.record(
                        "STORAGE_CLEANUP", "Inspection temporary file could not be removed");
            }
            result.executable = chosen.substring(root.length());
        }
    }
    private static void findBundles(File dir, int depth, List<File> found) {
        if (depth > 4 || found.size() > 1)
            return;
        if (new File(dir, "Info.plist").isFile()) {
            found.add(dir);
            return;
        }
        File[] files = dir.listFiles();
        if (files != null)
            for (File file : files)
                if (file.isDirectory() && !file.getName().equals("Frameworks")
                    && !file.getName().equals("PlugIns"))
                    findBundles(file, depth + 1, found);
    }
    private static void analyzeFolder(File folder, Result result) throws IOException {
        result.kind = "Folder";
        List<File> bundles = new ArrayList<>();
        findBundles(folder, 0, bundles);
        if (bundles.size() > 1)
            throw new IOException(
                "Several app bundles were found. Select the game's exact .app folder");
        File bundle = bundles.isEmpty() ? folder : bundles.get(0);
        Map<String, Object> info = Collections.emptyMap();
        File plist = new File(bundle, "Info.plist");
        if (plist.isFile())
            try (InputStream in = new FileInputStream(plist)) {
                info = metadata(bounded(in, 1024 * 1024), result);
            }
        applyMetadata(info, result);
        Set<String> artwork = new HashSet<>();
        File[] imageFiles = bundle.listFiles();
        if (imageFiles != null)
            for (File image : imageFiles)
                if (image.isFile())
                    artwork.add(image.getName());
        for (String name : LaunchImages.candidates(info, artwork)) {
            if (result.splashImages.size() == 8)
                break;
            try (InputStream in = new FileInputStream(new File(bundle, name))) {
                result.splashImages.add(bounded(in, 4 * 1024 * 1024));
            } catch (IOException ignored) { /* Optional, bounded artwork. */
            }
        }

        for (String name : iconCandidates(info)) {
            File icon = new File(bundle, name);
            if (icon.isFile())
                try (InputStream in = new FileInputStream(icon)) {
                    result.icon = bounded(in, MAX_ARTWORK_BYTES);
                    break;
                } catch (IOException ignored) { /* Optional cover. */
                }
        }
        String declared = LibraryRules.metadataText(info.get("CFBundleExecutable"), "");
        File executable = null;
        if (!declared.isEmpty()) {
            executable = new File(bundle, LibraryRules.fileName(declared));
            if (!executable.isFile())
                throw new IOException(
                    "The executable named by Info.plist is missing from the selected folder");
        } else {
            File[] files = bundle.listFiles();
            if (files != null)
                for (File candidate : files)
                    if (candidate.isFile()) {
                        try (InputStream in = new FileInputStream(candidate)) {
                            if (executableMagic(in)) {
                                if (executable != null)
                                    throw new IOException(
                                        "Multiple executables found. Select the exact .app folder with Info.plist");
                                executable = candidate;
                            }
                        }
                    }
        }
        if (executable == null)
            throw new IOException(
                "No Mach-O executable found. Select the extracted .app folder, not a folder of IPA downloads");
        inspectExecutable(executable, result);
        result.executable = folder.toPath().relativize(executable.toPath()).toString();
    }
    static String universalArchitectureSlices(long[] slice) {
        StringBuilder architectures = new StringBuilder();
        if (slice[4] != 0)
            architectures.append("ARM64");
        if (slice[5] != 0) {
            if (architectures.length() > 0)
                architectures.append(" + ");
            architectures.append("ARM64_32");
        }
        if (architectures.length() > 0)
            architectures.append(" + ");
        architectures.append("ARM32");
        return architectures.toString();
    }
    private static String unsupportedUniversalArchitecture(String error) {
        String marker = "universal binary contains ";
        String endMarker = " but no supported ARM32 slice";
        int start = error.indexOf(marker);
        int end = start < 0 ? -1 : error.indexOf(endMarker, start + marker.length());
        if (end < 0)
            return "Universal (no ARM32 slice)";
        return error.substring(start + marker.length(), end)
            + " · universal (no supported ARM32 slice)";
    }
    @SuppressWarnings("unchecked")
    private static List<String> iconCandidates(Map<String, Object> info) {
        List<String> base = new ArrayList<>();
        Object value = info.get("CFBundleIconFiles");
        Object icons = info.get("CFBundleIcons");
        if (icons instanceof Map) {
            Object primary = ((Map<String, Object>) icons).get("CFBundlePrimaryIcon");
            if (primary instanceof Map)
                value = ((Map<String, Object>) primary).get("CFBundleIconFiles");
        }
        if (value instanceof List)
            for (Object name : (List<?>) value)
                if (name instanceof String)
                    base.add((String) name);
        Object single = info.get("CFBundleIconFile");
        if (single instanceof String)
            base.add((String) single);
        Collections.reverse(base);
        base.add("Icon");
        base.add("Icon-72");
        base.add("Icon-Small");
        base.add("AppIcon60x60");
        base.add("AppIcon");
        List<String> result = new ArrayList<>();
        for (String name : base) {
            try {
                LibraryRules.fileName(name);
            } catch (IOException e) {
                continue;
            }
            String stem = name.endsWith(".png") ? name.substring(0, name.length() - 4) : name;
            result.add(stem + "@3x.png");
            result.add(stem + "@2x.png");
            result.add(stem + ".png");
        }
        return result;
    }
}
