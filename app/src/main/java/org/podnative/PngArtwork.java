package org.podnative;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.zip.CRC32;
import java.util.zip.DeflaterOutputStream;
import java.util.zip.Inflater;

/** Original bounded conversion of legacy iOS CgBI RGBA PNGs; no game assets bundled. */
final class PngArtwork {
    private PngArtwork() {}
    private static int integer(byte[] b, int p) {
        return (b[p] & 255) << 24 | (b[p + 1] & 255) << 16 | (b[p + 2] & 255) << 8 | b[p + 3] & 255;
    }
    static byte[] normalize(byte[] png) throws IOException {
        if (png.length < 8 || integer(png, 0) != 0x89504e47 || integer(png, 4) != 0x0d0a1a0a)
            return png;
        boolean cgbi = false;
        byte[] header = null;
        ByteArrayOutputStream compressed = new ByteArrayOutputStream();
        int p = 8, chunks = 0;
        while (p < png.length) {
            if (++chunks > 4096 || png.length - p < 12)
                throw new IOException("Invalid PNG chunk table");
            int size = integer(png, p), type = integer(png, p + 4);
            if (size < 0 || size > png.length - p - 12)
                throw new IOException("PNG chunk outside image");
            if (type == 0x43674249)
                cgbi = true;
            if (type == 0x49484452) {
                if (header != null || size != 13)
                    throw new IOException("Invalid PNG header");
                header = Arrays.copyOfRange(png, p + 8, p + 21);
            }
            if (type == 0x49444154)
                compressed.write(png, p + 8, size);
            p += size + 12;
            if (type == 0x49454e44)
                break;
        }
        if (!cgbi)
            return png;
        if (header == null)
            throw new IOException("CgBI PNG has no header");
        int w = integer(header, 0), h = integer(header, 4);
        if (w < 1 || h < 1 || w > 4096 || h > 4096 || (long) w * h * 4 > 16 * 1024 * 1024
            || header[8] != 8 || (header[9] != 6 && header[9] != 2) || header[10] != 0
            || header[11] != 0 || header[12] != 0)
            throw new IOException(
                "Unsupported CgBI format/dimensions (requires noninterlaced RGB/RGBA8)");
        int channels = header[9] == 6 ? 4 : 3;
        int stride = w * channels;
        header[9] = 6; // Normalized output is always standard RGBA8.
        byte[] filtered = new byte[(stride + 1) * h];
        Inflater inflater = new Inflater(true);
        try {
            inflater.setInput(compressed.toByteArray());
            int count = 0;
            while (count < filtered.length) {
                int n = inflater.inflate(filtered, count, filtered.length - count);
                if (n == 0)
                    break;
                count += n;
            }
            if (count == filtered.length && !inflater.finished()) {
                byte[] extra = new byte[1];
                if (inflater.inflate(extra) != 0)
                    throw new IOException("CgBI pixel data exceeds dimensions");
            }
            if (count != filtered.length || !inflater.finished() || inflater.getRemaining() != 0)
                throw new IOException("Invalid CgBI compressed pixel size");
        } catch (java.util.zip.DataFormatException e) {
            throw new IOException("Invalid CgBI deflate stream", e);
        } finally {
            inflater.end();
        }
        byte[] previous = new byte[stride], row = new byte[stride], rgbaRow = new byte[w * 4 + 1];
        ByteArrayOutputStream idat = new ByteArrayOutputStream();
        try (DeflaterOutputStream raw = new DeflaterOutputStream(idat)) {
            for (int y = 0; y < h; y++) {
                int filter = filtered[y * (stride + 1)] & 255;
                if (filter > 4)
                    throw new IOException("Unsupported PNG row filter");
                for (int x = 0; x < stride; x++) {
                    int a = x < channels ? 0 : row[x - channels] & 255, b = previous[x] & 255,
                        c = x < channels ? 0 : previous[x - channels] & 255;
                    int prediction = filter == 0 ? 0
                        : filter == 1            ? a
                        : filter == 2            ? b
                        : filter == 3            ? (a + b) / 2
                                                 : paeth(a, b, c);
                    row[x] = (byte) ((filtered[y * (stride + 1) + 1 + x] & 255) + prediction);
                }
                for (int x = 0; x < stride; x += channels) {
                    int alpha = channels == 4 ? row[x + 3] & 255 : 255;
                    int target = 1 + (x / channels) * 4;
                    rgbaRow[target] = (byte) unmultiply(row[x + 2] & 255, alpha);
                    rgbaRow[target + 1] = (byte) unmultiply(row[x + 1] & 255, alpha);
                    rgbaRow[target + 2] = (byte) unmultiply(row[x] & 255, alpha);
                    rgbaRow[target + 3] = (byte) alpha;
                }
                raw.write(rgbaRow);
                byte[] swap = previous;
                previous = row;
                row = swap;
            }
        }

        ByteArrayOutputStream output = new ByteArrayOutputStream();
        output.write(png, 0, 8);
        chunk(output, 0x49484452, header);
        chunk(output, 0x49444154, idat.toByteArray());
        chunk(output, 0x49454e44, new byte[0]);
        return output.toByteArray();
    }
    private static int unmultiply(int value, int alpha) {
        return alpha == 0 ? 0 : Math.min(255, (value * 255 + alpha / 2) / alpha);
    }
    private static int paeth(int a, int b, int c) {
        int p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }
    private static void chunk(ByteArrayOutputStream output, int type, byte[] bytes)
        throws IOException {
        DataOutputStream data = new DataOutputStream(output);
        data.writeInt(bytes.length);
        data.writeInt(type);
        data.write(bytes);
        CRC32 crc = new CRC32();
        for (int shift = 24; shift >= 0; shift -= 8) crc.update(type >>> shift & 255);
        crc.update(bytes);
        data.writeInt((int) crc.getValue());
    }
}
