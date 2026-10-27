package org.podnative;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class DynamicResolutionTest {
    @Test public void offNeverScalesAndInvalidSamplesAreIgnored() {
        DynamicResolution off = new DynamicResolution(100, 120);
        for (int i = 1; i <= 20; i++) off.sample(20, i * 1000L);
        assertEquals(100, off.percent());
        DynamicResolution on = new DynamicResolution(50, 60);
        on.sample(Double.NaN, 10000);
        on.sample(0, 11000);
        assertEquals(100, on.percent());
    }

    @Test public void slowFramesScaleWithinTheUserLimitAndProbeForRealGains() {
        DynamicResolution dynamic = new DynamicResolution(75, 60);
        for (int i = 4; i <= 6; i++) dynamic.sample(40, i * 1000L);
        assertEquals(90, dynamic.percent());
        for (int i = 10; i <= 12; i++) dynamic.sample(45, i * 1000L);
        assertEquals(80, dynamic.percent());
        for (int i = 16; i <= 18; i++) dynamic.sample(50, i * 1000L);
        assertEquals(75, dynamic.percent());
        for (int i = 22; i <= 40; i++) dynamic.sample(60, i * 1000L);
        assertEquals(80, dynamic.percent());
    }

    @Test public void cpuBoundOrLockedFramesRestoreQualityInsteadOfStayingBlurry() {
        DynamicResolution dynamic = new DynamicResolution(50, 60);
        for (int i = 4; i <= 20; i++) dynamic.sample(30, i * 1000L);
        assertEquals(100, dynamic.percent());
    }

    @Test public void highRefreshTargetRequiresActualHighFpsNotJustA120HzDisplay() {
        DynamicResolution dynamic = new DynamicResolution(50, 120);
        for (int i = 4; i <= 15; i++) dynamic.sample(60, i * 1000L);
        assertEquals(60.0, dynamic.target(), 0.001);
        for (int i = 16; i <= 18; i++) dynamic.sample(90, i * 1000L);
        assertEquals(120.0, dynamic.target(), 0.001);
    }

    @Test public void sizeIsAspectPreservingAndNeverUpscales() {
        assertArrayEquals(new int[] {1080, 2400}, SurfaceResolution.dimensions(1440, 3200, 1080, 100));
        assertArrayEquals(new int[] {810, 1800}, SurfaceResolution.dimensions(1440, 3200, 1080, 75));
        assertArrayEquals(new int[] {720, 1280}, SurfaceResolution.dimensions(720, 1280, 1440, 100));
        assertArrayEquals(new int[] {0, 0}, SurfaceResolution.dimensions(0, 0, 1080, 100));
    }
}
