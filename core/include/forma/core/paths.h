#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "forma/core/result.h"

namespace forma::paths {

std::string toUtf8(const std::filesystem::path& path);
std::string toGeneric(const std::filesystem::path& path);
std::filesystem::path fromUtf8(std::string_view text);

std::optional<std::string> env(const char* name);
std::filesystem::path homeDir();
std::filesystem::path userConfigDir();
std::filesystem::path userDataDir();
std::filesystem::path userCacheDir();

Result<std::string> readWholeFile(const std::filesystem::path& path);
Status writeFileAtomic(const std::filesystem::path& path, std::string_view data, bool executable = false);

std::int64_t unixMillis();
std::string nowIso8601();
std::string timestampId();

}
