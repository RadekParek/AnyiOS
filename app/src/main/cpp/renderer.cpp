#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "graphics.h"
namespace podnative {
namespace {
[[noreturn]] void error(const std::string& s) { throw std::runtime_error(s); }
void eglCheck(bool ok, const char* phase) {
    if (!ok) {
        std::ostringstream s;
        s << phase << " EGL=0x" << std::hex << eglGetError();
        error(s.str());
    }
}
void glCheck(const char* phase) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        std::ostringstream s;
        s << phase << " GL=0x" << std::hex << e;
        error(s.str());
    }
}
std::string preflight() {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS)
        error("VULKAN_INSTANCE_ENUM_FAILED");
    std::vector<VkExtensionProperties> exts(n);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data()) != VK_SUCCESS)
        error("VULKAN_INSTANCE_ENUM_FAILED");
    for (const char* required : {"VK_KHR_surface", "VK_KHR_android_surface"}) {
        if (std::none_of(exts.begin(), exts.end(),
                         [required](const auto& x) { return !strcmp(x.extensionName, required); }))
            error(std::string("VULKAN_MISSING_EXTENSION: ") + required);
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "AnyiOS preflight";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
        error("VULKAN_1_1_INSTANCE_FAILED");
    std::string selected;
    if (vkEnumeratePhysicalDevices(instance, &n, nullptr) == VK_SUCCESS && n) {
        std::vector<VkPhysicalDevice> devices(n);
        if (vkEnumeratePhysicalDevices(instance, &n, devices.data()) == VK_SUCCESS)
            for (auto device : devices) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(device, &props);
                VkPhysicalDeviceFeatures features{};
                vkGetPhysicalDeviceFeatures(device, &features);
                uint32_t count = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
                std::vector<VkQueueFamilyProperties> queues(count);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues.data());
                bool graphics = std::any_of(queues.begin(), queues.end(), [](const auto& q) {
                    return q.queueCount && (q.queueFlags & VK_QUEUE_GRAPHICS_BIT);
                });
                count = 0;
                bool swap = false;
                if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) ==
                    VK_SUCCESS) {
                    std::vector<VkExtensionProperties> de(count);
                    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, de.data()) ==
                        VK_SUCCESS)
                        swap = std::any_of(de.begin(), de.end(), [](const auto& e) {
                            return !strcmp(e.extensionName, "VK_KHR_swapchain");
                        });
                }
                if (props.apiVersion >= VK_API_VERSION_1_1 && graphics && swap) {
                    std::ostringstream s;
                    s << "candidate=" << props.deviceName
                      << " api=" << VK_VERSION_MAJOR(props.apiVersion) << '.'
                      << VK_VERSION_MINOR(props.apiVersion) << " driver=" << props.driverVersion
                      << " ETC2=" << features.textureCompressionETC2
                      << " anisotropy=" << features.samplerAnisotropy;
                    selected = s.str();
                    break;
                }
            }
    }
    vkDestroyInstance(instance, nullptr);
    if (selected.empty())
        error(
            "VULKAN_UNSUPPORTED: need Vulkan >=1.1, a graphics queue and VK_KHR_swapchain; "
            "ANGLE's OpenGL ES backend will be requested instead");
    return selected;
}
class AngleBackend final : public GraphicsBackend {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint program = 0, texture = 0, vertices = 0, indices = 0;
    GLint angle = -1, aspect = -1, imageMode = -1;
    bool showingImage = false;
    int width = 0, height = 0;
    std::string report;
    GLuint shader(GLenum type, const char* source) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &source, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        char log[4096]{};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "PodNative",
                            "shader type=%u log=%s", type, log);
        if (!ok) {
            glDeleteShader(s);
            error(std::string("SHADER_COMPILE: ") + log);
        }
        report += "\nshader type=" + std::to_string(type) + " log=" + log;
        return s;
    }

    void teardown() {
        if (display == EGL_NO_DISPLAY) return;
        if (context != EGL_NO_CONTEXT && eglGetCurrentContext() == context) {
            glDeleteProgram(program);
            glDeleteTextures(1, &texture);
            glDeleteBuffers(1, &vertices);
            glDeleteBuffers(1, &indices);
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        eglReleaseThread();
        display = EGL_NO_DISPLAY;
        surface = EGL_NO_SURFACE;
        context = EGL_NO_CONTEXT;
        program = 0;
        vertices = indices = texture = 0;
    }

   public:
    ~AngleBackend() override { teardown(); }
    void attach(ANativeWindow* window, int w, int h) override {
        if (w < 1 || h < 1) error("SURFACE_SIZE_INVALID");
        width = w;
        height = h;
        // ANGLE is used either way; the Vulkan backend is preferred because it is the
        // configuration this project validates, but a device whose driver cannot give us
        // a Vulkan 1.1 instance is reported and falls back to ANGLE's own OpenGL ES
        // backend. The substitution is always named in diagnostics; nothing silent.
        report = "embedded ANGLE\n";
        std::string vulkanFailure;
        try {
            report += "backend request: Vulkan\n" + preflight();
            open(window, true);
            report += "\nbackend in use: Vulkan";
            return;
        } catch (const std::exception& e) {
            vulkanFailure = e.what();
            report += std::string("\nVulkan backend unavailable: ") + vulkanFailure;
            teardown();
            eglReleaseThread();
        }
        report += "\nbackend request: OpenGL ES (fallback after Vulkan refusal)";
        open(window, false);
        report += "\nbackend in use: OpenGL ES via ANGLE\nVulkan refusal was: " + vulkanFailure;
    }
    void open(ANativeWindow* window, bool vulkan) {
        const char* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "EGL_ANGLE_platform_angle") ||
            (vulkan && !strstr(extensions, "EGL_ANGLE_platform_angle_vulkan")))
            error(vulkan ? "ANGLE_VULKAN_PLATFORM_EXTENSION_MISSING"
                         : "ANGLE_PLATFORM_EXTENSION_MISSING");
        auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!getDisplay) error("ANGLE_PLATFORM_ENTRYPOINT_MISSING");
        const EGLint attrs[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE,
            vulkan ? EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE : EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
            EGL_NONE};
        display = getDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
        eglCheck(display != EGL_NO_DISPLAY, "DISPLAY");
        EGLint major = 0, minor = 0;
        eglCheck(eglInitialize(display, &major, &minor), "INITIALIZE");
        eglCheck(eglBindAPI(EGL_OPENGL_ES_API), "BIND_API");
        const EGLint configAttrs[] = {EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE,
                                      8,
                                      EGL_GREEN_SIZE,
                                      8,
                                      EGL_BLUE_SIZE,
                                      8,
                                      EGL_ALPHA_SIZE,
                                      8,
                                      EGL_DEPTH_SIZE,
                                      24,
                                      EGL_NONE};
        EGLConfig config{};
        EGLint count = 0;
        eglCheck(eglChooseConfig(display, configAttrs, &config, 1, &count) && count == 1, "CONFIG");
        EGLint format = 0;
        eglCheck(eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format), "VISUAL");
        if (ANativeWindow_setBuffersGeometry(window, width, height, format) != 0)
            error("SURFACE_GEOMETRY_FAILED");
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttrs);
        eglCheck(context != EGL_NO_CONTEXT, "CONTEXT");
        surface = eglCreateWindowSurface(display, config, window, nullptr);
        eglCheck(surface != EGL_NO_SURFACE, "DRAWABLE");
        eglCheck(eglMakeCurrent(display, surface, surface, context), "MAKE_CURRENT");
        eglCheck(eglSwapInterval(display, 1), "SWAP_INTERVAL");
        auto str = [](GLenum e) {
            const auto* s = glGetString(e);
            return s ? reinterpret_cast<const char*>(s) : "unavailable";
        };
        report += "\nEGL vendor=" + std::string(eglQueryString(display, EGL_VENDOR)) +
                  " version=" + eglQueryString(display, EGL_VERSION);
        report += "\nGL version=" + std::string(str(GL_VERSION)) + "\nrenderer=" + str(GL_RENDERER);
        if (!strstr(str(GL_RENDERER), "ANGLE"))
            error("ANGLE_IDENTITY_MISMATCH: refusing unidentified renderer");
        const char* vs =
            "attribute vec3 position; attribute vec2 uv; varying vec2 tex; uniform float angle; "
            "uniform float aspect; uniform float imageMode; void main(){ if(imageMode>0.5){gl_Position=vec4(position,1.0); tex=uv; return;} float c=cos(angle), s=sin(angle); vec3 "
            "p=vec3(c*position.x+s*position.z,position.y,-s*position.x+c*position.z); "
            "p=vec3(p.x,0.9*p.y-0.435*p.z,0.435*p.y+0.9*p.z); p.z-=4.5; "
            "gl_Position=vec4(1.73*p.x/aspect,1.73*p.y,-1.02*p.z-0.202,-p.z); tex=uv; }";
        const char* fs =
            "precision mediump float; varying vec2 tex; uniform sampler2D image; void "
            "main(){gl_FragColor=texture2D(image,tex);}";
        GLuint a = shader(GL_VERTEX_SHADER, vs), b = 0;
        try {
            b = shader(GL_FRAGMENT_SHADER, fs);
        } catch (...) {
            glDeleteShader(a);
            throw;
        }
        program = glCreateProgram();
        glAttachShader(program, a);
        glAttachShader(program, b);
        glBindAttribLocation(program, 0, "position");
        glBindAttribLocation(program, 1, "uv");
        glLinkProgram(program);
        glDeleteShader(a);
        glDeleteShader(b);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        char log[4096]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_INFO, "PodNative", "program link=%d log=%s", linked, log);
        if (!linked) error(std::string("PROGRAM_LINK: ") + log);
        report += "\nprogram linked; log=" + std::string(log);
        // 24 independent face vertices; index buffer, texture and program persist for the surface
        // lifetime.
        const GLfloat v[] = {
            -1, -1, 1,  0, 0, 1,  -1, 1,  1, 0, 1, 1,  1,  1, 1, -1, 1,  1,  0, 1, 1,  -1, -1, 0, 0,
            -1, -1, -1, 1, 0, -1, 1,  -1, 1, 1, 1, 1,  -1, 0, 1, -1, -1, -1, 0, 0, -1, -1, 1,  1, 0,
            -1, 1,  1,  1, 1, -1, 1,  -1, 0, 1, 1, -1, 1,  0, 0, 1,  -1, -1, 1, 0, 1,  1,  -1, 1, 1,
            1,  1,  1,  0, 1, -1, 1,  1,  0, 0, 1, 1,  1,  1, 0, 1,  1,  -1, 1, 1, -1, 1,  -1, 0, 1,
            -1, -1, -1, 0, 0, 1,  -1, -1, 1, 0, 1, -1, 1,  1, 1, -1, -1, 1,  0, 1};
        GLushort idx[36];
        for (int f = 0; f < 6; f++) {
            const int order[] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) idx[f * 6 + k] = GLushort(f * 4 + order[k]);
        }
        glGenBuffers(1, &vertices);
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        glGenBuffers(1, &indices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        uint8_t pixels[8 * 8 * 4];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int p = (y * 8 + x) * 4;
                bool bright = ((x / 2 + y / 2) % 2) == 0;
                pixels[p] = bright ? 119 : 24;
                pixels[p + 1] = bright ? 220 : 65;
                pixels[p + 2] = bright ? 204 : 92;
                pixels[p + 3] = 255;
            }
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "image"), 0);
        imageMode = glGetUniformLocation(program, "imageMode");
        glUniform1f(imageMode, 0);
        angle = glGetUniformLocation(program, "angle");
        aspect = glGetUniformLocation(program, "aspect");
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glViewport(0, 0, width, height);
        glCheck("RESOURCE_SETUP");
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            error("FRAMEBUFFER_INCOMPLETE");
        __android_log_print(ANDROID_LOG_INFO, "PodNative", "%s", report.c_str());
    }
    void image(const uint8_t* rgba, int w, int h) override {
        if (!rgba || w <= 0 || h <= 0 || w > 2048 || h > 2048) error("IMAGE_BOUNDS");
        const float ratio = float(w) / h, screen = float(width) / height;
        const float x = ratio > screen ? 1.0f : ratio / screen;
        const float y = ratio > screen ? screen / ratio : 1.0f;
        const GLfloat quad[] = {-x, -y, 0, 0, 1, x, -y, 0, 1, 1, x, y, 0, 1, 0, -x, y, 0, 0, 0};
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glUseProgram(program);
        glUniform1f(imageMode, 1);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        showingImage = true;
        glCheck("IMPORTED_IMAGE_UPLOAD");
    }
    void draw(float seconds) override {
        glClearColor(showingImage ? 0 : 0.035f, showingImage ? 0 : 0.06f, showingImage ? 0 : 0.09f,
                     1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);
        glUniform1f(angle, seconds * 0.55f);
        glUniform1f(aspect, float(width) / height);
        glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glDrawElements(GL_TRIANGLES, showingImage ? 6 : 36, GL_UNSIGNED_SHORT, nullptr);
        glCheck("DRAW");
        eglCheck(eglSwapBuffers(display, surface), "PRESENT");
    }
    std::string diagnostics() const override { return report; }
};
}  // namespace
std::unique_ptr<GraphicsBackend> makeAngleBackend() { return std::make_unique<AngleBackend>(); }
}  // namespace podnative
