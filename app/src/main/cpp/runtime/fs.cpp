#include "fs.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <system_error>

#include "error.h"

namespace podnative::runtime {
namespace {
constexpr size_t PathLimit = 4096;
constexpr uint64_t MaximumArchiveEntries = 30000;
constexpr uint64_t MaximumArchiveEntryBytes = 4ULL * 1024 * 1024 * 1024;
constexpr uint64_t MaximumCentralDirectoryBytes = 64ULL * 1024 * 1024;
constexpr size_t IoBufferBytes = 64 * 1024;
bool boundary(const std::string& path, size_t length) {
    if (length > path.size()) return false;
    return path.size() == length || path[length] == '/';
}
uint16_t little16(const uint8_t* bytes) { return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8; }
uint32_t little32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 |
           uint32_t(bytes[3]) << 24;
}
uint64_t little64(const uint8_t* bytes) {
    return uint64_t(little32(bytes)) | uint64_t(little32(bytes + 4)) << 32;
}
bool readAt(std::ifstream& input, uint64_t fileSize, uint64_t offset, uint8_t* output,
            size_t count) {
    if (!count) return true;
    if (offset > fileSize || uint64_t(count) > fileSize - offset ||
        offset > uint64_t(std::numeric_limits<std::streamoff>::max()) ||
        count > size_t(std::numeric_limits<std::streamsize>::max()))
        return false;
    input.clear();
    input.seekg(std::streamoff(offset), std::ios::beg);
    if (!input) return false;
    input.read(reinterpret_cast<char*>(output), std::streamsize(count));
    return input.gcount() == std::streamsize(count);
}

bool safeComponents(const std::string& path, bool trailingSlash) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos)
        return false;
    if (trailingSlash != (path.back() == '/')) return false;
    const size_t end = trailingSlash ? path.size() - 1 : path.size();
    if (!end) return false;
    size_t begin = 0;
    while (begin < end) {
        const size_t slash = path.find('/', begin);
        const size_t stop = slash == std::string::npos || slash >= end ? end : slash;
        const std::string part = path.substr(begin, stop - begin);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos)
            return false;
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7f) return false;
        begin = stop + 1;
    }
    return true;
}

std::filesystem::path canonicalRoot(const std::string& path, const char* label) {
    if (path.empty()) throw Error("FS_MOUNT", std::string(label) + " is empty");
    std::error_code error;
    const auto result = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_directory(result, error) || error)
        throw Error("FS_MOUNT", std::string(label) + " is not a safe directory");
    return result;
}

uint64_t zip64Field(const std::vector<uint8_t>& extra, uint64_t& at) {
    if (at > extra.size() || extra.size() - size_t(at) < 8)
        throw Error("FS_ARCHIVE", "truncated ZIP64 size field");
    const uint64_t value = little64(extra.data() + size_t(at));
    at += 8;
    return value;
}
uint32_t zip64DiskField(const std::vector<uint8_t>& extra, uint64_t& at) {
    if (at > extra.size() || extra.size() - size_t(at) < 4)
        throw Error("FS_ARCHIVE", "truncated ZIP64 disk field");
    const uint32_t value = little32(extra.data() + size_t(at));
    at += 4;
    return value;
}

