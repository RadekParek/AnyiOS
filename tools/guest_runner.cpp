// Host runner for a real iOS .app bundle: the same entry point the Android app uses.
//
// This exists so compatibility work can be measured on the host instead of by guessing:
// it hands the bundle's Mach-O to `attemptLaunch` and prints the report the app shows,
// which names the exact API the guest stopped at. It is a development tool — the PC
// artifact ships it so a report can be reproduced without a phone.
//
//   guest-runner [--seconds N] [--instructions N] [--screen-size WIDTHxHEIGHT]
//                [--verbose] [--precise] [--log-lines N] [--native-gles]
//                [--frame-out screenshot.ppm] [--stop-after-first-frame]
//                [--bundle-archive IPA --bundle-prefix Payload/App.app/
//                 --bundle-cache DIR --bundle-overlay DIR]
//                <bundle.app|executable> [container dir]
//
// Without --seconds/--instructions the guest runs until it stops on its own, which is what a
// device does. Both flags exist so a test can bound a probe, not because the runtime needs a
// cap.
//
// The second argument is the guest's Documents directory; without it the guest gets no
// writable storage. Nothing here is a claim of rendering or gameplay: the report says what
// the guest executed and where it stopped.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/launch.h"

namespace {
namespace fs = std::filesystem;

// Mach-O magic, in both endiannesses, thin and universal.
bool isMachO(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    uint32_t magic = 0;
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic))) return false;
    return magic == 0xfeedface || magic == 0xfeedfacf || magic == 0xcafebabe ||
           magic == 0xcefaedfe || magic == 0xcffaedfe || magic == 0xbebafeca;
}

