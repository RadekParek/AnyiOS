#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "graphics.h"
#include "macho.h"
#include "macho_container.h"
#include "macho_info.h"
#include "runtime/cpu_probe.h"
#include "runtime/error.h"
#include "runtime/launch.h"
#include "runtime/loader.h"
#include "runtime/services.h"
namespace {
struct ImportCoverageRuntime {
    std::mutex mutex;
    std::unique_ptr<podnative::runtime::Memory> memory;
    std::unique_ptr<podnative::runtime::Services> services;
    podnative::runtime::ExportGraph graph;
    std::vector<std::string> libraries;
    size_t requester = SIZE_MAX;

    std::string summary(const podnative::MachORequirements& requirements) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!services || libraries != requirements.libraries) {
            services.reset();
            memory.reset();
            graph = podnative::runtime::ExportGraph();
            libraries = requirements.libraries;
            memory = std::make_unique<podnative::runtime::Memory>();
            services = std::make_unique<podnative::runtime::Services>(*memory);
            services->install(graph, libraries);
            requester = graph.addImage("<import report executable>");
            graph.dependencies(requester, libraries);
            graph.setMainExecutable(requester);
        }
        return requirements.summary([this](const podnative::ImportedSymbol& symbol) {
            try {
                return graph.resolve(requester, symbol.libraryOrdinal, symbol.name).has_value();
            } catch (const podnative::runtime::Error&) {
                return false;  // an invalid/missing ordinal is an actionable unresolved import
            }
        });
    }
};
std::string importCoverageSummary(const podnative::MachORequirements& requirements) {
    static ImportCoverageRuntime runtime;
    return runtime.summary(requirements);
}
std::unique_ptr<podnative::GraphicsBackend> backend;
// The Settings switch. It is process-wide on purpose: the report and the guest log must agree
// about whether deep tracing was on for the run that produced them.
bool verboseLogging = false;
// Settings ▸ "Native OpenGL ES". Also process-wide and also read at launch: the driver is
// resolved once, so the switch has to be settled before the guest makes its first GL call.
bool nativeGles = false;
std::atomic<int> guestScreenWidth{320}, guestScreenHeight{480};
std::atomic<bool> guestLaunchCancelled{false};
std::mutex guestFrameMutex;
std::vector<uint8_t> guestFrame;
int guestFrameWidth = 0, guestFrameHeight = 0;
uint32_t guestFrameGeneration = 0;
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
                report += "\n\n" +
                          importCoverageSummary(podnative::inspectRequirements(b.data(), b.size()));
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

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_beginGuestLaunch(JNIEnv*,
                                                                                   jclass) {
    guestLaunchCancelled.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(guestFrameMutex);
    guestFrame.clear();
    guestFrameWidth = guestFrameHeight = 0;
    guestFrameGeneration = 0;
}

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_cancelGuestLaunch(JNIEnv*,
                                                                                    jclass) {
    guestLaunchCancelled.store(true, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL Java_org_podnative_NativeBridge_setGuestScreenSize(JNIEnv* env,
                                                                                     jclass,
                                                                                     jint width,
                                                                                     jint height) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), "UIKIT_SCREEN");
        return;
    }
    guestScreenWidth.store(width, std::memory_order_relaxed);
    guestScreenHeight.store(height, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_podnative_NativeBridge_takeGuestFrame(JNIEnv* env, jclass, jint afterGeneration) {
    std::lock_guard<std::mutex> lock(guestFrameMutex);
    if (guestFrame.empty() ||
        (afterGeneration >= 0 && uint32_t(afterGeneration) == guestFrameGeneration))
        return env->NewByteArray(0);
    if (guestFrameWidth <= 0 || guestFrameHeight <= 0 ||
        guestFrame.size() != size_t(guestFrameWidth) * size_t(guestFrameHeight) * 4) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                      "GUEST_FRAME: stored readback has invalid dimensions");
        return nullptr;
    }
    const size_t payload = guestFrame.size();
    if (payload > size_t(std::numeric_limits<jsize>::max()) - 12) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                      "GUEST_FRAME: readback exceeds the JNI array limit");
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(jsize(payload + 12));
    if (!result) return nullptr;
    const uint32_t width = uint32_t(guestFrameWidth), height = uint32_t(guestFrameHeight);
    const uint32_t generation = guestFrameGeneration;
    const jbyte header[12] = {
        jbyte(width),           jbyte(width >> 8),       jbyte(width >> 16),
        jbyte(width >> 24),     jbyte(height),           jbyte(height >> 8),
        jbyte(height >> 16),    jbyte(height >> 24),     jbyte(generation),
        jbyte(generation >> 8), jbyte(generation >> 16), jbyte(generation >> 24)};
    env->SetByteArrayRegion(result, 0, 12, header);
    env->SetByteArrayRegion(result, 12, jsize(payload),
                            reinterpret_cast<const jbyte*>(guestFrame.data()));
    return env->ExceptionCheck() ? nullptr : result;
}

