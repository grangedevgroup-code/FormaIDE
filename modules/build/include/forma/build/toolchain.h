#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"

namespace forma::build {

struct ToolInfo {
    std::string id;
    std::string name;
    std::string category;
    bool available = false;
    std::string version;
    std::string path;
    std::string note;
    std::string installHint;
    std::vector<std::string> usedFor;
};

struct QtKit {
    std::string version;
    std::string compiler;
    std::filesystem::path prefix;
};

class ToolchainDetector {
public:
    ToolchainDetector();

    std::vector<ToolInfo> detectAll(bool refresh = false);
    ToolInfo detect(std::string_view id, bool refresh = false);
    bool isAvailable(std::string_view id);
    std::vector<std::string> knownToolIds() const;

    std::vector<QtKit> qtKits();
    std::optional<QtKit> bestQtKit();
    std::map<std::string, std::string> msvcEnvironment();
    std::optional<std::filesystem::path> androidSdk();
    std::optional<std::filesystem::path> javaHome();
    std::optional<std::filesystem::path> gradle();

    static Json toJson(const ToolInfo& tool);

private:
    ToolInfo probe(std::string_view id);

    std::mutex mutex_;
    std::map<std::string, ToolInfo> cache_;
    std::optional<std::map<std::string, std::string>> msvcEnvironment_;
};

std::string hostPlatform();
bool hostCanBuildFor(std::string_view platform, std::string* reason = nullptr);

std::string versionKey(std::string_view version);

}
