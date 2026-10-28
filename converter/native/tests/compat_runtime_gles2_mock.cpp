// Recording GLES2 double: it validates ABI and framebuffer wiring, not pixels.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {
std::vector<std::string> sources;
unsigned int boundFramebuffer = 999;
unsigned int boundRenderbuffer = 0;
unsigned int boundArrayBuffer = 0;
unsigned int lastAttribBuffer = 999;
unsigned int framebufferCalls = 0;
unsigned int drawCalls = 0;
unsigned int generatedBuffers = 0;
unsigned int textureUploads = 0;
unsigned int pixelReads = 0;
unsigned int integerQueries = 0;
int pixelUnpack = 4;
int pixelPack = 4;
const void *attributePointer = nullptr;
float uniform[4]{};
}
extern "C" {
unsigned int glCreateShader(unsigned int) { return 101; }
unsigned int glCreateProgram() { return 102; }
void glShaderSource(unsigned int, int count, const char *const *strings, const int *lengths) {
    sources.clear();
    for (int i = 0; i < count; ++i) {
        if (lengths && lengths[i] >= 0) sources.emplace_back(strings[i], static_cast<std::size_t>(lengths[i]));
        else sources.emplace_back(strings[i]);
    }
}
void glCompileShader(unsigned int) {}
void glLinkProgram(unsigned int) {}
void glUseProgram(unsigned int) {}
void glAttachShader(unsigned int, unsigned int) {}
int glGetUniformLocation(unsigned int, const char *) { return 7; }
void glGetShaderiv(unsigned int, unsigned int pname, int *value) { if (pname != 0xDEAD) *value = 1; }
void glGetShaderInfoLog(unsigned int shader, int size, int *length, char *text) {
    if (shader == 0) return; // invalid object: the driver leaves outputs alone
    if (length) *length = size > 3 ? 3 : (size > 0 ? size - 1 : 0);
    if (size > 0 && text) { const int n = size > 3 ? 3 : size - 1; std::copy("abc", "abc" + n, text); text[n] = '\0'; }
}
void glUniform4f(int, float a, float b, float c, float d) {
    uniform[0] = a; uniform[1] = b; uniform[2] = c; uniform[3] = d;
}
void glUniform4fv(int, int count, const float *data) {
    if (count > 0) std::copy(data, data + 4, uniform);
}
void glGenBuffers(int count, unsigned int *names) {
    ++generatedBuffers;
    for (int i = 0; i < count; ++i) names[i] = 30u + static_cast<unsigned>(i);
}
void glBindBuffer(unsigned int target, unsigned int buffer) { if (target == 0x8892) boundArrayBuffer = buffer; }
void glDeleteBuffers(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == boundArrayBuffer) boundArrayBuffer = 0;
}
void glPixelStorei(unsigned int name, int value) {
    if (name == 0x0CF5) pixelUnpack = value;
    if (name == 0x0D05) pixelPack = value;
}
void glGetIntegerv(unsigned int name, int *values) {
    ++integerQueries;
    if (name == 0x86A2) { values[0] = 2; return; }
    if (name == 0x86A3) { values[0] = 0x8D64; values[1] = 0x8D65; return; }
    if (name == 0x0BA2) { for (int i = 0; i < 4; ++i) values[i] = i + 10; return; }
    values[0] = 4096;
}
void glReadPixels(int, int, int width, int height, unsigned int format, unsigned int type, void *pixels) {
    ++pixelReads;
    if (!pixels || width <= 0 || height <= 0 || format != 0x1907 || type != 0x1401) return;
    auto *bytes = static_cast<unsigned char *>(pixels);
    const int row = width * 3;
    const int stride = (row + pixelPack - 1) & ~(pixelPack - 1);
    for (int y = 0; y < height; ++y) for (int x = 0; x < row; ++x) bytes[y * stride + x] = 0x5a;
}
void glTexImage2D(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *) { ++textureUploads; }
void glGenRenderbuffers(int count, unsigned int *names) {
    for (int i = 0; i < count; ++i) names[i] = 40u + static_cast<unsigned>(i);
}
void glBindRenderbuffer(unsigned int target, unsigned int name) { if (target == 0x8D41) boundRenderbuffer = name; }
void glGetRenderbufferParameteriv(unsigned int target, unsigned int pname, int *value) {
    if (target == 0x8D41 && pname == 0x8D50) *value = 8;
}
void glDeleteRenderbuffers(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == boundRenderbuffer) boundRenderbuffer = 0;
}
void glGenFramebuffers(int count, unsigned int *names) {
    for (int i = 0; i < count; ++i) names[i] = 50u + static_cast<unsigned>(i);
}
void glBindFramebuffer(unsigned int target, unsigned int framebuffer) {
    if (target == 0x8D40) boundFramebuffer = framebuffer;
    ++framebufferCalls;
}
void glDeleteFramebuffers(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == boundFramebuffer) boundFramebuffer = 0;
}
void glFramebufferRenderbuffer(unsigned int, unsigned int, unsigned int, unsigned int) {}
void glFramebufferTexture2D(unsigned int, unsigned int, unsigned int, unsigned int, int) {}
unsigned int glCheckFramebufferStatus(unsigned int target) { return target == 0x8D40 ? 0x8CD5 : 0; }
void glEnableVertexAttribArray(unsigned int) {}
void glDisableVertexAttribArray(unsigned int) {}
void glVertexAttribPointer(unsigned int, int, unsigned int, unsigned char, int, const void *pointer) {
    lastAttribBuffer = boundArrayBuffer;
    attributePointer = pointer;
}
void glDrawArrays(unsigned int, int, int) { ++drawCalls; }
void glDrawElements(unsigned int, int, unsigned int, const void *) { ++drawCalls; }
unsigned int recordedBoundFramebuffer() { return boundFramebuffer; }
unsigned int recordedFramebufferCalls() { return framebufferCalls; }
unsigned int recordedDrawCalls() { return drawCalls; }
unsigned int recordedGeneratedBuffers() { return generatedBuffers; }
unsigned int recordedTextureUploads() { return textureUploads; }
unsigned int recordedPixelReads() { return pixelReads; }
unsigned int recordedIntegerQueries() { return integerQueries; }
int recordedUnpackAlignment() { return pixelUnpack; }
unsigned int recordedAttribBuffer() { return lastAttribBuffer; }
std::uintptr_t recordedAttribPointer() { return reinterpret_cast<std::uintptr_t>(attributePointer); }
int recordedSourceCount() { return static_cast<int>(sources.size()); }
int recordedSourceLength(unsigned int i) { return i < sources.size() ? static_cast<int>(sources[i].size()) : -1; }
int recordedSourceByte(unsigned int i, unsigned int j) {
    return i < sources.size() && j < sources[i].size() ? static_cast<unsigned char>(sources[i][j]) : -1;
}
float recordedUniform(unsigned int i) { return i < 4 ? uniform[i] : -1.0f; }
}
