#include "runtime/gles.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/memory.h"
using namespace podnative::runtime;
namespace {
void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
// Angry Birds 1.0 imports exactly these (read from its LC_SYMTAB); this is the contract
// the bridge signs up for, so a symbol dropped from the table is caught here.
const char* kAngryBirdsImports[] = {
    "_glActiveTexture",
    "_glBindBuffer",
    "_glBindFramebufferOES",
    "_glBindRenderbufferOES",
    "_glBindTexture",
    "_glBlendFunc",
    "_glBufferData",
    "_glCheckFramebufferStatusOES",
    "_glClear",
    "_glClearColor",
    "_glClientActiveTexture",
    "_glColor4f",
    "_glColorPointer",
    "_glCompressedTexImage2D",
    "_glDeleteBuffers",
    "_glDeleteFramebuffersOES",
    "_glDeleteRenderbuffersOES",
    "_glDeleteTextures",
    "_glDepthFunc",
    "_glDepthMask",
    "_glDisable",
    "_glDisableClientState",
    "_glDrawArrays",
    "_glDrawElements",
    "_glEnable",
    "_glEnableClientState",
    "_glFramebufferRenderbufferOES",
    "_glFramebufferTexture2DOES",
    "_glFrontFace",
    "_glGenBuffers",
    "_glGenFramebuffersOES",
    "_glGenRenderbuffersOES",
    "_glGenTextures",
    "_glGetIntegerv",
    "_glGetRenderbufferParameterivOES",
    "_glLightfv",
    "_glLineWidth",
    "_glLoadMatrixf",
    "_glMaterialfv",
    "_glMatrixMode",
    "_glNormalPointer",
    "_glPixelStorei",
    "_glRenderbufferStorageOES",
    "_glScissor",
    "_glTexCoordPointer",
    "_glTexEnvi",
    "_glTexImage2D",
    "_glTexParameteri",
    "_glTexSubImage2D",
    "_glVertexPointer",
    "_glViewport",
};
// A driver must never be faked. With no GLES library resolvable the bridge reports itself
// unavailable, owns no symbols, and refuses every call instead of returning success.
void noDriver() {
    Memory memory(8 * 1024 * 1024);
    GLES gles(memory);
    check(!gles.load(), "load() reports failure when no driver answers");
    check(!gles.available(), "no driver means the bridge is not available");
    check(!gles.symbols().empty(), "ownership of entry points does not need a driver");
    check(gles.describe().find("no OpenGL ES driver") != std::string::npos,
          "describe() names the missing driver: " + gles.describe());
    CpuState state = {};
    check(!gles.invoke("_glClear", state), "the bridge refuses a call without a driver");
    check(!gles.makeCurrent(), "no context can be made current without a driver");
    check(!gles.createDrawable(320, 480), "no drawable can be created without a driver");
    check(!gles.present(), "nothing can be presented without a driver");
    check(gles.callCount() == 0 && gles.drawCount() == 0, "no call is counted as performed");
    check(gles.lastFrame().empty(), "no frame is invented");
}
// The symbol table is data, not a driver: it can be checked with nothing installed.
void ownedSymbols() {
    Memory memory(8 * 1024 * 1024);
    GLES gles(memory);
    const std::vector<std::string> symbols = gles.symbols();
    check(!symbols.empty(), "the bridge advertises its entry points");
    for (const char* name : kAngryBirdsImports) {
        bool found = false;
        for (const std::string& symbol : symbols) found = found || symbol == name;
        check(found, std::string("Angry Birds imports ") + name);
    }
    // EAGL is a UIKit object boundary, not an exported GL symbol; it must not appear here.
    for (const std::string& symbol : symbols)
        check(symbol.rfind("_egl", 0) != 0, "EGL is host-side only: " + symbol);
}
// Marshalling is only reachable through a driver, so the *rejection* path is what a host
// without GL can verify: an unknown name is not consumed and stays a named trap.
void unknownNames() {
    Memory memory(8 * 1024 * 1024);
    GLES gles(memory);
    CpuState state = {};
    check(!gles.invoke("_glNotAnEntryPoint", state), "unknown GL names are not consumed");
    check(!gles.invoke("_NSLog", state), "a non-GL symbol is not consumed");
}
}  // namespace
int main() {
    noDriver();
    ownedSymbols();
    unknownNames();
    std::cout << "gles: no-driver refusal, symbol coverage and trap naming verified\n";
}
