#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace podnative::runtime {
// The guest sandbox. Guest code sees iOS-style absolute paths; only the trees the
// launcher mounted are reachable. An IPA mount is a ZIP-backed read-through filesystem:
// individual app files are verified and cached only when first opened. Bundle writes go
// to a private copy-on-write overlay, never back into the imported archive.
class FileSystem final {
   public:
    void mount(std::string guestRoot, std::string hostRoot, bool writable);
    void mountArchive(std::string guestRoot, std::string archivePath, std::string archivePrefix,
                      std::string cacheRoot, std::string overlayRoot, bool writable);
    size_t count() const { return mounts.size(); }
    // Longest-prefix match. Returns an empty string when the guest path is outside every mount
    // or asks for access the mount does not grant; `reason` says which. `truncate` allows a
    // write-only open to create a fresh overlay file without decompressing its archive version.
    std::string resolve(const std::string& guestPath, bool write, std::string& reason,
                        bool truncate = false);
    bool remove(const std::string& guestPath, std::string& reason);
    bool rename(const std::string& from, const std::string& to, std::string& reason);
    std::string describe() const;

   private:
    struct ArchiveEntry {
        std::string name;
        uint16_t flags = 0, method = 0;
        uint32_t crc = 0;
        uint64_t compressedSize = 0, uncompressedSize = 0, localHeaderOffset = 0;
    };
    struct ArchiveIndex {
        std::string archivePath, prefix;
        uint64_t archiveSize = 0;
        std::map<std::string, ArchiveEntry> entries;
        static std::shared_ptr<ArchiveIndex> open(const std::string& path,
                                                  const std::string& prefix);
        const ArchiveEntry* find(const std::string& relative) const;
        std::string materialize(const std::string& relative, const ArchiveEntry& entry,
                                const std::filesystem::path& root, std::string& reason) const;
        bool extract(const ArchiveEntry& entry, const std::filesystem::path& destination,
                     const std::filesystem::path& root, std::string& reason) const;
    };
    struct Mount {
        enum class Kind { Directory, Archive };
        std::string guestRoot, hostRoot;
        bool writable = false;
        Kind kind = Kind::Directory;
        std::filesystem::path canonicalRoot, cacheRoot, overlayRoot;
        std::shared_ptr<ArchiveIndex> archive;
    };
    std::vector<Mount> mounts;
    static bool validGuestPath(const std::string& path, std::string& reason);
    static bool containedBy(const std::filesystem::path& root, const std::filesystem::path& target);
    static std::filesystem::path safeTarget(const std::filesystem::path& root,
                                            const std::string& relative, std::string& reason);
    static std::filesystem::path whiteoutPath(const std::filesystem::path& root,
                                              const std::string& relative);
    static bool createWhiteout(const std::filesystem::path& root, const std::string& relative,
                               std::string& reason);
    static std::string archivePath(const Mount& mount, const std::string& relative);
    static bool safeArchivePath(const std::string& path, bool trailingSlash);
};
}  // namespace podnative::runtime
