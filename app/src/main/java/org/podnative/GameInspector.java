package org.podnative;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class GameInspector {
    private static final int MAX_EXECUTABLE = 64 * 1024 * 1024;
    private GameInspector() {}
    private static byte[] bounded(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        int read;
        while ((read = in.read(buffer)) != -1) {
            if (out.size() > MAX_EXECUTABLE - read)
                throw new IOException("Executable exceeds 64 MiB inspection limit");
            out.write(buffer, 0, read);
        }
        return out.toByteArray();
    }
    static String inspect(File file) throws IOException {
        try (FileInputStream in = new FileInputStream(file)) {
            int a = in.read(), b = in.read();
            if (a != 'P' || b != 'K') {
                try (FileInputStream raw = new FileInputStream(file)) {
                    return NativeBridge.inspect(bounded(raw));
                }
            }
        }
        try (ZipFile zip = new ZipFile(file)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            int count = 0;
            String result = null;
            while (entries.hasMoreElements()) {
                if (++count > 20000)
                    throw new IOException("Archive exceeds 20,000-entry limit");
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                // Inspect immediate files in an app bundle; never extract archive paths.
                if (entry.isDirectory() || !name.matches("Payload/[^/]+\\.app/[^/]+"))
                    continue;
                try (InputStream in = zip.getInputStream(entry)) {
                    byte[] magic = new byte[4];
                    int got = 0, r;
                    while (got < 4 && (r = in.read(magic, got, 4 - got)) > 0) got += r;
                    if (got == 4 && (magic[0] & 255) == 0xce && (magic[1] & 255) == 0xfa
                        && (magic[2] & 255) == 0xed && (magic[3] & 255) == 0xfe) {
                        if (result != null)
                            throw new IOException(
                                "Multiple ARM32 candidates; import the exact executable instead");
                        try (InputStream exe = zip.getInputStream(entry)) {
                            result = NativeBridge.inspect(bounded(exe));
                        }
                    }
                }
            }
            return result != null ? result
                                  : "No thin ARM32 Mach-O found. Fat/ARM64 archives are not "
                    + "supported by this inspector.";
        }
    }
}
