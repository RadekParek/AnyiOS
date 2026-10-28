// A recording GLES double: validates guest ABI marshalling, not GPU rendering.
extern "C" {
static float color[4]{};
static unsigned int alphaFunction{};
static float alphaReference{};
static unsigned int arrayBuffer{};
static unsigned int vertexBuffer{};
static unsigned int normalBuffer{};
static unsigned int normalType{};
static int normalStride{};
static const void *vertexPointer{};
static const void *normalPointer{};
static unsigned int draws{};
static unsigned int framebuffer{};
static unsigned int renderbuffer{};
void glClearColor(float red, float green, float blue, float alpha) {
    color[0] = red; color[1] = green; color[2] = blue; color[3] = alpha;
}
void glAlphaFunc(unsigned int function, float reference) {
    alphaFunction = function; alphaReference = reference;
}
void glBindBuffer(unsigned int target, unsigned int name) { if (target == 0x8892) arrayBuffer = name; }
void glDeleteBuffers(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == arrayBuffer) arrayBuffer = 0;
}
void glEnableClientState(unsigned int) {}
void glVertexPointer(int, unsigned int, int, const void *pointer) { vertexBuffer = arrayBuffer; vertexPointer = pointer; }
void glNormalPointer(unsigned int type, int stride, const void *pointer) {
    normalBuffer = arrayBuffer; normalType = type; normalStride = stride; normalPointer = pointer;
}
void glDrawArrays(unsigned int, int, int) { ++draws; }
void glDrawElements(unsigned int, int, unsigned int, const void *) { ++draws; }
void glBindFramebufferOES(unsigned int target, unsigned int name) { if (target == 0x8D40) framebuffer = name; }
void glBindRenderbufferOES(unsigned int target, unsigned int name) { if (target == 0x8D41) renderbuffer = name; }
void glFramebufferRenderbufferOES(unsigned int, unsigned int, unsigned int, unsigned int) {}
void glFramebufferTexture2DOES(unsigned int, unsigned int, unsigned int, unsigned int, int) {}
void glDeleteRenderbuffersOES(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == renderbuffer) renderbuffer = 0;
}
void glDeleteFramebuffersOES(int count, const unsigned int *names) {
    for (int i = 0; i < count; ++i) if (names[i] == framebuffer) framebuffer = 0;
}
unsigned int recordedFramebuffer() { return framebuffer; }
unsigned int recordedArrayBuffer() { return arrayBuffer; }
unsigned int recordedVertexBuffer() { return vertexBuffer; }
unsigned int recordedNormalBuffer() { return normalBuffer; }
unsigned int recordedNormalType() { return normalType; }
int recordedNormalStride() { return normalStride; }
const void *recordedVertexPointer() { return vertexPointer; }
const void *recordedNormalPointer() { return normalPointer; }
unsigned int recordedDraws() { return draws; }
float recordedColor(unsigned int index) { return index < 4 ? color[index] : -1.0f; }
unsigned int recordedAlphaFunction() { return alphaFunction; }
float recordedAlphaReference() { return alphaReference; }
}