struct CentralDirectory {
    uint64_t entries = 0, size = 0, offset = 0, eocdOffset = 0;
};
CentralDirectory findCentralDirectory(std::ifstream& input, uint64_t fileSize) {
    if (fileSize < 22) throw Error("FS_ARCHIVE", "ZIP end record is missing");
    const uint64_t tailSize64 = std::min<uint64_t>(fileSize, 22 + 65535);
    std::vector<uint8_t> tail(static_cast<size_t>(tailSize64));
    const uint64_t tailOffset = fileSize - tailSize64;
    if (!readAt(input, fileSize, tailOffset, tail.data(), tail.size()))
        throw Error("FS_ARCHIVE", "ZIP end record could not be read");
    size_t endRecord = std::string::npos;
    for (size_t pos = tail.size() - 22;; pos--) {
        if (little32(tail.data() + pos) == 0x06054b50 &&
            pos + 22 + little16(tail.data() + pos + 20) == tail.size()) {
            endRecord = pos;
            break;
        }
        if (!pos) break;
    }
    if (endRecord == std::string::npos)
        throw Error("FS_ARCHIVE", "ZIP end record or archive comment is malformed");
    const uint8_t* eocd = tail.data() + endRecord;
    const uint16_t disk = little16(eocd + 4), centralDisk = little16(eocd + 6);
    const uint16_t entriesOnDisk16 = little16(eocd + 8), entries16 = little16(eocd + 10);
    const uint32_t centralSize32 = little32(eocd + 12), centralOffset32 = little32(eocd + 16);
    CentralDirectory result;
    result.eocdOffset = tailOffset + endRecord;
    if (disk || centralDisk) throw Error("FS_ARCHIVE", "multi-disk ZIP archives are not supported");
    result.entries = entries16;
    result.size = centralSize32;
    result.offset = centralOffset32;
    const bool needsZip64 = entriesOnDisk16 == 0xffff || entries16 == 0xffff ||
                            centralSize32 == 0xffffffff || centralOffset32 == 0xffffffff;
    if (needsZip64) {
        if (result.eocdOffset < 20) throw Error("FS_ARCHIVE", "ZIP64 locator is missing");
        std::array<uint8_t, 20> locator{};
        if (!readAt(input, fileSize, result.eocdOffset - locator.size(), locator.data(),
                    locator.size()) ||
            little32(locator.data()) != 0x07064b50 || little32(locator.data() + 4) != 0 ||
            little32(locator.data() + 16) != 1)
            throw Error("FS_ARCHIVE", "ZIP64 locator is invalid");
        const uint64_t zip64Offset = little64(locator.data() + 8);
        std::array<uint8_t, 56> record{};
        if (!readAt(input, fileSize, zip64Offset, record.data(), record.size()) ||
            little32(record.data()) != 0x06064b50 || little64(record.data() + 4) < 44 ||
            little32(record.data() + 16) != 0 || little32(record.data() + 20) != 0 ||
            little64(record.data() + 24) != little64(record.data() + 32))
            throw Error("FS_ARCHIVE", "ZIP64 end record is invalid");
        result.entries = little64(record.data() + 32);
        result.size = little64(record.data() + 40);
        result.offset = little64(record.data() + 48);
    } else if (entriesOnDisk16 != entries16) {
        throw Error("FS_ARCHIVE", "split ZIP central directory is inconsistent");
    }
    if (result.entries > MaximumArchiveEntries || result.size > MaximumCentralDirectoryBytes ||
        result.offset > fileSize || result.size > fileSize - result.offset ||
        result.offset + result.size > result.eocdOffset)
        throw Error("FS_ARCHIVE", "ZIP central directory exceeds its safe limits");
    return result;
}

}  // namespace

bool FileSystem::containedBy(const std::filesystem::path& root,
                             const std::filesystem::path& target) {
    auto rootPart = root.begin();
    auto targetPart = target.begin();
    for (; rootPart != root.end(); ++rootPart, ++targetPart)
        if (targetPart == target.end() || *rootPart != *targetPart) return false;
    return true;
}

bool FileSystem::safeArchivePath(const std::string& path, bool trailingSlash) {
    return safeComponents(path, trailingSlash);
}

std::filesystem::path FileSystem::safeTarget(const std::filesystem::path& root,
                                             const std::string& relative, std::string& reason) {
    if (!safeArchivePath(relative, false)) {
        reason = "archive path contains an unsafe component";
        return {};
    }
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(root / relative, error);
    if (error || !containedBy(root, target)) {
        reason = "archive path resolves outside its private mount";
        return {};
    }
    return target;
}

std::filesystem::path FileSystem::whiteoutPath(const std::filesystem::path& root,
                                               const std::string& relative) {
    return root / ".anyios-whiteouts" / relative;
}

bool FileSystem::createWhiteout(const std::filesystem::path& root, const std::string& relative,
                                std::string& reason) {
    const auto marker = whiteoutPath(root, relative);
    std::error_code error;
    std::filesystem::create_directories(marker.parent_path(), error);
    if (error) {
        reason = "could not create a bundle-overlay deletion marker";
        return false;
    }
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    if (!output) {
        reason = "could not write a bundle-overlay deletion marker";
        return false;
    }
    output.close();
    if (!output) {
        reason = "could not finish a bundle-overlay deletion marker";
        return false;
    }
    return true;
}

