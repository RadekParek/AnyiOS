package org.podnative;

import static org.junit.Assert.assertEquals;
import org.junit.Test;

public class GameArchitectureTest {
    @Test public void recognizesOnlyPositiveSlicesAndKeepsArm64_32Distinct() {
        assertEquals("ARM32", GameArchitecture.chip("ARMv7 · ARM32"));
        assertEquals("ARM32", GameArchitecture.chip("ARMv7 · ARM32 universal (no ARM64 slice detected)"));
        assertEquals("ARM32", GameArchitecture.chip("ARMv7 · ARM32 universal - no ARM64 slice detected"));
        assertEquals("ARM64 + ARM32", GameArchitecture.chip("ARMv7 · ARM32 + ARM64"));
        assertEquals("ARM64 + ARM32", GameArchitecture.chip("ARM64 + ARM32 universal (ARM32 selected: ARMv7)"));
        assertEquals("ARM64_32 + ARM32", GameArchitecture.chip("ARM64_32 + ARM32 universal (ARM32 selected: ARMv7)"));
        assertEquals("ARM64 + ARM64_32 + ARM32", GameArchitecture.chip("ARM64 + ARM64_32 + ARM32 universal (ARM32 selected: ARMv7)"));
    }
}
