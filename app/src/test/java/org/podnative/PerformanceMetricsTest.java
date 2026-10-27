package org.podnative;

import static org.junit.Assert.*;

import java.io.File;
import java.nio.file.Files;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class PerformanceMetricsTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void cpuIsNormalizedToAllAvailableCoresAndRejectsInvalidIntervals() {
        assertEquals(12.5, CpuUsage.percent(100, 1100, 1000, 2000, 8), 0.001);
        assertEquals(50.0, CpuUsage.percent(100, 4100, 1000, 2000, 8), 0.001);
        assertEquals(100.0, CpuUsage.percent(100, 9100, 1000, 2000, 8), 0.001);
        assertTrue(Double.isNaN(CpuUsage.percent(100, 200, 0, 1000, 8)));
        assertTrue(Double.isNaN(CpuUsage.percent(100, 50, 1000, 2000, 8)));
        assertTrue(Double.isNaN(CpuUsage.percent(100, 200, 1000, 1000, 8)));
        assertTrue(Double.isNaN(CpuUsage.percent(100, 200, 1000, 2000, 0)));
    }

    @Test
    public void parsesOnlyRecognizedGpuCounterFormats() {
        assertEquals(25.0, GpuUsageReader.parseBusyPair("250 1000\n"), 0.001);
        assertTrue(Double.isNaN(GpuUsageReader.parseBusyPair("0 0")));
        assertTrue(Double.isNaN(GpuUsageReader.parseBusyPair("200 100")));
        assertTrue(Double.isNaN(GpuUsageReader.parseBusyPair("garbage")));
        assertEquals(34.0, GpuUsageReader.parsePercent("34%"), 0.001);
        assertEquals(74.0, GpuUsageReader.parsePercent("74@490000000"), 0.001);
        assertTrue(Double.isNaN(GpuUsageReader.parsePercent("1000")));
        assertTrue(Double.isNaN(GpuUsageReader.parsePercent("-1")));
    }

    @Test
    public void readsKgslWindowsAndHandlesPowerCollapsedSamples() throws Exception {
        File sys = temporary.newFolder("sys");
        File node = new File(sys, "class/kgsl/kgsl-3d0/gpubusy");
        assertTrue(node.getParentFile().mkdirs());
        Files.writeString(node.toPath(), "0 0\n");
        GpuUsageReader reader = new GpuUsageReader(sys, temporary.newFolder("proc"));
        assertTrue(Double.isNaN(reader.sample(1000)));
        Files.writeString(node.toPath(), "250 1000\n");
        assertEquals(25.0, reader.sample(2000), 0.001);
        Files.writeString(node.toPath(), "0 0\n");
        assertEquals(25.0, reader.sample(3000), 0.001);
        assertTrue(Double.isNaN(reader.sample(8001)));
        Files.writeString(node.toPath(), "125 1000\n");
        assertEquals(12.5, reader.sample(8500), 0.001);
        assertTrue(node.delete());
        assertTrue(Double.isNaN(reader.sample(9000)));
    }

    @Test
    public void anEmptyKgslWindowDoesNotHideAUsableAlternative() throws Exception {
        File sys = temporary.newFolder("multiple");
        File kgsl = new File(sys, "class/kgsl/kgsl-3d0/gpubusy");
        File mali = new File(sys, "class/misc/mali0/device/utilization");
        assertTrue(kgsl.getParentFile().mkdirs());
        assertTrue(mali.getParentFile().mkdirs());
        Files.writeString(kgsl.toPath(), "0  0\n");
        Files.writeString(mali.toPath(), "81\n");
        GpuUsageReader reader = new GpuUsageReader(sys, temporary.newFolder("proc3"));
        assertEquals(81.0, reader.sample(1000), 0.001);
    }

    @Test
    public void usesReadableDeviceGpuNodesButNeverCpuDevfreq() throws Exception {
        File sys = temporary.newFolder("devfreq");
        File cpu = new File(sys, "class/devfreq/cpu0/load");
        File gpu = new File(sys, "class/devfreq/abc.mali/load");
        assertTrue(cpu.getParentFile().mkdirs());
        assertTrue(gpu.getParentFile().mkdirs());
        Files.writeString(cpu.toPath(), "99");
        Files.writeString(gpu.toPath(), "42@100000");
        GpuUsageReader reader = new GpuUsageReader(sys, temporary.newFolder("proc2"));
        assertEquals(42.0, reader.sample(1000), 0.001);
        assertTrue(gpu.delete());
        assertTrue(Double.isNaN(reader.sample(2000)));
        assertTrue(Double.isNaN(reader.sample(17000)));
    }
}