std::string FileSystem::archivePath(const Mount& mount, const std::string& relative) {
    return mount.archive->prefix + relative;
}

bool FileSystem::validGuestPath(const std::string& path, std::string& reason) {
    if (path.empty() || path[0] != '/') {
        reason = "guest path is not absolute";
        return false;
    }
    if (path.size() > PathLimit) {
        reason = "guest path is longer than the sandbox limit";
        return false;
    }
    size_t at = 0;
    while (at <= path.size()) {
        const size_t end = path.find('/', at);
        const std::string part = path.substr(at, end == std::string::npos ? end : end - at);
        if (part == "..") {
            reason = "guest path contains a parent reference";
            return false;
        }
        if (part == ".") {
            reason = "guest path contains a current-directory reference";
            return false;
        }
        for (char c : part)
            if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
                reason = "guest path contains a control or separator character";
                return false;
            }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return true;
}

std::shared_ptr<FileSystem::ArchiveIndex> FileSystem::ArchiveIndex::open(
    const std::string& path, const std::string& prefix) {
    if (!safeComponents(prefix, true))
        throw Error("FS_MOUNT", "IPA app root contains an unsafe path");
    auto archive = std::make_shared<ArchiveIndex>();
    archive->archivePath = path;
    archive->prefix = prefix;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error("FS_ARCHIVE", "IPA source archive could not be opened");
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0) throw Error("FS_ARCHIVE", "IPA source size could not be read");
    archive->archiveSize = uint64_t(length);
    const CentralDirectory central = findCentralDirectory(input, archive->archiveSize);
    uint64_t cursor = central.offset;
    const uint64_t centralEnd = central.offset + central.size;
    for (uint64_t index = 0; index < central.entries; index++) {
        std::array<uint8_t, 46> fixed{};
        if (cursor > centralEnd || centralEnd - cursor < fixed.size() ||
            !readAt(input, archive->archiveSize, cursor, fixed.data(), fixed.size()) ||
            little32(fixed.data()) != 0x02014b50)
            throw Error("FS_ARCHIVE", "ZIP central directory entry is truncated or malformed");
        const uint16_t flags = little16(fixed.data() + 8), method = little16(fixed.data() + 10);
        const uint32_t crc = little32(fixed.data() + 16);
        const uint32_t compressed32 = little32(fixed.data() + 20);
        const uint32_t uncompressed32 = little32(fixed.data() + 24);
        const uint16_t nameLength = little16(fixed.data() + 28);
        const uint16_t extraLength = little16(fixed.data() + 30);
        const uint16_t commentLength = little16(fixed.data() + 32);
        const uint16_t diskStart16 = little16(fixed.data() + 34);
        const uint32_t localOffset32 = little32(fixed.data() + 42);
        const uint64_t variableLength = uint64_t(nameLength) + extraLength + commentLength;
        if (!nameLength || nameLength > 4096 || variableLength > centralEnd - cursor - fixed.size())
            throw Error("FS_ARCHIVE", "ZIP central directory entry has invalid lengths");
        std::vector<uint8_t> variable(static_cast<size_t>(variableLength));
        if (!readAt(input, archive->archiveSize, cursor + fixed.size(), variable.data(),
                    variable.size()))
            throw Error("FS_ARCHIVE", "ZIP central directory name or extra data is truncated");
        const std::string name(reinterpret_cast<const char*>(variable.data()), nameLength);
        if (name.find('\0') != std::string::npos)
            throw Error("FS_ARCHIVE", "ZIP entry name contains a NUL byte");
        uint64_t compressedSize = compressed32, uncompressedSize = uncompressed32,
                 localOffset = localOffset32;
        uint32_t diskStart = diskStart16;
        const bool needsZip64 = compressed32 == 0xffffffff || uncompressed32 == 0xffffffff ||
                                localOffset32 == 0xffffffff || diskStart16 == 0xffff;
        if (needsZip64) {
            uint64_t extraAt = nameLength;
            const uint64_t extraEnd = uint64_t(nameLength) + extraLength;
            bool foundZip64 = false;
            while (extraAt + 4 <= extraEnd) {
                const uint16_t id = little16(variable.data() + size_t(extraAt));
                const uint16_t size = little16(variable.data() + size_t(extraAt + 2));
                extraAt += 4;
                if (size > extraEnd - extraAt)
                    throw Error("FS_ARCHIVE", "ZIP extra field exceeds its entry");
                if (id == 0x0001) {
                    const uint64_t requiredBytes = uint64_t(uncompressed32 == 0xffffffff ? 8 : 0) +
                                                   uint64_t(compressed32 == 0xffffffff ? 8 : 0) +
                                                   uint64_t(localOffset32 == 0xffffffff ? 8 : 0) +
                                                   uint64_t(diskStart16 == 0xffff ? 4 : 0);
                    if (requiredBytes > size)
                        throw Error("FS_ARCHIVE", "ZIP64 entry extra field is truncated");
                    uint64_t dataAt = extraAt;
                    if (uncompressed32 == 0xffffffff)
                        uncompressedSize = zip64Field(variable, dataAt);
                    if (compressed32 == 0xffffffff) compressedSize = zip64Field(variable, dataAt);
                    if (localOffset32 == 0xffffffff) localOffset = zip64Field(variable, dataAt);
                    if (diskStart16 == 0xffff) diskStart = zip64DiskField(variable, dataAt);
                    foundZip64 = true;
                    break;
                }
                extraAt += size;
            }
            if (!foundZip64) throw Error("FS_ARCHIVE", "ZIP64 entry sizes are missing");
        }
        if (diskStart != 0) throw Error("FS_ARCHIVE", "multi-disk ZIP entries are not supported");
        cursor += fixed.size() + variableLength;
        const bool directory = !name.empty() && name.back() == '/';
        if (directory || name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string relative = name.substr(prefix.size());
        if (!safeComponents(relative, false))
            throw Error("FS_ARCHIVE", "IPA app contains an unsafe resource path");
        if (compressedSize > archive->archiveSize || localOffset >= archive->archiveSize ||
            uncompressedSize > MaximumArchiveEntryBytes)
            throw Error("FS_ARCHIVE", "IPA app resource exceeds the archive safety limit");
        ArchiveEntry entry;
        entry.name = name;
        entry.flags = flags;
        entry.method = method;
        entry.crc = crc;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.localHeaderOffset = localOffset;
        if (!archive->entries.emplace(relative, std::move(entry)).second)
            throw Error("FS_ARCHIVE", "IPA app contains duplicate resource names");
    }
    if (cursor != centralEnd)
        throw Error("FS_ARCHIVE", "ZIP central directory length does not match its entries");
    return archive;
}

