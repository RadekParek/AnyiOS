#include "runtime/fs.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "runtime/error.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/stdio.h"
using namespace podnative::runtime;
namespace {
template <class F>
void fault(const char* code, F action) {
    try {
        action();
    } catch (const Error& e) {
        if (e.code == code) return;
        std::cerr << e.what() << " expected " << code << '\n';
        std::abort();
    }
    std::cerr << "Expected fault " << code << '\n';
    std::abort();
}
void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "Failed: " << message << '\n';
    std::abort();
}
struct Call {
    Memory& memory;
    Stdio& stdio;
    CpuState state;
    uint32_t operator()(const std::string& name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0,
                        uint32_t d = 0) {
        state = {};
        state.registers[0] = a;
        state.registers[1] = b;
        state.registers[2] = c;
        state.registers[3] = d;
        if (!stdio.invoke(name, state)) throw Error("TEST_SYMBOL", "unimplemented " + name);
        return state.registers[0];
    }
};
void put(Memory& memory, uint32_t address, const std::string& text) {
    memory.write(address, text.c_str(), text.size() + 1);
}
std::string readText(Memory& memory, uint32_t address, size_t size) {
    std::vector<char> bytes(size + 1, 0);
    memory.read(address, bytes.data(), size);
    return std::string(bytes.data());
}
std::string hostText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}  // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / "anyios-fs-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bundle" / "data");
    std::filesystem::create_directories(root / "container");
    std::ofstream(root / "bundle" / "data" / "level.txt") << "level one";
    std::error_code ignored;
    std::filesystem::create_symlink("/etc/passwd", root / "bundle" / "escape", ignored);

    Memory memory;
    std::vector<uint8_t> page(65536, 0);
    memory.map({0x1000, 65536, uint8_t(Read | Write), "guest data"}, page.data(), page.size());
    GuestHeap heap(memory);

    FileSystem files;
    std::string reason;
    check(files.resolve("/var/mobile/x", false, reason).empty(),
          "nothing is reachable before a mount exists");
    files.mount("/var/mobile/Documents", (root / "container").string(), true);
    // The bundle is writable: this image targets iOS 3, where a title's own directory
    // was writable and games wrote caches next to their resources.
    files.mount("/var/mobile", (root / "bundle").string(), true);
    check(files.count() == 2, "both mounts are registered");

    const std::string level = files.resolve("/var/mobile/data/level.txt", false, reason);
    check(!level.empty() && std::filesystem::exists(level), "bundle path resolves");
    check(!files.resolve("/var/mobile/data/level.txt", true, reason).empty(),
          "the bundle mount accepts writes, as the iOS 3 app directory did");
    // A writable bundle is still a sandbox: nothing outside the mounts is reachable.
    check(files.resolve("/etc/passwd", true, reason).empty(), "paths outside the mounts fail");
    check(reason.find("outside every mounted directory") != std::string::npos,
          "refusal names the sandbox boundary");
    check(files.resolve("/var/mobile/Documents/save.dat", true, reason).find("container") !=
              std::string::npos,
          "the container mount is writable and wins by longest prefix");
    check(files.resolve("/private/var/mobile/x", false, reason).empty(),
          "a path outside every mount is refused");
    check(files.resolve("/var/mobile/../etc/passwd", false, reason).empty(),
          "parent references are refused");
    check(files.resolve("relative/path", false, reason).empty(), "relative paths are refused");
    check(files.resolve("/var/mobile/escape", false, reason).empty(),
          "a symlink inside the bundle is not followed");
    check(reason.find("symbolic link") != std::string::npos, "the refusal names the symlink");
    check(files.describe().find("/var/mobile (read-write)") != std::string::npos,
          "mount description names the access mode");

    Stdio stdio(memory, heap, files);
    std::vector<std::string> log;
    stdio.setLogSink([&](const std::string& text) { log.push_back(text); });
    check(stdio.openFiles() == 3, "three standard streams exist");
    check(memory.read32(stdio.standardCell(1)) == 0x46494c45,
          "the __stdoutp cell holds a FILE object");
    Call call{memory, stdio, {}};

    // Reading a real bundle file through the guest ABI.
    put(memory, 0x1000, "/var/mobile/data/level.txt");
    put(memory, 0x1100, "rb");
    const uint32_t file = call("_fopen", 0x1000, 0x1100);
    check(file != 0, "fopen opens a bundle file read-only");
    check(call("_fread", 0x2000, 1, 32, file) == 9, "fread reports the items read");
    check(readText(memory, 0x2000, 9) == "level one", "fread copies the real file bytes");
    check(call("_feof", file) != 0, "the stream reports end of file");
    check(call("_fseek", file, 0, 0) == 0, "fseek rewinds");
    check(call("_ftell", file) == 0, "ftell reports the rewound position");
    check(call("_fgetc", file) == 'l', "fgetc reads one byte");
    check(call("_fclose", file) == 0, "fclose closes the file");
    fault("STDIO_BAD_FILE", [&] { call("_fread", 0x2000, 1, 4, file); });

    // Writing through the writable container, then reading it back from the host.
    put(memory, 0x1200, "/var/mobile/Documents/save.dat");
    put(memory, 0x1300, "wb");
    const uint32_t out = call("_fopen", 0x1200, 0x1300);
    check(out != 0, "fopen creates a file in the container");
    put(memory, 0x3000, "save data");
    check(call("_fwrite", 0x3000, 1, 9, out) == 9, "fwrite reports the items written");
    check(call("_fclose", out) == 0, "the written file closes");
    check(hostText(root / "container" / "save.dat") == "save data",
          "the guest write reached the mounted host directory");

    // A title may create files beside its own resources, as iOS 3 allowed: AngryBirds
    // writes data/bundleIndex.idx on first run and treats a failure as fatal.
    put(memory, 0x1400, "/var/mobile/data/cache.dat");
    log.clear();
    const uint32_t cache = call("_fopen", 0x1400, 0x1300);
    check(cache != 0, "fopen creates a file inside the bundle");
    put(memory, 0x3100, "cache");
    check(call("_fwrite", 0x3100, 1, 5, cache) == 5, "the bundle write succeeds");
    check(call("_fclose", cache) == 0, "the bundle file closes");
    check(hostText(root / "bundle" / "data" / "cache.dat") == "cache",
          "the guest write reached the host bundle directory");
    // Leaving the sandbox is still refused.
    put(memory, 0x1600, "/var/mobile/../escape.dat");
    log.clear();
    check(call("_fopen", 0x1600, 0x1300) == 0, "a parent reference still leaves no way out");

    // Formatted output to a standard stream and to a file.
    put(memory, 0x1500, "boot=%d");
    check(call("_fprintf", stdio.standardCell(1), 0x1500, 7) == 6, "fprintf to stdout formats");
    check(!log.empty() && log.back() == "boot=7", "stdout output reaches the log sink");
    check(stdio.openFiles() == 3, "standard streams stay open");
    check(call("_remove", 0x1200) == 0, "remove deletes a container file");
    check(!std::filesystem::exists(root / "container" / "save.dat"), "the file is gone");

    // A stale FILE object is refused instead of being dereferenced.
    fault("STDIO_BAD_FILE", [&] { call("_fclose", 0x4000); });
    fault("STDIO_BAD_FILE", [&] { call("_fflush", 0x1234); });
    stdio.closeAll();
    std::filesystem::remove_all(root);
    std::cout << "fs_test: ok\n";
    return 0;
}
