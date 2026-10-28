// A recording GLES double: validates guest ABI marshalling, not GPU rendering.
extern "C" {
static float color[4]{};
static unsigned int alphaFunction{};
static float alphaReference{};
void glClearColor(float red, float green, float blue, float alpha) {
    color[0] = red; color[1] = green; color[2] = blue; color[3] = alpha;
}
void glAlphaFunc(unsigned int function, float reference) {
    alphaFunction = function; alphaReference = reference;
}
float recordedColor(unsigned int index) { return index < 4 ? color[index] : -1.0f; }
unsigned int recordedAlphaFunction() { return alphaFunction; }
float recordedAlphaReference() { return alphaReference; }
}
