package org.podnative;

import java.io.IOException;
import java.io.StringReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import javax.xml.parsers.DocumentBuilderFactory;
import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.xml.sax.InputSource;

/** Bounded metadata parser only; not an implementation of Apple's property-list APIs. */
final class Plist {
    private static final int LIMIT = 1024 * 1024;
    private Plist() {}
    static Map<String, Object> read(byte[] data) throws IOException {
        if (data.length > LIMIT)
            throw new IOException("Info.plist exceeds 1 MiB metadata limit");
        Object value;
        if (data.length >= 8
            && new String(data, 0, 8, StandardCharsets.US_ASCII).equals("bplist00")) {
            value = new Binary(data).root();
        } else {
            try {
                String xml;
                if (data.length >= 2 && ((data[0] & 255) == 0xff || (data[0] & 255) == 0xfe))
                    xml = new String(data, StandardCharsets.UTF_16);
                else
                    xml = new String(data, StandardCharsets.UTF_8);
                if (xml.contains("<!ENTITY"))
                    throw new IOException("Entity declarations are not permitted in metadata");
                // Bound nesting before building a DOM (Android's DOM parser recurses).
                var handler = new org.xml.sax.helpers.DefaultHandler() {
                    int depth, elements;
                    @Override
                    public void startElement(String uri, String local, String name,
                        org.xml.sax.Attributes attributes) throws org.xml.sax.SAXException {
                        if (++depth > 36 || ++elements > 50000)
                            throw new org.xml.sax.SAXException(
                                "Metadata nesting/object limit exceeded");
                    }
                    @Override
                    public void endElement(String uri, String local, String name) {
                        depth--;
                    }
                    @Override
                    public InputSource resolveEntity(String publicId, String systemId) {
                        return new InputSource(new StringReader(""));
                    }
                };
                javax.xml.parsers.SAXParserFactory.newInstance().newSAXParser().parse(
                    new InputSource(new StringReader(xml)), handler);
                DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
                factory.setExpandEntityReferences(false);
                var builder = factory.newDocumentBuilder();
                // Standard plist DOCTYPE is allowed, but external entities/DTDs are never fetched.
                builder.setEntityResolver(
                    (publicId, systemId) -> new InputSource(new StringReader("")));
                Element root =
                    builder.parse(new InputSource(new StringReader(xml))).getDocumentElement();
                if (!root.getTagName().equals("plist"))
                    throw new IOException("Expected plist root element");
                List<Element> children = children(root);
                if (children.size() != 1)
                    throw new IOException("Expected one plist root value");
                value = xmlValue(children.get(0), 0, new int[] {0});
            } catch (IOException e) {
                throw e;
            } catch (Exception e) {
                throw new IOException("Invalid XML Info.plist: " + e.getMessage(), e);
            }
        }
        if (!(value instanceof Map))
            throw new IOException("Info.plist root must be a dictionary");
        @SuppressWarnings("unchecked") Map<String, Object> map = (Map<String, Object>) value;
        return map;
    }
    private static List<Element> children(Element element) throws IOException {
        List<Element> result = new ArrayList<>();
        for (Node n = element.getFirstChild(); n != null; n = n.getNextSibling()) {
            if (n instanceof Element)
                result.add((Element) n);
            if (result.size() > 4096)
                throw new IOException("Metadata collection is too large");
        }
        return result;
    }
    private static Object xmlValue(Element e, int depth, int[] count) throws IOException {
        if (depth > 32 || ++count[0] > 50000)
            throw new IOException("Metadata nesting/object limit exceeded");
        switch (e.getTagName()) {
            case "dict": {
                List<Element> values = children(e);
                if (values.size() % 2 != 0)
                    throw new IOException("Unpaired dictionary key");
                Map<String, Object> map = new LinkedHashMap<>();
                for (int i = 0; i < values.size(); i += 2) {
                    if (!values.get(i).getTagName().equals("key"))
                        throw new IOException("Expected dictionary key");
                    String key = values.get(i).getTextContent();
                    if (map.containsKey(key))
                        throw new IOException("Duplicate metadata key");
                    map.put(key, xmlValue(values.get(i + 1), depth + 1, count));
                }
                return map;
            }
            case "array": {
                List<Object> list = new ArrayList<>();
                for (Element child : children(e)) list.add(xmlValue(child, depth + 1, count));
                return list;
            }
            case "true":
                return true;
            case "false":
                return false;
            case "string":
            case "date":
            case "data":
                return e.getTextContent();
            case "integer":
                try {
                    return Long.parseLong(e.getTextContent().trim());
                } catch (NumberFormatException ex) {
                    throw new IOException("Invalid metadata integer", ex);
                }
            case "real":
                try {
                    return Double.parseDouble(e.getTextContent().trim());
                } catch (NumberFormatException ex) {
                    throw new IOException("Invalid metadata real", ex);
                }
            default:
                throw new IOException("Unsupported metadata tag: " + e.getTagName());
        }
    }
    private static final class Binary {
        final byte[] bytes;
        final int offsetSize, refSize, objects, top;
        final long table;
        final Map<Integer, Object> cache = new HashMap<>();
        final Set<Integer> active = new HashSet<>();
        int work;
        long allocated;
        void budget(long bytes) throws IOException {
            if (bytes < 0 || bytes > 4L * LIMIT - allocated)
                throw new IOException("Decoded metadata allocation limit exceeded");
            allocated += bytes;
        }
        Binary(byte[] bytes) throws IOException {
            this.bytes = bytes;
            if (bytes.length < 40)
                throw new IOException("Truncated binary plist");
            int t = bytes.length - 32;
            offsetSize = bytes[t + 6] & 255;
            refSize = bytes[t + 7] & 255;
            if (offsetSize < 1 || offsetSize > 8 || refSize < 1 || refSize > 8)
                throw new IOException("Invalid binary plist field widths");
            long n = number(t + 8, 8), root = number(t + 16, 8);
            table = number(t + 24, 8);
            if (n < 1 || n > 50000 || root >= n || table < 8 || table > t
                || n * offsetSize > t - table)
                throw new IOException("Invalid binary plist trailer");
            objects = (int) n;
            top = (int) root;
        }
        long number(long p, int n) throws IOException {
            if (n < 1 || n > 8 || p < 0 || p > bytes.length || n > bytes.length - p)
                throw new IOException("Metadata integer out of bounds");
            long value = 0;
            for (int i = 0; i < n; i++) {
                if (i == 0 && n == 8 && (bytes[(int) p] & 128) != 0)
                    throw new IOException("Metadata offset/count exceeds signed range");
                value = value << 8 | bytes[(int) p + i] & 255;
            }
            return value;
        }
        Object root() throws IOException {
            return value(top, 0);
        }
        Object value(int id, int depth) throws IOException {
            if (id < 0 || id >= objects || depth > 32 || ++work > 100000)
                throw new IOException("Metadata reference/nesting limit exceeded");
            if (active.contains(id))
                throw new IOException("Cyclic metadata object reference");
            if (cache.containsKey(id))
                return cache.get(id);
            active.add(id);
            try {
                long p = number(table + (long) id * offsetSize, offsetSize);
                if (p < 8 || p >= table)
                    throw new IOException("Metadata object outside object table");
                int marker = bytes[(int) p++] & 255, type = marker >> 4, nibble = marker & 15;
                long count = nibble;
                if ((type == 4 || type == 5 || type == 6 || type == 10 || type == 13)
                    && nibble == 15) {
                    if (p >= table || (bytes[(int) p] & 0xf0) != 0x10)
                        throw new IOException("Invalid extended metadata length");
                    int width = 1 << (bytes[(int) p++] & 15);
                    if (width > 8)
                        throw new IOException("Metadata length width exceeds limit");
                    count = number(p, width);
                    p += width;
                }
                Object result;
                if (type == 0) {
                    if (nibble == 8)
                        result = false;
                    else if (nibble == 9)
                        result = true;
                    else if (nibble == 0)
                        result = null;
                    else
                        throw new IOException("Unsupported binary plist simple value");
                } else if (type == 1 || type == 8) {
                    int width = type == 8 ? nibble + 1 : 1 << nibble;
                    require(p, width);
                    if (type == 1 && width == 8)
                        result = ByteBuffer.wrap(bytes, (int) p, 8)
                                     .order(ByteOrder.BIG_ENDIAN)
                                     .getLong();
                    else
                        result = number(p, width);
                } else if (type == 2 || type == 3) {
                    int width = 1 << nibble;
                    if ((width != 4 && width != 8) || (type == 3 && width != 8))
                        throw new IOException("Invalid metadata real/date width");
                    require(p, width);
                    ByteBuffer b =
                        ByteBuffer.wrap(bytes, (int) p, width).order(ByteOrder.BIG_ENDIAN);
                    result = width == 4 ? (double) b.getFloat() : b.getDouble();
                } else if (type == 4 || type == 5 || type == 6) {
                    long size = type == 6 ? count * 2 : count;
                    if (count > LIMIT)
                        throw new IOException("Metadata scalar exceeds limit");
                    require(p, size);
                    budget(64 + size * 2);
                    if (type == 4)
                        result = java.util.Arrays.copyOfRange(bytes, (int) p, (int) (p + size));
                    else
                        result = new String(bytes, (int) p, (int) size,
                            type == 6 ? StandardCharsets.UTF_16BE : StandardCharsets.US_ASCII);
                } else if (type == 10 || type == 13) {
                    if (count > 2048)
                        throw new IOException("Metadata collection exceeds limit");
                    require(p, count * refSize * (type == 13 ? 2 : 1));
                    budget(64 + count * (type == 13 ? 96 : 16));
                    if (type == 10) {
                        List<Object> list = new ArrayList<>();
                        for (int i = 0; i < count; i++)
                            list.add(value(reference(p + (long) i * refSize), depth + 1));
                        result = list;
                    } else {
                        Map<String, Object> map = new LinkedHashMap<>();
                        for (int i = 0; i < count; i++) {
                            Object key = value(reference(p + (long) i * refSize), depth + 1);
                            if (!(key instanceof String) || map.containsKey(key))
                                throw new IOException("Invalid/duplicate metadata key");
                            map.put((String) key,
                                value(reference(p + (count + i) * refSize), depth + 1));
                        }
                        result = map;
                    }
                } else
                    throw new IOException("Unsupported binary plist object type " + type);
                cache.put(id, result);
                return result;
            } finally {
                active.remove(id);
            }
        }
        int reference(long p) throws IOException {
            long ref = number(p, refSize);
            if (ref >= objects)
                throw new IOException("Metadata object reference out of bounds");
            return (int) ref;
        }
        void require(long p, long size) throws IOException {
            if (p < 8 || size < 0 || p > table || size > table - p)
                throw new IOException("Metadata object bytes exceed object table");
        }
    }
}
