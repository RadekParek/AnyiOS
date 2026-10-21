#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace podnative::runtime {
struct ExportDefinition {
    uint32_t address;
    bool weak = false;
};
// Session-owned addresses, never host pointers. Insertion order is image load order.
class ExportGraph final {
   public:
    size_t addImage(const std::string& installName);
    void setMainExecutable(size_t image);
    void alias(size_t image, const std::string& installName);
    void dependencies(size_t image, std::vector<std::string> names);
    void define(size_t image, const std::string& symbol, ExportDefinition definition);
    bool strong(size_t image, const std::string& symbol) const;
    bool hasStrong(const std::string& symbol) const;
    std::optional<uint32_t> resolve(size_t requester, int ordinal, const std::string& symbol) const;

   private:
    struct Image {
        std::vector<std::string> dependencies;
        std::map<std::string, ExportDefinition> symbols;
    };
    std::vector<Image> images;
    std::map<std::string, size_t> names;
    size_t main = 0, definitions = 0, nameBytes = 0;
    void text(const std::string& name);
};
}  // namespace podnative::runtime
