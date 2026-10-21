#pragma once
#include <string>
#include <vector>

namespace podnative::runtime {
// The guest sandbox. Guest code sees iOS-style absolute paths; only the trees the
// launcher mounted are reachable, a read-only mount cannot be written through, and
// symbolic links are refused instead of being followed out of the sandbox.
class FileSystem final {
   public:
    void mount(std::string guestRoot, std::string hostRoot, bool writable);
    size_t count() const { return mounts.size(); }
    // Longest-prefix match. Returns an empty string when the guest path is outside
    // every mount or asks for access the mount does not grant; `reason` says which.
    std::string resolve(const std::string& guestPath, bool write, std::string& reason) const;
    std::string describe() const;

   private:
    struct Mount {
        std::string guestRoot, hostRoot;
        bool writable;
    };
    std::vector<Mount> mounts;
    static bool validGuestPath(const std::string& path, std::string& reason);
};
}  // namespace podnative::runtime
