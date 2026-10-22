// Host runner for a real iOS .app bundle: the same entry point the Android app uses.
//
// This exists so compatibility work can be measured on the host instead of by guessing:
// it hands the bundle's Mach-O to `attemptLaunch` and prints the report the app shows,
// which names the exact API the guest stopped at. It is a development tool — the PC
// artifact ships it so a report can be reproduced without a phone.
//
//   guest-runner [--seconds N] [--instructions N] [--verbose] [--precise] [--log-lines N]
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
    podnative::runtime::LaunchOptions options;
    std::vector<std::string> positional;
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
        std::cerr << "usage: guest-runner [--seconds N] [--instructions N] [--verbose] "
                     "<bundle.app|executable> [container directory]\n"
                     "  no --seconds/--instructions: run until the guest stops by itself\n";
        return 2;
    }
    try {
        fs::path bundle;
        const std::vector<uint8_t> image = readBundle(positional[0], bundle);
        std::string container;
        if (positional.size() == 2) {
            fs::create_directories(positional[1]);
            container = positional[1];
        }
        std::cout << podnative::runtime::attemptLaunch(image.data(), image.size(), bundle.string(),
                                                       container, options)
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "guest-runner: " << e.what() << '\n';
        return 1;
    }
}