const FileSystem::ArchiveEntry* FileSystem::ArchiveIndex::find(const std::string& relative) const {
    auto found = entries.find(relative);
    return found == entries.end() ? nullptr : &found->second;
}

bool FileSystem::ArchiveIndex::extract(const ArchiveEntry& entry,
                                       const std::filesystem::path& destination,
                                       const std::filesystem::path& root,
                                       std::string& reason) const {
    if (entry.flags & (1 | 0x40)) {
        reason = "IPA resource is encrypted; the runtime does not decrypt archives";
        return false;
    }
    if (entry.method != 0 && entry.method != 8) {
        reason = "IPA resource uses an unsupported ZIP compression method";
        return false;
    }
    std::error_code error;
    const auto parent = destination.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) {
        reason = "could not create the private IPA resource cache directory";
        return false;
    }
    const auto canonicalParent = std::filesystem::weakly_canonical(parent, error);
    if (error || !containedBy(root, canonicalParent)) {
        reason = "IPA resource cache parent resolves outside its private directory";
        return false;
    }
    static std::atomic<uint64_t> nextTemp{1};
    std::filesystem::path temporary = destination;
    temporary += ".pending-" + std::to_string(nextTemp.fetch_add(1, std::memory_order_relaxed));
    std::ifstream input(archivePath, std::ios::binary);
    if (!input) {
        reason = "IPA source archive could not be reopened";
        return false;
    }
    std::array<uint8_t, 30> local{};
    if (!readAt(input, archiveSize, entry.localHeaderOffset, local.data(), local.size()) ||
        little32(local.data()) != 0x04034b50 || little16(local.data() + 8) != entry.method ||
        (little16(local.data() + 6) & (1 | 0x40))) {
        reason = "IPA local resource header is invalid or encrypted";
        return false;
    }
    const uint16_t localNameLength = little16(local.data() + 26);
    const uint16_t localExtraLength = little16(local.data() + 28);
    const uint64_t dataOffset =
        entry.localHeaderOffset + local.size() + localNameLength + localExtraLength;
    if (dataOffset > archiveSize || entry.compressedSize > archiveSize - dataOffset) {
        reason = "IPA compressed resource is truncated";
        return false;
    }
    std::vector<uint8_t> localName(localNameLength);
    if (!readAt(input, archiveSize, entry.localHeaderOffset + local.size(), localName.data(),
                localName.size()) ||
        std::string(reinterpret_cast<const char*>(localName.data()), localName.size()) !=
            entry.name) {
        reason = "IPA local and central resource names disagree";
        return false;
    }
    input.clear();
    input.seekg(std::streamoff(dataOffset), std::ios::beg);
    if (!input) {
        reason = "IPA compressed resource could not be positioned";
        return false;
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        reason = "could not open the private IPA resource cache";
        return false;
    }
    std::array<uint8_t, IoBufferBytes> inBuffer{}, outBuffer{};
    uint64_t compressedRead = 0, compressedConsumed = 0, uncompressedWritten = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    bool success = true;
    if (entry.method == 0) {
        if (entry.compressedSize != entry.uncompressedSize) {
            reason = "stored IPA resource has inconsistent compressed and plain sizes";
            success = false;
        }
        while (success && compressedRead < entry.compressedSize) {
            const size_t amount =
                size_t(std::min<uint64_t>(inBuffer.size(), entry.compressedSize - compressedRead));
            input.read(reinterpret_cast<char*>(inBuffer.data()), std::streamsize(amount));
            if (input.gcount() != std::streamsize(amount)) {
                reason = "stored IPA resource ended before its declared size";
                success = false;
                break;
            }
            output.write(reinterpret_cast<const char*>(inBuffer.data()), std::streamsize(amount));
            if (!output) {
                reason = "could not write the private IPA resource cache";
                success = false;
                break;
            }
            crc = crc32(crc, inBuffer.data(), uInt(amount));
            compressedRead += amount;
            compressedConsumed += amount;
            uncompressedWritten += amount;
        }
    } else {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            reason = "could not initialize the IPA DEFLATE decoder";
            success = false;
        } else {
            int status = Z_OK;
            while (success && status != Z_STREAM_END) {
                if (!stream.avail_in && compressedRead < entry.compressedSize) {
                    const size_t amount = size_t(
                        std::min<uint64_t>(inBuffer.size(), entry.compressedSize - compressedRead));
                    input.read(reinterpret_cast<char*>(inBuffer.data()), std::streamsize(amount));
                    if (input.gcount() != std::streamsize(amount)) {
                        reason = "compressed IPA resource ended before its declared size";
                        success = false;
                        break;
                    }
                    stream.next_in = inBuffer.data();
                    stream.avail_in = uInt(amount);
                    compressedRead += amount;
                }
                stream.next_out = outBuffer.data();
                stream.avail_out = uInt(outBuffer.size());
                const uInt beforeInput = stream.avail_in;
                status = inflate(&stream, Z_NO_FLUSH);
                const size_t consumed = size_t(beforeInput - stream.avail_in);
                const size_t produced = outBuffer.size() - stream.avail_out;
                compressedConsumed += consumed;
                if (produced) {
                    if (uint64_t(produced) >
                        entry.uncompressedSize -
                            std::min(uncompressedWritten, entry.uncompressedSize)) {
                        reason = "decompressed IPA resource exceeds its declared size";
                        success = false;
                        break;
                    }
                    output.write(reinterpret_cast<const char*>(outBuffer.data()),
                                 std::streamsize(produced));
                    if (!output) {
                        reason = "could not write the private IPA resource cache";
                        success = false;
                        break;
                    }
                    crc = crc32(crc, outBuffer.data(), uInt(produced));
                    uncompressedWritten += produced;
                }
                if (status != Z_OK && status != Z_STREAM_END) {
                    reason = "IPA DEFLATE data is invalid or incomplete";
                    success = false;
                    break;
                }
                if (status == Z_OK && !consumed && !produced &&
                    compressedRead == entry.compressedSize && !stream.avail_in) {
                    reason = "IPA DEFLATE stream ended before its final block";
                    success = false;
                }
            }
            inflateEnd(&stream);
            if (success && (compressedConsumed != entry.compressedSize ||
                            uncompressedWritten != entry.uncompressedSize)) {
                reason = "IPA DEFLATE output size does not match its directory record";
                success = false;
            }
        }
    }
    output.flush();
    if (!output) {
        reason = "could not finish the private IPA resource cache";
        success = false;
    }
    output.close();
    if (success && uncompressedWritten != entry.uncompressedSize) {
        reason = "IPA resource size does not match its directory record";
        success = false;
    }
    if (success && uint32_t(crc) != entry.crc) {
        reason = "IPA resource CRC does not match its directory record";
        success = false;
    }
    if (!success) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) {
        std::filesystem::remove(temporary, error);
        reason = "could not publish the verified IPA resource cache file";
        return false;
    }
    return true;
}

