package io.github.teamgdb.yakumo;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class DriverZipTest {
    @Rule public TemporaryFolder folder = new TemporaryFolder();

    private static byte[] zip(String[] names, byte[][] contents) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(bytes)) {
            for (int i = 0; i < names.length; ++i) {
                zip.putNextEntry(new ZipEntry(names[i]));
                zip.write(contents[i]);
                zip.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    private static byte[] text(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private String[] extract(byte[] zip) throws IOException {
        return DriverZip.extract(new ByteArrayInputStream(zip), folder.getRoot());
    }

    @Test
    public void extractsLibrariesAndMetaJsonFlattened() throws IOException {
        byte[] zip = zip(new String[] {"pkg/meta.json", "pkg/lib/vulkan.adreno.so", "pkg/libgsl.so"},
            new byte[][] {text("{}"), text("driver"), text("helper")});
        assertArrayEquals(new String[] {"vulkan.adreno.so", "libgsl.so"}, extract(zip));
        assertEquals("driver", new String(Files.readAllBytes(new File(folder.getRoot(), "vulkan.adreno.so").toPath()),
            StandardCharsets.UTF_8));
        assertTrue(new File(folder.getRoot(), "meta.json").isFile());
    }

    @Test
    public void skipsEverythingElse() throws IOException {
        byte[] zip = zip(new String[] {"readme.txt", "x/../../evil.sh", "a.so"},
            new byte[][] {text("hi"), text("rm"), text("so")});
        assertArrayEquals(new String[] {"a.so"}, extract(zip));
        String[] files = folder.getRoot().list();
        Arrays.sort(files);
        assertArrayEquals(new String[] {"a.so"}, files);
    }

    @Test
    public void firstEntryWinsWhenNamesCollideAfterFlattening() throws IOException {
        byte[] zip = zip(new String[] {"one/a.so", "two/a.so"}, new byte[][] {text("first"), text("second")});
        assertArrayEquals(new String[] {"a.so"}, extract(zip));
        assertEquals("first", new String(Files.readAllBytes(new File(folder.getRoot(), "a.so").toPath()),
            StandardCharsets.UTF_8));
    }

    @Test
    public void noLibraryGivesAnEmptyList() throws IOException {
        assertEquals(0, extract(zip(new String[] {"meta.json"}, new byte[][] {text("{}")})).length);
    }

    @Test
    public void notAZipGivesNoLibraries() throws IOException {
        assertEquals(0, extract(text("this is not a zip")).length);
    }

    @Test
    public void refusesTooManyEntries() throws IOException {
        String[] names = new String[DriverZip.MAX_ENTRIES + 1];
        byte[][] contents = new byte[names.length][];
        for (int i = 0; i < names.length; ++i) {
            names[i] = "f" + i + ".txt";
            contents[i] = new byte[0];
        }
        try {
            extract(zip(names, contents));
            fail("expected IOException");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("too many"));
        }
    }

    @Test
    public void refusesAZipThatUnpacksToTooMuch() throws IOException {
        byte[] block = new byte[1 << 20];
        int count = (int) (DriverZip.MAX_BYTES >> 20) + 1;
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream out = new ZipOutputStream(bytes)) {
            out.putNextEntry(new ZipEntry("big.so"));
            for (int i = 0; i < count; ++i) out.write(block); // zeros compress to a few hundred KB
            out.closeEntry();
        }
        assertFalse(bytes.size() > DriverZip.MAX_BYTES);
        try {
            extract(bytes.toByteArray());
            fail("expected IOException");
        } catch (IOException expected) {
            assertTrue(expected.getMessage().contains("too much"));
        }
    }
}
