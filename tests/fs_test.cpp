#include "runtime/fs.h"

#include <sys/stat.h>
#include <zlib.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
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
void put16(std::ofstream& output, uint16_t value) {
    const char bytes[] = {char(value), char(value >> 8)};
    output.write(bytes, sizeof(bytes));
}
void put32(std::ofstream& output, uint32_t value) {
    const char bytes[] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
    output.write(bytes, sizeof(bytes));
}
std::string deflateRaw(const std::string& input) {
    z_stream stream{};
    assert(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                        Z_DEFAULT_STRATEGY) == Z_OK);
    std::string output(compressBound(uLong(input.size())), '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = uInt(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = uInt(output.size());
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    output.resize(stream.total_out);
    assert(deflateEnd(&stream) == Z_OK);
    return output;
}
struct ZipItem {
    std::string name, data;
    uint16_t method;
};
void writeZip(const std::filesystem::path& path, const std::vector<ZipItem>& items) {
    struct DirectoryItem {
        std::string name, compressed;
        uint32_t crc = 0, offset = 0;
        uint16_t method = 0;
        uint32_t plainSize = 0;
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    std::vector<DirectoryItem> directory;
    for (const ZipItem& item : items) {
        DirectoryItem entry;
        entry.name = item.name;
        entry.method = item.method;
        entry.plainSize = uint32_t(item.data.size());
        entry.compressed = item.method == 8 ? deflateRaw(item.data) : item.data;
        entry.crc = uint32_t(
            crc32(0L, reinterpret_cast<const Bytef*>(item.data.data()), uInt(item.data.size())));
        entry.offset = uint32_t(output.tellp());
        put32(output, 0x04034b50);
        put16(output, 20);
        put16(output, 0);
        put16(output, entry.method);
        put16(output, 0);
        put16(output, 0);
        put32(output, entry.crc);
        put32(output, uint32_t(entry.compressed.size()));
        put32(output, entry.plainSize);
        put16(output, uint16_t(entry.name.size()));
        put16(output, 0);
        output.write(entry.name.data(), std::streamsize(entry.name.size()));
        output.write(entry.compressed.data(), std::streamsize(entry.compressed.size()));
        directory.push_back(std::move(entry));
    }
    const uint32_t centralOffset = uint32_t(output.tellp());
    for (const DirectoryItem& entry : directory) {
        put32(output, 0x02014b50);
        put16(output, 20);
        put16(output, 20);
        put16(output, 0);
        put16(output, entry.method);
        put16(output, 0);
        put16(output, 0);
        put32(output, entry.crc);
        put32(output, uint32_t(entry.compressed.size()));
        put32(output, entry.plainSize);
        put16(output, uint16_t(entry.name.size()));
        put16(output, 0);
        put16(output, 0);
        put16(output, 0);
        put16(output, 0);
        put32(output, 0);
        put32(output, entry.offset);
        output.write(entry.name.data(), std::streamsize(entry.name.size()));
    }
    const uint32_t centralSize = uint32_t(output.tellp()) - centralOffset;
    put32(output, 0x06054b50);
    put16(output, 0);
    put16(output, 0);
    put16(output, uint16_t(directory.size()));
    put16(output, uint16_t(directory.size()));
    put32(output, centralSize);
    put32(output, centralOffset);
    put16(output, 0);
    assert(output.good());
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
    std::filesystem::create_symlink("data", root / "bundle" / "internal-data", ignored);

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
    check(files.resolve("/var/mobile/internal-data/level.txt", false, reason) == level,
          "a symlink that resolves inside its mount is safely canonicalized");
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
          "a symlink cannot escape the mounted bundle");
    check(reason.find("symbolic link") != std::string::npos,
          "the refusal names the escaping symlink");
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

    // fopen follows an alias only when its canonical destination remains inside the mount.
    put(memory, 0x1700, "/var/mobile/internal-data/level.txt");
    const uint32_t aliasFile = call("_fopen", 0x1700, 0x1100);
    check(aliasFile != 0, "fopen accepts an in-mount symlink to a bundle resource");
    check(call("_fclose", aliasFile) == 0, "the aliased stream closes normally");
    put(memory, 0x1700, "/var/mobile/escape");
    check(call("_fopen", 0x1700, 0x1100) == 0,
          "fopen still refuses a symlink whose target escapes the mount");

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
    put(memory, 0x1500, "raw stdout");
    check(call("_fwrite", 0x1500, 1, 10, stdio.standardCell(1)) == 10,
          "fwrite to stdout reports the requested byte count");
    check(log.back() == "raw stdout", "fwrite to stdout is captured instead of using host stdout");
    check(stdio.openFiles() == 3, "standard streams stay open");
    check(call("_remove", 0x1200) == 0, "remove deletes a container file");
    check(!std::filesystem::exists(root / "container" / "save.dat"), "the file is gone");

    // The IPA is mounted as a ZIP: opening one compressed member populates only that cache file.
    const auto ipa = root / "sample.ipa";
    const std::string zippedLevel = "archive-compressed-level";
    writeZip(ipa, {{"Payload/Example.app/data/level.txt", zippedLevel, 8},
                   {"Payload/Example.app/data/unused.bin", "not opened", 0},
                   {"Payload/Other.app/ignored.txt", "other app", 0}});
    const std::string originalIpaBytes = [&] {
        std::ifstream input(ipa, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    const auto zipCache = root / "zip-cache";
    const auto zipOverlay = root / "zip-overlay";
    std::filesystem::create_directories(zipCache);
    std::filesystem::create_directories(zipOverlay);
    const uint64_t ipaSize = std::filesystem::file_size(ipa);
    FileSystem zipFiles;
    zipFiles.mountArchive("/var/mobile", ipa.string(), "Payload/Example.app/", zipCache.string(),
                          zipOverlay.string(), true);
    Memory zipMemory;
    std::vector<uint8_t> zipPage(65536, 0);
    zipMemory.map({0x1000, 65536, uint8_t(Read | Write), "archive guest data"}, zipPage.data(),
                  zipPage.size());
    GuestHeap zipHeap(zipMemory);
    Stdio zipStdio(zipMemory, zipHeap, zipFiles);
    Call zipCall{zipMemory, zipStdio, {}};
    put(zipMemory, 0x5000, "/var/mobile/data/level.txt");
    put(zipMemory, 0x5100, "rb");
    const uint32_t zippedFile = zipCall("_fopen", 0x5000, 0x5100);
    check(zippedFile != 0, "fopen reads a ZIP-backed app member");
    check(zipCall("_fread", 0x5200, 1, uint32_t(zippedLevel.size()), zippedFile) ==
              zippedLevel.size(),
          "fread returns decompressed archive bytes");
    check(readText(zipMemory, 0x5200, zippedLevel.size()) == zippedLevel,
          "DEFLATE member contents reach the guest memory");
    check(zipCall("_fclose", zippedFile) == 0, "ZIP-backed guest stream closes");
    check(hostText(zipCache / "data" / "level.txt") == zippedLevel,
          "the opened IPA member is cached and CRC-verified");
    check(!std::filesystem::exists(zipCache / "data" / "unused.bin"),
          "an unopened IPA member is not extracted");

    std::string zipReason;
    const std::string writableMember =
        zipFiles.resolve("/var/mobile/data/level.txt", true, zipReason, false);
    check(!writableMember.empty() && writableMember.find("zip-overlay") != std::string::npos,
          "opening an archive member for writing creates a private copy-on-write file");
    std::ofstream(writableMember, std::ios::binary | std::ios::trunc) << "modified by guest";
    check(hostText(zipFiles.resolve("/var/mobile/data/level.txt", false, zipReason)) ==
              "modified by guest",
          "bundle reads prefer the writable overlay over the IPA member");
    check(zipFiles.rename("/var/mobile/data/level.txt", "/var/mobile/data/renamed.txt", zipReason),
          "renaming an IPA member moves its copy-on-write data into the overlay");
    check(zipFiles.resolve("/var/mobile/data/level.txt", false, zipReason).empty(),
          "renaming an IPA member leaves a whiteout at its old path");
    check(hostText(zipFiles.resolve("/var/mobile/data/renamed.txt", false, zipReason)) ==
              "modified by guest",
          "the renamed overlay file preserves guest writes");
    check(zipFiles.remove("/var/mobile/data/renamed.txt", zipReason),
          "remove deletes an overlay copy without touching the source archive");
    check(!zipFiles.rename("/var/mobile/data/level.txt", "/var/mobile/data/revived.txt", zipReason),
          "rename cannot resurrect an archive member hidden by a whiteout");
    const std::string newMember =
        zipFiles.resolve("/var/mobile/data/bundleIndex.idx", true, zipReason, true);
    check(!newMember.empty() && newMember.find("zip-overlay") != std::string::npos,
          "truncate can create a new bundle file without extracting an archive member");
    std::ofstream(newMember, std::ios::binary) << "index";
    check(!zipFiles.resolve("/var/mobile/data/bundleIndex.idx", false, zipReason).empty(),
          "new bundle files are visible to subsequent guest reads");
    check(std::filesystem::file_size(ipa) == ipaSize, "guest writes never change the IPA size");
    {
        std::ifstream input(ipa, std::ios::binary);
        const std::string currentIpaBytes{std::istreambuf_iterator<char>(input),
                                          std::istreambuf_iterator<char>()};
        check(currentIpaBytes == originalIpaBytes, "guest writes leave the source IPA unchanged");
    }
    zipStdio.closeAll();

    // Malformed archives, unsafe member paths, and CRC damage fail closed.
    const auto brokenArchive = root / "broken.ipa";
    std::ofstream(brokenArchive, std::ios::binary) << "not a ZIP";
    FileSystem brokenFiles;
    fault("FS_ARCHIVE", [&] {
        brokenFiles.mountArchive("/var/mobile", brokenArchive.string(), "Payload/Example.app/",
                                 (root / "broken-cache").string(),
                                 (root / "broken-overlay").string(), true);
    });
    const auto unsafeArchive = root / "unsafe.ipa";
    writeZip(unsafeArchive, {{"Payload/Example.app/../../escape", "unsafe", 0}});
    FileSystem unsafeFiles;
    fault("FS_ARCHIVE", [&] {
        unsafeFiles.mountArchive("/var/mobile", unsafeArchive.string(), "Payload/Example.app/",
                                 (root / "unsafe-cache").string(),
                                 (root / "unsafe-overlay").string(), true);
    });
    const auto crcArchive = root / "bad-crc.ipa";
    writeZip(crcArchive, {{"Payload/Example.app/data/crc.bin", "CRC-protected", 0}});
    {
        std::fstream damaged(crcArchive, std::ios::in | std::ios::out | std::ios::binary);
        std::array<char, 30> header{};
        damaged.read(header.data(), header.size());
        const uint16_t nameLength = uint8_t(header[26]) | uint16_t(uint8_t(header[27])) << 8;
        const uint16_t extraLength = uint8_t(header[28]) | uint16_t(uint8_t(header[29])) << 8;
        damaged.seekg(std::streamoff(header.size() + nameLength + extraLength));
        char byte = 0;
        damaged.read(&byte, 1);
        byte ^= 1;
        damaged.seekp(std::streamoff(header.size() + nameLength + extraLength));
        damaged.write(&byte, 1);
        assert(damaged.good());
    }
    const auto crcCache = root / "crc-cache";
    const auto crcOverlay = root / "crc-overlay";
    std::filesystem::create_directories(crcCache);
    std::filesystem::create_directories(crcOverlay);
    FileSystem crcFiles;
    crcFiles.mountArchive("/var/mobile", crcArchive.string(), "Payload/Example.app/",
                          crcCache.string(), crcOverlay.string(), true);
    check(crcFiles.resolve("/var/mobile/data/crc.bin", false, zipReason).empty() &&
              zipReason.find("CRC") != std::string::npos,
          "corrupted member bytes are rejected instead of cached");

    // A stale FILE object is refused instead of being dereferenced.
    fault("STDIO_BAD_FILE", [&] { call("_fclose", 0x4000); });
    fault("STDIO_BAD_FILE", [&] { call("_fflush", 0x1234); });
    stdio.closeAll();
    std::filesystem::remove_all(root);
    std::cout << "fs_test: ok\n";
    return 0;
}
