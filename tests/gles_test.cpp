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
// Common ES 2.0/core spellings and state/readback entry points added for newer titles.
const char* kExpandedGlesImports[] = {
    "_glAttachShader", "_glBindAttribLocation", "_glBindFramebuffer", "_glBindRenderbuffer",
    "_glCheckFramebufferStatus", "_glClearStencil", "_glCompileShader", "_glCompressedTexSubImage2D",
    "_glCopyTexImage2D", "_glCopyTexSubImage2D", "_glCreateProgram", "_glCreateShader",
    "_glDetachShader", "_glFinish", "_glFlush", "_glGetActiveAttrib", "_glGetActiveUniform",
    "_glGetAttachedShaders", "_glGetBufferParameteriv", "_glGetFramebufferAttachmentParameteriv",
    "_glGetFramebufferAttachmentParameterivOES",
    "_glColorMaterial", "_glCullFace", "_glDeleteFramebuffers", "_glDeleteProgram",
    "_glDeleteRenderbuffers", "_glDeleteShader", "_glDisableVertexAttribArray",
    "_glEnableVertexAttribArray",
    "_glFramebufferRenderbuffer", "_glFramebufferTexture2D", "_glGenFramebuffers",
    "_glGenRenderbuffers", "_glGenerateMipmap", "_glGetAttribLocation", "_glGetBooleanv",
    "_glGetError", "_glGetFloatv", "_glGetProgramInfoLog", "_glGetProgramiv", "_glGetShaderInfoLog",
    "_glGetShaderiv", "_glGetShaderPrecisionFormat", "_glGetShaderSource", "_glGetString",
    "_glGetTexParameterfv", "_glGetTexParameteriv", "_glGetUniformfv", "_glGetUniformiv",
    "_glGetUniformLocation", "_glGetVertexAttribfv", "_glGetVertexAttribiv",
    "_glGetVertexAttribPointerv", "_glIsEnabled", "_glIsProgram", "_glIsShader", "_glLinkProgram",
    "_glLightf", "_glLightModelfv", "_glLightModeli", "_glLoadIdentity", "_glMaterialf",
    "_glMultMatrixf", "_glNormal3f", "_glOrthof", "_glPopMatrix", "_glPushMatrix",
    "_glRotatef", "_glScalef", "_glTranslatef", "_glFrustumf",
    "_glReadPixels", "_glReleaseShaderCompiler", "_glShaderBinary", "_glBufferSubData",
    "_glShaderSource", "_glBlendEquation", "_glBlendFuncSeparate", "_glStencilFunc", "_glStencilOp",
    "_glUniform1f", "_glUniform1fv", "_glUniform1i", "_glUniform1iv", "_glUniform2f", "_glUniform2fv",
    "_glUniform2i", "_glUniform2iv", "_glUniform3f", "_glUniform3fv", "_glUniform3i", "_glUniform3iv",
    "_glTexParameterf", "_glTexParameterfv", "_glTexParameteriv", "_glValidateProgram",
    "_glUniform4f", "_glUniform4fv", "_glUniform4i", "_glUniform4iv", "_glUniformMatrix2fv",
    "_glUniformMatrix3fv", "_glUniformMatrix4fv", "_glUseProgram", "_glVertexAttrib1f", "_glVertexAttrib1fv", "_glVertexAttrib2f", "_glVertexAttrib2fv",
    "_glVertexAttrib3f", "_glVertexAttrib3fv", "_glVertexAttrib4f", "_glVertexAttrib4fv",
    "_glVertexAttribPointer",
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
    for (const char* name : kExpandedGlesImports) {
        bool found = false;
        for (const std::string& symbol : symbols) found = found || symbol == name;
        check(found, std::string("expanded GLES import ") + name);
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