extern "C" JNIEXPORT jstring JNICALL Java_org_podnative_NativeBridge_attemptLaunch(
    JNIEnv* env, jclass, jbyteArray bytes, jstring bundlePath, jstring containerPath,
    jstring bundleArchivePath, jstring bundleArchivePrefix, jstring bundleCachePath,
    jstring bundleOverlayPath) {
    try {
        const jsize n = env->GetArrayLength(bytes);
        if (n > 64 * 1024 * 1024)
            throw std::runtime_error("LAUNCH_SIZE: ARM32 slice exceeds inspection budget");
        std::vector<uint8_t> data(static_cast<size_t>(n), uint8_t{});
        if (n) {
            env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(data.data()));
            if (env->ExceptionCheck()) return nullptr;
        }
        const std::string bundle = pathFrom(env, bundlePath);
        const std::string container = pathFrom(env, containerPath);
        const std::string archive = pathFrom(env, bundleArchivePath);
        const std::string archivePrefix = pathFrom(env, bundleArchivePrefix);
        const std::string cache = pathFrom(env, bundleCachePath);
        const std::string overlay = pathFrom(env, bundleOverlayPath);
        {
            std::lock_guard<std::mutex> lock(guestFrameMutex);
            guestFrame.clear();
            guestFrameWidth = guestFrameHeight = 0;
            guestFrameGeneration = 0;
        }
        podnative::runtime::LaunchOptions options;
        options.seconds =
            60.0;  // bounded main-menu startup probe; the screen shows its final frame
        options.verbose = verboseLogging;
        options.nativeGles = nativeGles;
        options.screenWidth = guestScreenWidth.load(std::memory_order_relaxed);
        options.screenHeight = guestScreenHeight.load(std::memory_order_relaxed);
        options.cancelRequested = &guestLaunchCancelled;
        options.bundleArchive = archive;
        options.bundleArchivePrefix = archivePrefix;
        options.bundleCache = cache;
        options.bundleOverlay = overlay;
        options.frameSink = [lastFrame = std::chrono::steady_clock::time_point{},
                             firstFrame = true](int width, int height,
                                                const std::vector<uint8_t>& pixels) mutable {
            const auto now = std::chrono::steady_clock::now();
            if (!firstFrame && now - lastFrame < std::chrono::milliseconds(100)) return;
            firstFrame = false;
            lastFrame = now;
            if (width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
                pixels.size() != size_t(width) * size_t(height) * 4)
                return;
            std::lock_guard<std::mutex> lock(guestFrameMutex);
            try {
                guestFrame = pixels;
                guestFrameWidth = width;
                guestFrameHeight = height;
                ++guestFrameGeneration;
                if (guestFrameGeneration == 0) ++guestFrameGeneration;
            } catch (const std::bad_alloc&) {
                // Keep the most recently delivered preview if another frame cannot be copied.
            }
        };
        const std::string report =
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
                                                                        jint width, jint height,
                                                                        jint rgbaOffset) {
    try {
        if (!pixels || width <= 0 || height <= 0 || width > 2048 || height > 2048 || rgbaOffset < 0)
            throw std::runtime_error("IMAGE_BOUNDS");
        const jsize arrayLength = env->GetArrayLength(pixels);
        const size_t payload = size_t(width) * size_t(height) * 4;
        if (size_t(rgbaOffset) > size_t(arrayLength) ||
            payload > size_t(arrayLength) - size_t(rgbaOffset))
            throw std::runtime_error("IMAGE_BOUNDS");
        if (!backend) throw std::runtime_error("BACKEND_NOT_READY");
        std::vector<uint8_t> rgba(payload);
        env->GetByteArrayRegion(pixels, rgbaOffset, jsize(rgba.size()),
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
