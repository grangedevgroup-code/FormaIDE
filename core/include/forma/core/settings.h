#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>

#include "forma/core/json.h"

namespace forma {

class Settings {
public:
    explicit Settings(std::filesystem::path file);

    Status load();
    Status save();

    Json get(std::string_view key, const Json& fallback = nullptr) const;
    void set(std::string_view key, const Json& value);
    void erase(std::string_view key);

    std::string getString(std::string_view key, std::string_view fallback = {}) const;
    bool getBool(std::string_view key, bool fallback = false) const;
    int getInt(std::string_view key, int fallback = 0) const;

    const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    Json values_ = Json::object();
};

}
