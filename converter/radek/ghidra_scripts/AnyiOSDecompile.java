// Bounded inspection only: inferred C is not the original game source.
// Run by Ghidra's local analyzeHeadless, never by the Android app.
// @category AnyiOS
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.util.headless.HeadlessScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.OutputStreamWriter;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

public class AnyiOSDecompile extends HeadlessScript {
    private static final int MAX_FUNCTIONS = 32;
    private static final int MAX_FUNCTION_CHARS = 24000;
    private static final int MAX_OUTPUT_CHARS = 900000;
    private static final int SECONDS_PER_FUNCTION = 3;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1 || currentProgram == null) {
            throw new IllegalArgumentException("expected one output path and one imported program");
        }
        int attempted = 0, completed = 0, written = 0;
        boolean limited = false;
        DecompInterface decompiler = new DecompInterface();
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("Ghidra could not open the imported program");
        }
        try (OutputStreamWriter out = new OutputStreamWriter(
                new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            out.write("/* ANYIOS_GHIDRA_REPORT_V1\n");
            out.write(" * Ghidra-inferred pseudocode; not original source or a runnable port.\n");
            out.write(" * Language: " + currentProgram.getLanguageID().toString() + "\n */\n\n");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && attempted < MAX_FUNCTIONS && written < MAX_OUTPUT_CHARS) {
                monitor.checkCancelled();
                Function function = functions.next();
                attempted++;
                DecompileResults result = decompiler.decompileFunction(function, SECONDS_PER_FUNCTION, monitor);
                if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                    continue;
                }
                String code = result.getDecompiledFunction().getC();
                if (code == null) continue;
                completed++;
                int allowed = Math.min(MAX_FUNCTION_CHARS, MAX_OUTPUT_CHARS - written);
                if (code.length() > allowed) {
                    code = code.substring(0, allowed) + "\n/* function output truncated */\n";
                    limited = true;
                }
                out.write("/* address " + function.getEntryPoint().toString() + " */\n");
                out.write(code);
                out.write("\n\n");
                written += code.length();
            }
            limited = limited || functions.hasNext();
            // A completion marker is required; the caller discards partial files.
            out.write("\n/* ANYIOS_RESULT attempted=" + attempted + " completed=" + completed
                    + " limited=" + (limited ? 1 : 0)
                    + " analysisTimedOut=" + (analysisTimeoutOccurred() ? 1 : 0) + " */\n");
        } finally {
            decompiler.dispose();
        }
    }
}
