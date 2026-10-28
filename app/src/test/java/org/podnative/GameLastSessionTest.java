package org.podnative;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.content.Context;
import android.os.Looper;
import dev.radek.conventor.SharedCatalog;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, manifest = Config.NONE, application = Application.class)
public class GameLastSessionTest {
    private Context context;
    private GameLibrary library;
    private ExecutorService worker;
    private File first, second;

    @Before
    public void setUp() throws Exception {
        SharedCatalog.end();
        SharedCatalog.install(null);
        context = RuntimeEnvironment.getApplication();
        context.getSharedPreferences("anyiose_runtime", Context.MODE_PRIVATE)
            .edit()
            .clear()
            .commit();
        first = createGame("First game", "AnyiOSEMU errored: first failure\n");
        second = createGame("Second game", "AnyiOSEMU errored: second failure\n");
        openLibrary();
    }

    private File createGame(String title, String log) throws Exception {
        File directory =
            new File(new File(context.getFilesDir(), "games"), "game-" + UUID.randomUUID());
        File data = new File(directory, "sandbox/AnyiOSEMU");
        Files.createDirectories(data.toPath());
        JSONObject metadata = new JSONObject()
                                  .put("title", title)
                                  .put("kind", "IPA")
                                  .put("architecture", "ARM32")
                                  .put("status", "Imported");
        Files.write(new File(directory, "game.json").toPath(),
            metadata.toString().getBytes(StandardCharsets.UTF_8));
        Files.write(new File(directory, "source.bin").toPath(), new byte[] {1, 2, 3});
        Files.write(
            new File(data, "AnyiOSEMU_log.txt").toPath(), log.getBytes(StandardCharsets.UTF_8));
        return directory;
    }

    private void openLibrary() throws Exception {
        library = GameLibrary.get(context);
        var field = GameLibrary.class.getDeclaredField("worker");
        field.setAccessible(true);
        worker = (ExecutorService) field.get(library);
        drain();
    }

    private void drain() throws Exception {
        worker.submit(() -> {}).get(30, TimeUnit.SECONDS);
        shadowOf(Looper.getMainLooper()).idle();
    }

    private GameLibrary.LastSession loadLastSession() throws Exception {
        AtomicReference<GameLibrary.LastSession> result = new AtomicReference<>();
        library.loadLastSession(result::set);
        drain();
        return result.get();
    }

    private void closeLibrary() throws Exception {
        if (worker != null)
            worker.shutdownNow();
        var field = GameLibrary.class.getDeclaredField("instance");
        field.setAccessible(true);
        field.set(null, null);
        worker = null;
        SharedCatalog.end();
    }

    @After
    public void tearDown() throws Exception {
        closeLibrary();
    }

    @Test
    public void preparationAndRecreationSelectTheLastGameNotAnotherGamesNewerLog()
        throws Exception {
        GameLibrary.Game chosen = library.games()
                                      .stream()
                                      .filter(game -> game.directory.equals(first))
                                      .findFirst()
                                      .orElseThrow();
        AtomicReference<File> source = new AtomicReference<>();
        library.prepareAnyiOSEmu(chosen, source::set);
        drain();
        assertNotNull(source.get());
        assertEquals(first.getName(),
            context.getSharedPreferences("anyiose_runtime", Context.MODE_PRIVATE)
                .getString("last_game_id", ""));
        File firstLog = new File(first, "sandbox/AnyiOSEMU/AnyiOSEMU_log.txt");
        File secondLog = new File(second, "sandbox/AnyiOSEMU/AnyiOSEMU_log.txt");
        // The preparation retained the old summary and started a fresh latest log.
        assertTrue(LatestGameLog.lastBlocker(chosen.anyioseDataDirectory()).previousSession);
        Files.write(firstLog.toPath(),
            "AnyiOSEMU errored: latest failure\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(secondLog.setLastModified(firstLog.lastModified() + 5000));

        closeLibrary();
        openLibrary(); // mimic the library being reconstructed after process death
        GameLibrary.LastSession session = loadLastSession();
        assertNotNull(session);
        assertEquals(first.getName(), session.game.id);
        assertFalse(session.readError);
        assertFalse(session.blocker.previousSession);
        assertTrue(session.blocker.message.contains("latest failure"));
    }

    @Test
    public void olderInstallWithoutLastGameIdFallsBackToNewestLogWithoutInventingFailure()
        throws Exception {
        File firstLog = new File(first, "sandbox/AnyiOSEMU/AnyiOSEMU_log.txt");
        File secondLog = new File(second, "sandbox/AnyiOSEMU/AnyiOSEMU_log.txt");
        Files.write(secondLog.toPath(), "normal exit\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(secondLog.setLastModified(firstLog.lastModified() + 5000));
        GameLibrary.LastSession session = loadLastSession();
        assertNotNull(session);
        assertEquals(second.getName(), session.game.id);
        assertNull(session.blocker);
        assertFalse(session.readError);
    }
}