std::string FileSystem::ArchiveIndex::materialize(const std::string& relative,
                                                  const ArchiveEntry& entry,
                                                  const std::filesystem::path& root,
                                                  std::string& reason) const {
    const auto destination = FileSystem::safeTarget(root, relative, reason);
    if (destination.empty()) return {};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination, error);
    if (!error && std::filesystem::is_regular_file(status)) {
        const uint64_t size = std::filesystem::file_size(destination, error);
        if (!error && size == entry.uncompressedSize) return destination.string();
        error.clear();
        std::filesystem::remove(destination, error);
        error.clear();
    }
    if (!extract(entry, destination, root, reason)) return {};
    return destination.string();
}

void FileSystem::mount(std::string guestRoot, std::string hostRoot, bool writable) {
    std::string reason;
    if (!validGuestPath(guestRoot, reason)) throw Error("FS_MOUNT", reason);
    if (guestRoot.size() > 1 && guestRoot.back() == '/') guestRoot.pop_back();
    const auto root = canonicalRoot(hostRoot, "mounted host root");
    for (const auto& existing : mounts)
        if (existing.guestRoot == guestRoot) throw Error("FS_MOUNT", "duplicate guest mount");
    Mount mount;
    mount.guestRoot = std::move(guestRoot);
    mount.hostRoot = root.string();
    mount.writable = writable;
    mount.kind = Mount::Kind::Directory;
    mount.canonicalRoot = root;
    mounts.push_back(std::move(mount));
    std::sort(mounts.begin(), mounts.end(), [](const Mount& a, const Mount& b) {
        return a.guestRoot.size() > b.guestRoot.size();
    });
}

