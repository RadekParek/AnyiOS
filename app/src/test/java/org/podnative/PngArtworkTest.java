package org.podnative;

import static org.junit.Assert.*;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.util.zip.Deflater;
import java.util.zip.InflaterInputStream;
import org.junit.Test;

public class PngArtworkTest {
    private void chunk(DataOutputStream out, String name, byte[] data) throws Exception {
        out.writeInt(data.length);
        out.writeBytes(name);
        out.write(data);
        out.writeInt(0);
    }
    private byte[] cgbi(int filter) throws Exception {
        return cgbi(filter, false);
    }
    private byte[] cgbi(int filter, boolean rgb) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeLong(0x89504e470d0a1a0aL);
        chunk(out, "CgBI", new byte[0]);
        ByteArrayOutputStream header = new ByteArrayOutputStream();
        DataOutputStream h = new DataOutputStream(header);
        h.writeInt(1);
        h.writeInt(1);
        h.write(new byte[] {8, (byte) (rgb ? 2 : 6), 0, 0, 0});
        chunk(out, "IHDR", header.toByteArray());
        byte[] pixels = rgb ? new byte[] {(byte) filter, 40, 80, 120}
                            : new byte[] {(byte) filter, 0, 0, (byte) 128, (byte) 128};
        Deflater deflater = new Deflater(6, true);
        deflater.setInput(pixels);
        deflater.finish();
        byte[] buf = new byte[100];
        int n = deflater.deflate(buf);
        deflater.end();
        chunk(out, "IDAT", java.util.Arrays.copyOf(buf, n));
        chunk(out, "IEND", new byte[0]);
        return bytes.toByteArray();
    }
    @Test
    public void convertsRawBgraAndUnpremultipliesForAllFilters() throws Exception {
        for (int variant = 0; variant < 10; variant++) {
            int filter = variant % 5;
            boolean rgb = variant >= 5;
            byte[] png = PngArtwork.normalize(cgbi(filter, rgb));
            var input = new java.io.DataInputStream(new ByteArrayInputStream(png));
            input.readLong();
            byte[] idat = null;
            while (input.available() > 0) {
                int n = input.readInt(), type = input.readInt();
                byte[] data = new byte[n];
                input.readFully(data);
                input.readInt();
                if (type == 0x49444154)
                    idat = data;
            }
            assertNotNull(idat);
            try (var inflater = new InflaterInputStream(new ByteArrayInputStream(idat))) {
                assertArrayEquals(
                    new byte[] {0, (byte) 255, 0, 0, (byte) 128}, inflater.readAllBytes());
            }
        }
    }
    @Test
    public void rejectsInvalidFilterAndLeavesOrdinaryDataAlone() throws Exception {
        byte[] data = {1, 2, 3};
        assertSame(data, PngArtwork.normalize(data));
        assertThrows(java.io.IOException.class, () -> PngArtwork.normalize(cgbi(5)));
        byte[] invalid = cgbi(0);
        invalid[8] = (byte) 0x7f;
        assertThrows(java.io.IOException.class, () -> PngArtwork.normalize(invalid));
    }
}