// A bundle's executable is the one Mach-O in the .app root. Choosing by size picks the
// executable over a helper or a dylib if a title ships both.
std::vector<uint8_t> readBundle(const fs::path& path, fs::path& bundle) {
    if (fs::is_directory(path)) {
        bundle = path;
        fs::path best;
        std::uintmax_t bestSize = 0;
        for (const auto& entry : fs::directory_iterator(path)) {
            if (!entry.is_regular_file() || !isMachO(entry.path())) continue;
            const auto size = entry.file_size();
            if (size > bestSize) {
                best = entry.path();
                bestSize = size;
            }
        }
        if (best.empty()) throw std::runtime_error("no Mach-O executable in " + path.string());
        std::cerr << "guest-runner: executable " << best.filename() << " (" << bestSize
                  << " bytes), bundle " << bundle.string() << '\n';
        std::ifstream file(best, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    bundle = path.parent_path();
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace

int main(int argc, char** argv) {
    int capturedWidth = 0, capturedHeight = 0;
    std::vector<uint8_t> capturedPixels;
    podnative::runtime::LaunchOptions options;
    fs::path frameOutput;
    std::vector<std::string> positional;
    bool stopAfterFirstFrame = false;
    std::atomic<bool> cancelRequested{false};
    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "--seconds" && i + 1 < argc) {
            options.seconds = std::stod(argv[++i]);
        } else if (argument == "--instructions" && i + 1 < argc) {
            options.instructions = std::stoull(argv[++i]);
        } else if (argument == "--log-lines" && i + 1 < argc) {
            // How many captured guest log lines the report carries. A verbose trace can be
            // long; the cap keeps the report readable while still showing the stop.
            options.logLines = std::stoi(argv[++i]);
        } else if (argument == "--precise") {
            // Exact PCs in the instruction trace, at roughly a tenth of the execution rate.
            options.precise = true;
            options.verbose = true;
        } else if (argument == "--native-gles") {
            // The Settings ▸ "Native OpenGL ES" switch, reproducible on a host: resolve GL
            // against the machine's own driver first instead of the bundled ANGLE.
            options.nativeGles = true;
        } else if (argument == "--frame-out" && i + 1 < argc) {
            frameOutput = argv[++i];
        } else if (argument == "--stop-after-first-frame") {
            stopAfterFirstFrame = true;
        } else if (argument == "--screen-size" && i + 1 < argc) {
            const std::string dimensions = argv[++i];
            const size_t separator = dimensions.find('x');
            if (separator == std::string::npos)
                throw std::runtime_error("screen size must be WIDTHxHEIGHT");
            options.screenWidth = std::stoi(dimensions.substr(0, separator));
            options.screenHeight = std::stoi(dimensions.substr(separator + 1));
        } else if (argument == "--orientation" && i + 1 < argc) {
            options.interfaceOrientation = std::stoi(argv[++i]);
        } else if (argument == "--bundle-archive" && i + 1 < argc) {
            options.bundleArchive = argv[++i];
        } else if (argument == "--bundle-prefix" && i + 1 < argc) {
            options.bundleArchivePrefix = argv[++i];
        } else if (argument == "--bundle-cache" && i + 1 < argc) {
            options.bundleCache = argv[++i];
        } else if (argument == "--bundle-overlay" && i + 1 < argc) {
            options.bundleOverlay = argv[++i];
        } else if (argument == "--verbose") {
            // Deep trace: every service call, every unwind decision, every GL error. The
            // report then carries the tail of it, which is what a compatibility report needs.
            options.verbose = true;
            options.logLines = 4000;
        } else if (argument.rfind("--", 0) == 0) {
            std::cerr << "guest-runner: unknown option " << argument << '\n';
            return 2;
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.empty() || positional.size() > 2) {
        std::cerr << "usage: guest-runner [--seconds N] [--instructions N] "
                     "[--screen-size WIDTHxHEIGHT] [--orientation 1..4] [--verbose] [--precise] "
                     "[--log-lines N] [--native-gles] [--frame-out screenshot.ppm] "
                     "[--stop-after-first-frame] "
                     "[--bundle-archive IPA --bundle-prefix Payload/App.app/ "
                     "--bundle-cache DIR --bundle-overlay DIR] "
                     "<bundle.app|executable> [container directory]\n"
                     "  no --seconds/--instructions: run until the guest stops by itself\n";
        return 2;
    }
    const bool archiveMount = !options.bundleArchive.empty();
    if (stopAfterFirstFrame && frameOutput.empty()) {
        std::cerr << "guest-runner: --stop-after-first-frame requires --frame-out\n";
        return 2;
    }
    if (stopAfterFirstFrame) options.cancelRequested = &cancelRequested;
    const bool hasArchiveDetails = !options.bundleArchivePrefix.empty() ||
                                   !options.bundleCache.empty() || !options.bundleOverlay.empty();
    if (archiveMount != hasArchiveDetails ||
        (archiveMount && (options.bundleArchivePrefix.empty() || options.bundleCache.empty() ||
                          options.bundleOverlay.empty() || positional.size() != 2))) {
        std::cerr << "guest-runner: archive-backed runs need --bundle-archive, --bundle-prefix, "
                     "--bundle-cache, --bundle-overlay, and a container directory\n";
        return 2;
    }
    try {
        fs::path bundle;
        const std::vector<uint8_t> image = readBundle(positional[0], bundle);
        std::string container;
        if (positional.size() == 2) {
            fs::create_directories(positional[1]);
            container = fs::absolute(positional[1]).lexically_normal().string();
        }
        if (archiveMount) {
            options.bundleArchive = fs::absolute(options.bundleArchive).lexically_normal().string();
            options.bundleCache = fs::absolute(options.bundleCache).lexically_normal().string();
            options.bundleOverlay = fs::absolute(options.bundleOverlay).lexically_normal().string();
        }
        const std::string bundleHost = archiveMount ? std::string() : bundle.string();
        if (!frameOutput.empty()) {
            frameOutput = fs::absolute(frameOutput).lexically_normal();
            options.frameSink = [&](int width, int height, const std::vector<uint8_t>& pixels) {
                if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * height * 4)
                    throw std::runtime_error("invalid guest framebuffer dimensions");
                capturedWidth = width;
                capturedHeight = height;
                capturedPixels = pixels;
                if (stopAfterFirstFrame) cancelRequested.store(true, std::memory_order_relaxed);
            };
        }
        const std::string report = podnative::runtime::attemptLaunch(
            image.data(), image.size(), bundleHost, container, options);
        if (stopAfterFirstFrame && report.find("LAUNCH_CANCELLED:") == std::string::npos)
            throw std::runtime_error("no framebuffer was streamed before the guest stopped");
        if (!frameOutput.empty()) {
            if (capturedPixels.empty())
                throw std::runtime_error("the guest did not present a framebuffer");
            std::ofstream output(frameOutput, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot write " + frameOutput.string());
            output << "P6\n" << capturedWidth << ' ' << capturedHeight << "\n255\n";
            std::vector<uint8_t> rgb(size_t(capturedWidth) * capturedHeight * 3);
            for (size_t pixel = 0; pixel < size_t(capturedWidth) * capturedHeight; pixel++) {
                rgb[pixel * 3] = capturedPixels[pixel * 4];
                rgb[pixel * 3 + 1] = capturedPixels[pixel * 4 + 1];
                rgb[pixel * 3 + 2] = capturedPixels[pixel * 4 + 2];
            }
            output.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
            if (!output) throw std::runtime_error("failed while writing " + frameOutput.string());
            std::cerr << "guest-runner: captured " << capturedWidth << 'x' << capturedHeight
                      << " guest framebuffer to " << frameOutput << '\n';
        }
        std::cout << report << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "guest-runner: " << e.what() << '\n';
        return 1;
    }
}