void FileSystem::mountArchive(std::string guestRoot, std::string archivePath,
                              std::string archivePrefix, std::string cacheRoot,
                              std::string overlayRoot, bool writable) {
    std::string reason;
    if (!validGuestPath(guestRoot, reason)) throw Error("FS_MOUNT", reason);
    if (guestRoot.size() > 1 && guestRoot.back() == '/') guestRoot.pop_back();
    if (!writable) throw Error("FS_MOUNT", "IPA app mounts need a private write overlay");
    auto archive = ArchiveIndex::open(archivePath, archivePrefix);
    std::error_code error;
    std::filesystem::create_directories(cacheRoot, error);
    if (error) throw Error("FS_MOUNT", "could not create the IPA resource cache directory");
    std::filesystem::create_directories(overlayRoot, error);
    if (error) throw Error("FS_MOUNT", "could not create the app bundle write overlay");
    const auto canonicalCache = canonicalRoot(cacheRoot, "IPA resource cache");
    const auto canonicalOverlay = canonicalRoot(overlayRoot, "app bundle write overlay");
    for (const auto& existing : mounts)
        if (existing.guestRoot == guestRoot) throw Error("FS_MOUNT", "duplicate guest mount");
    Mount mount;
    mount.guestRoot = std::move(guestRoot);
    mount.hostRoot = archivePath;
    mount.writable = true;
    mount.kind = Mount::Kind::Archive;
    mount.cacheRoot = canonicalCache;
    mount.overlayRoot = canonicalOverlay;
    mount.archive = std::move(archive);
    mounts.push_back(std::move(mount));
    std::sort(mounts.begin(), mounts.end(), [](const Mount& a, const Mount& b) {
        return a.guestRoot.size() > b.guestRoot.size();
    });
}

