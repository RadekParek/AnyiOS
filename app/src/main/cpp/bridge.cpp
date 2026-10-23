#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <stdexcept>
#include <vector>

#include "graphics.h"
#include "macho.h"
#include "macho_container.h"
#include "macho_info.h"
#include "runtime/cpu_probe.h"
#include "runtime/error.h"
#include "runtime/launch.h"
#include "runtime/loader.h"
namespace {
std::unique_ptr<podnative::GraphicsBackend> backend;
// The Settings switch. It is process-wide on purpose: the report and the guest log must agree
// about whether deep tracing was on for the run that produced them.
bool verboseLogging = false;
// Settings ▸ "Native OpenGL ES". Also process-wide and also read at launch: the driver is
// resolved once, so the switch has to be settled before the guest makes its first GL call.
bool nativeGles = false;
void fail(JNIEnv* env, const std::exception& e) {
    __android_log_print(ANDROID_LOG_ERROR, "PodNative", "%s", e.what());
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), e.what());
}
}  // namespace
extern "C" JNIEXPORT jstring JNICALL Java_org_podnative_NativeBridge_start(JNIEnv* env, jclass,
                                                                           jobject surface, jint w,
                                                                           jint h) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "SURFACE_NULL");
        return nullptr;
    }
    try {
        backend = podnative::makeAngleBackend();
        backend->attach(window, w, h);
        std::string info = backend->diagnostics();
        jstring result = env->NewStringUTF(info.c_str());
        ANativeWindow_release(window);
        return result;
    } catch (const std::exception& e) {
        backend.reset();
        ANativeWindow_release(window);
        fail(env, e);
        return nullptr;
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_frame(JNIEnv* env, jclass,
                                                                        jfloat t) {
    try {
        if (!backend) throw std::runtime_error("BACKEND_NOT_READY");
        backend->draw(t);
    } catch (const std::exception& e) {
        fail(env, e);
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_stop(JNIEnv*, jclass) {
    backend.reset();
}
extern "C" JNIEXPORT jstring JNICALL Java_org_podnative_NativeBridge_inspect(JNIEnv* env, jclass,
                                                                             jbyteArray bytes) {
    try {
        auto n = env->GetArrayLength(bytes);
        if (n > 64 * 1024 * 1024)
            throw std::runtime_error("MACHO_SIZE: inspection limited to 64 MiB");
        std::vector<uint8_t> b(n);
        env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(b.data()));
        if (env->ExceptionCheck()) return nullptr;
        std::string report = podnative::inspectMachO(b.data(), b.size());
        if (report.rfind("INSPECTED_ARM32:", 0) == 0) {
            try {
                report += "\n\n" + podnative::inspectRequirements(b.data(), b.size()).summary();
            } catch (const podnative::runtime::Error& e) {
                report += "\n\nIMPORT_ANALYSIS_BLOCKED: " + std::string(e.what());
            }
            try {
                // Validation is bounded and temporary; not game launch and not retained runtime
                // state.
                auto image = podnative::runtime::loadMachO(b.data(), b.size(), 64 * 1024 * 1024);
                report +=
                    "\n\n" + image.summary() + "\nValidation mappings released after inspection.";
            } catch (const podnative::runtime::Error& e) {
                report += "\n\nLOAD_BLOCKED: " + std::string(e.what());
            }
        }
        return env->NewStringUTF(report.c_str());
    } catch (const std::exception& e) {
        fail(env, e);
        return nullptr;
    }
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_org_podnative_NativeBridge_arm32Slice(JNIEnv* env, jclass, jbyteArray header, jlong length) {
    try {
        const auto n = env->GetArrayLength(header);
        if (n > 4096 || length < 0)
            throw std::runtime_error("CONTAINER_ARGUMENT: invalid header/file length");
        std::vector<uint8_t> bytes(n);
        env->GetByteArrayRegion(header, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
        if (env->ExceptionCheck()) return nullptr;
        const auto slice =
            podnative::selectArm32Slice(bytes.data(), bytes.size(), uint64_t(length));
        const jlong result[] = {jlong(slice.offset), jlong(slice.size), jlong(slice.subtype),
                                slice.universal ? 1 : 0};
        auto array = env->NewLongArray(4);
        if (array) env->SetLongArrayRegion(array, 0, 4, result);
        return array;
    } catch (const std::exception& e) {
        fail(env, e);
        return nullptr;
    }
}

namespace {
// JNI strings are modified UTF-8; a sandbox path needs the plain bytes and no NULs.
std::string pathFrom(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (!text) return {};
    std::string path(text);
    env->ReleaseStringUTFChars(value, text);
    if (path.size() > 4096) return {};
    return path;
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_podnative_NativeBridge_attemptLaunch(
    JNIEnv* env, jclass, jbyteArray bytes, jstring bundlePath, jstring containerPath) {
    try {
        const auto n = env->GetArrayLength(bytes);
        if (n > 64 * 1024 * 1024)
            throw std::runtime_error("LAUNCH_SIZE: ARM32 slice exceeds inspection budget");
        std::vector<uint8_t> data(n);
        env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) return nullptr;
        const std::string bundle = pathFrom(env, bundlePath);
        const std::string container = pathFrom(env, containerPath);
        podnative::runtime::LaunchOptions options;
        options.verbose = verboseLogging;
        options.nativeGles = nativeGles;
        if (options.verbose) options.logLines = 4000;
        const auto report =
            podnative::runtime::attemptLaunch(data.data(), data.size(), bundle, container, options);
        return env->NewStringUTF(report.c_str());
    } catch (const std::exception& e) {
        fail(env, e);
        return nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_setVerbose(JNIEnv*, jclass,
                                                                             jboolean on) {
    verboseLogging = on == JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_setNativeGles(JNIEnv*, jclass,
                                                                                jboolean on) {
    nativeGles = on == JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_image(JNIEnv* env, jclass,
                                                                        jbyteArray pixels,
                                                                        jint width, jint height) {
    try {
        if (width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
            env->GetArrayLength(pixels) != width * height * 4)
            throw std::runtime_error("IMAGE_BOUNDS");
        if (!backend) throw std::runtime_error("BACKEND_NOT_READY");
        std::vector<uint8_t> rgba(size_t(width) * height * 4);
        env->GetByteArrayRegion(pixels, 0, jsize(rgba.size()),
                                reinterpret_cast<jbyte*>(rgba.data()));
        if (env->ExceptionCheck()) return;
        backend->image(rgba.data(), width, height);
    } catch (const std::exception& e) {
        fail(env, e);
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_org_podnative_NativeBridge_cpuProbe(JNIEnv* env, jclass) {
    const auto report = podnative::runtime::cpuProbe();
    return env->NewStringUTF(report.c_str());
}
