#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <stdexcept>
#include <vector>

#include "graphics.h"
#include "macho.h"
#include "macho_info.h"
#include "runtime/error.h"
#include "runtime/loader.h"
namespace {
std::unique_ptr<podnative::GraphicsBackend> backend;
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
