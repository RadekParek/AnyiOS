#include "fs.h"

#include <algorithm>
#include <filesystem>

#include "error.h"

namespace podnative::runtime {
namespace {
constexpr size_t PathLimit = 4096;
bool boundary(const std::string& path, size_t length) {
    if (length > path.size()) return false;
    return path.size() == length || path[length] == '/';
}
// A component-wise symlink check: the guest must not be able to leave a mount by
// following a link that the host extractor happened to leave in the bundle.
bool followsLink(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path prefix;
    size_t components = 0;
    for (const auto& part : path) {
        prefix /= part;
        components++;
        if (components <= 1) continue;  // The root itself is whatever the host provides.
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (error) {
            // A missing component means the caller is about to create it; every other
            // unknown state is refused rather than allowed to escape the mount.
            if (error == std::errc::no_such_file_or_directory) return false;
            return true;
        }
        if (std::filesystem::is_symlink(status)) return true;
        if (!std::filesystem::exists(status)) return false;
    }
    return false;
}
}  // namespace
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
void FileSystem::mount(std::string guestRoot, std::string hostRoot, bool writable) {
    std::string reason;
    if (!validGuestPath(guestRoot, reason)) throw Error("FS_MOUNT", reason);
    if (guestRoot.size() > 1 && guestRoot.back() == '/') guestRoot.pop_back();
    if (hostRoot.empty()) throw Error("FS_MOUNT", "host root is empty");
    for (const auto& existing : mounts)
        if (existing.guestRoot == guestRoot) throw Error("FS_MOUNT", "duplicate guest mount");
    mounts.push_back({std::move(guestRoot), std::move(hostRoot), writable});
    // Longest guest root first, so a writable Documents mount wins over the read-only
    // bundle that contains it.
    std::sort(mounts.begin(), mounts.end(), [](const Mount& a, const Mount& b) {
        return a.guestRoot.size() > b.guestRoot.size();
    });
}
std::string FileSystem::resolve(const std::string& guestPath, bool write,
                                std::string& reason) const {
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
    std::filesystem::path host(chosen->hostRoot);
    if (!tail.empty()) host /= tail;
    if (followsLink(host)) {
        reason = "path resolves through a symbolic link";
        return {};
    }
    return host.string();
}
std::string FileSystem::describe() const {
    if (mounts.empty()) return "no guest directories are mounted; file access is refused";
    std::string text = std::to_string(mounts.size()) + " mounted guest director" +
                       (mounts.size() == 1 ? "y" : "ies") + ":";
    for (size_t index = 0; index < mounts.size(); index++) {
        const auto& mount = mounts[index];
        text += (index ? ", " : " ") + mount.guestRoot +
                (mount.writable ? " (read-write)" : " (read-only)");
    }
    return text;
}
}  // namespace podnative::runtime