std::string FileSystem::resolve(const std::string& guestPath, bool write, std::string& reason,
                                bool truncate) {
    if (!validGuestPath(guestPath, reason)) return {};
    const Mount* chosen = nullptr;
    for (const auto& candidate : mounts) {
        if (!boundary(guestPath, candidate.guestRoot.size())) continue;
        if (!guestPath.compare(0, candidate.guestRoot.size(), candidate.guestRoot)) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        reason = "path is outside every mounted directory";
        return {};
    }
    if (write && !chosen->writable) {
        reason = "mount " + chosen->guestRoot + " is read-only";
        return {};
    }
    std::string tail = guestPath.substr(chosen->guestRoot.size());
    while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
    if (chosen->kind == Mount::Kind::Directory) {
        std::error_code error;
        std::filesystem::path host = chosen->canonicalRoot;
        if (!tail.empty()) host /= tail;
        host = std::filesystem::weakly_canonical(host, error);
        if (error) {
            reason = "guest path cannot be resolved safely inside its mount";
            return {};
        }
        if (!containedBy(chosen->canonicalRoot, host)) {
            reason = "path resolves outside the mount through a symbolic link";
            return {};
        }
        return host.string();
    }
    if (tail.empty() || !safeArchivePath(tail, false)) {
        reason = "guest archive path is empty or contains an unsafe component";
        return {};
    }
    if (tail == ".anyios-whiteouts" || tail.rfind(".anyios-whiteouts/", 0) == 0) {
        reason = "private bundle-overlay bookkeeping is not guest-visible";
        return {};
    }
    const auto whiteout = whiteoutPath(chosen->overlayRoot, tail);
    std::error_code error;
    if (std::filesystem::exists(whiteout, error) && !error) {
        if (!write) {
            reason = "archive resource was removed in the writable bundle overlay";
            return {};
        }
        std::filesystem::remove(whiteout, error);
        if (error) {
            reason = "could not clear the bundle-overlay deletion marker";
            return {};
        }
    }
    const auto overlayTarget = safeTarget(chosen->overlayRoot, tail, reason);
    if (overlayTarget.empty()) return {};
    const auto overlayStatus = std::filesystem::symlink_status(overlayTarget, error);
    if (!error && std::filesystem::is_regular_file(overlayStatus)) return overlayTarget.string();
    error.clear();
    const ArchiveEntry* entry = chosen->archive->find(tail);
    if (write) {
        if (!truncate && entry)
            return chosen->archive->materialize(tail, *entry, chosen->overlayRoot, reason);
        const auto parent = overlayTarget.parent_path();
        std::filesystem::create_directories(parent, error);
        if (error) {
            reason = "could not create the private bundle-overlay directory";
            return {};
        }
        const auto canonicalParent = std::filesystem::weakly_canonical(parent, error);
        if (error || !containedBy(chosen->overlayRoot, canonicalParent)) {
            reason = "bundle-overlay parent resolves outside its private directory";
            return {};
        }
        return overlayTarget.string();
    }
    if (!entry) {
        reason = "path is not present in the IPA app bundle";
        return {};
    }
    return chosen->archive->materialize(tail, *entry, chosen->cacheRoot, reason);
}

