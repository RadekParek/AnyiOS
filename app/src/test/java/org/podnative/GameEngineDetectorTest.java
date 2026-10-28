package org.podnative;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import org.junit.Test;

public class GameEngineDetectorTest {
    @Test
    public void recognizesSpecificBundlesButNotGenericOrPluginNames() {
        GameEngineDetector unity = new GameEngineDetector();
        unity.observePath("Data/Managed/UnityEngine.dll");
        assertEquals("Unity (detected)", unity.result());
        GameEngineDetector modernUnity = new GameEngineDetector();
        modernUnity.observePath("Frameworks/UnityFramework.framework/UnityFramework");
        assertEquals("Unity (detected)", modernUnity.result());
        GameEngineDetector unreal = new GameEngineDetector();
        unreal.observePath("SwordGame/CookedIPhone/Engine.xxx");
        assertEquals("Unreal Engine (detected)", unreal.result());
        GameEngineDetector unknown = new GameEngineDetector();
        unknown.observePath("Data/engine.dll");
        unknown.observePath("PlugIns/Ad.app/Data/globalgamemanagers");
        unknown.observePath("Frameworks/CookedIPhone/Engine.xxx");
        unknown.observePath("../Data/globalgamemanagers");
        assertEquals("Unknown", unknown.result());
    }

    @Test
    public void metadataAndMultipleEnginesAreReportedWithoutInventingAVersion() {
        GameEngineDetector detector = new GameEngineDetector();
        detector.observeMetadata(Map.of("godot_path", "MyProject"));
        detector.observePath("Data/globalgamemanagers");
        assertEquals("Godot, Unity (detected)", detector.result());
        GameEngineDetector empty = new GameEngineDetector();
        empty.observeMetadata(Map.of("godot_path", ""));
        assertEquals("Unknown", empty.result());
    }

    @Test
    public void pairedExecutableSymbolsSpanReadBoundaries() throws Exception {
        byte[] bytes = new byte[16420];
        byte[] first = "CCDirector".getBytes(StandardCharsets.US_ASCII);
        byte[] second = "CCSprite".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(first, 0, bytes, 16380, first.length);
        System.arraycopy(second, 0, bytes, 16405, second.length);
        GameEngineDetector detector = new GameEngineDetector();
        detector.observeExecutable(new ByteArrayInputStream(bytes));
        assertEquals("Cocos2d (detected)", detector.result());
        GameEngineDetector single = new GameEngineDetector();
        single.observeExecutable(new ByteArrayInputStream(first));
        assertEquals("Unknown", single.result());
    }

    @Test
    public void octarineNeedsTheSpecificSourcePathAndSurvivesChunkBoundaries() throws Exception {
        byte[] bytes = new byte[16500];
        byte[] signature = GameEngineDetector.OCTARINE_SOURCE.getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(signature, 0, bytes, 16380, signature.length);
        GameEngineDetector detector = new GameEngineDetector();
        detector.observeExecutable(new ByteArrayInputStream(bytes));
        assertEquals("Octarine (detected)", detector.result());
        GameEngineDetector generic = new GameEngineDetector();
        generic.observeRuntimeLog("Octarine HSSSoundManager.cpp Terraria");
        assertEquals("Unknown", generic.result());
        generic.observeRuntimeLog("/Users/codeglue/dev/" + GameEngineDetector.OCTARINE_SOURCE);
        assertEquals("Octarine (detected)", generic.result());
        assertEquals("Octarine, Unity (detected)",
            GameEngineDetector.merge("Unity (detected)", generic.result()));
    }

    @Test
    public void gamemakerUsesRootGameDataNotPluginData() {
        GameEngineDetector detector = new GameEngineDetector();
        detector.observePath("PlugIns/Ad.app/game.ios");
        assertEquals("Unknown", detector.result());
        detector.observePath("game.ios");
        assertEquals("GameMaker (detected)", detector.result());
    }

    @Test
    public void ninjagoUsesExactBundleIdsAndLabelsUnverifiedEngineAsReported() {
        for (String id : new String[] {"com.lego.NinjagoSpinjitzuScavengerHunt",
                 "com.lego.riseofthesnakes", "COM.LEGO.RISEOFTHESNAKES"}) {
            GameEngineDetector detector = new GameEngineDetector();
            detector.observeMetadata(Map.of("CFBundleIdentifier", id));
            assertEquals("Big Blue Bubble engine (reported)", detector.result());
            detector.observePath("Data/globalgamemanagers");
            assertEquals("Unity (detected); Big Blue Bubble engine (reported)", detector.result());
            assertEquals("Octarine, Unity (detected); Big Blue Bubble engine (reported)",
                GameEngineDetector.merge(detector.result(), "Octarine (detected)"));
        }
        GameEngineDetector unrelated = new GameEngineDetector();
        unrelated.observeMetadata(Map.of("CFBundleIdentifier", "example.riseofthesnakes",
            "CFBundleName", "Ninjago Scavenger Hunt"));
        assertEquals("Unknown", unrelated.result());
        assertEquals("Unity (detected)", GameEngineDetector.merge("Unknown", "Unity (detected)"));
        assertEquals("Big Blue Bubble engine (detected)",
            GameEngineDetector.merge(
                "Big Blue Bubble engine (reported)", "Big Blue Bubble engine (detected)"));
    }

    @Test
    public void binaryScanIsBounded() throws Exception {
        class Endless extends InputStream {
            long bytes;
            @Override
            public int read() {
                bytes++;
                return 0;
            }
            @Override
            public int read(byte[] buffer, int offset, int length) {
                bytes += length;
                return length;
            }
        }
        Endless input = new Endless();
        GameEngineDetector detector = new GameEngineDetector();
        detector.observeExecutable(input);
        assertEquals(32L * 1024 * 1024, input.bytes);
        assertEquals("Unknown", detector.result());
        assertTrue(GameRequirements.fromPlist(Map.of())
                .withEngine(detector.result())
                .description()
                .contains("Runtime: AnyiOSEMU"));
    }
}