bool FileSystem::remove(const std::string& guestPath, std::string& reason) {
    if (!validGuestPath(guestPath, reason)) return false;
    Mount* chosen = nullptr;
    for (auto& candidate : mounts) {
        if (boundary(guestPath, candidate.guestRoot.size()) &&
            !guestPath.compare(0, candidate.guestRoot.size(), candidate.guestRoot)) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        reason = "path is outside every mounted directory";
        return false;
    }
    if (!chosen->writable) {
        reason = "mount " + chosen->guestRoot + " is read-only";
        return false;
    }
    std::string relative = guestPath.substr(chosen->guestRoot.size());
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (chosen->kind == Mount::Kind::Directory) {
        const std::string host = resolve(guestPath, true, reason);
        if (host.empty()) return false;
        std::error_code error;
        const bool removed = std::filesystem::remove(host, error);
        if (error || !removed) {
            reason = error ? error.message() : "file does not exist";
            return false;
        }
        return true;
    }
    if (relative.empty() || !safeArchivePath(relative, false)) {
        reason = "guest archive path is empty or contains an unsafe component";
        return false;
    }
    if (relative == ".anyios-whiteouts" || relative.rfind(".anyios-whiteouts/", 0) == 0) {
        reason = "private bundle-overlay bookkeeping is not guest-visible";
        return false;
    }
    const auto target = safeTarget(chosen->overlayRoot, relative, reason);
    if (target.empty()) return false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(target, error);
    const bool overlayExists = !error && std::filesystem::is_regular_file(status);
    error.clear();
    const bool archiveExists = chosen->archive->find(relative) != nullptr;
    if (!overlayExists && !archiveExists) {
        reason = "file does not exist";
        return false;
    }
    if (overlayExists && !std::filesystem::remove(target, error)) {
        reason = error ? error.message() : "bundle overlay file could not be removed";
        return false;
    }
    if (archiveExists && !createWhiteout(chosen->overlayRoot, relative, reason)) return false;
    return true;
}

bool FileSystem::rename(const std::string& from, const std::string& to, std::string& reason) {
    if (!validGuestPath(from, reason) || !validGuestPath(to, reason)) return false;
    if (from == to) return true;
    Mount* sourceMount = nullptr;
    std::string sourceRelative;
    for (auto& candidate : mounts) {
        if (boundary(from, candidate.guestRoot.size()) &&
            !from.compare(0, candidate.guestRoot.size(), candidate.guestRoot)) {
            sourceMount = &candidate;
            sourceRelative = from.substr(candidate.guestRoot.size());
            while (!sourceRelative.empty() && sourceRelative.front() == '/')
                sourceRelative.erase(sourceRelative.begin());
            break;
        }
    }
    if (!sourceMount || !sourceMount->writable) {
        reason = sourceMount ? "source mount is read-only" : "source is outside every mount";
        return false;
    }
    const bool archiveSource = sourceMount->kind == Mount::Kind::Archive &&
                               sourceMount->archive->find(sourceRelative) != nullptr;
    if (sourceMount->kind == Mount::Kind::Archive && resolve(from, false, reason).empty())
        return false;
    const std::string fromHost = resolve(from, true, reason, false);
    if (fromHost.empty()) return false;
    const std::string toHost = resolve(to, true, reason, true);
    if (toHost.empty()) return false;
    std::error_code error;
    std::filesystem::rename(fromHost, toHost, error);
    if (error) {
        reason = error.message();
        return false;
    }
    if (archiveSource && !createWhiteout(sourceMount->overlayRoot, sourceRelative, reason))
        return false;
    return true;
}

std::string FileSystem::describe() const {
    if (mounts.empty()) return "no guest directories are mounted; file access is refused";
    std::string text = std::to_string(mounts.size()) + " mounted guest director" +
                       (mounts.size() == 1 ? "y" : "ies") + ":";
    for (size_t index = 0; index < mounts.size(); index++) {
        const auto& mount = mounts[index];
        text += (index ? ", " : " ") + mount.guestRoot;
        if (mount.kind == Mount::Kind::Archive)
            text +=
                " (IPA ZIP read-through, verified file cache, "
                "copy-on-write overlay)";
        else
            text += mount.writable ? " (read-write)" : " (read-only)";
    }
    return text;
}
}  // namespace podnative::runtime
