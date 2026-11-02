#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "forma/core/json.h"

namespace forma::log {

enum class Level { Debug, Info, Warning, Error };

struct Record {
    Level level = Level::Info;
    std::string area;
    std::string message;
    Json fields;
    std::string time;
};

using Sink = std::function<void(const Record&)>;

void setLogFile(const std::filesystem::path& path);
void setSink(Sink sink);
void setMinimumLevel(Level level);
void write(Level level, std::string_view area, std::string_view message, Json fields = Json::object());

inline void debug(std::string_view area, std::string_view message, Json fields = Json::object())
{
    write(Level::Debug, area, message, std::move(fields));
}

inline void info(std::string_view area, std::string_view message, Json fields = Json::object())
{
    write(Level::Info, area, message, std::move(fields));
}

inline void warning(std::string_view area, std::string_view message, Json fields = Json::object())
{
    write(Level::Warning, area, message, std::move(fields));
}

inline void error(std::string_view area, std::string_view message, Json fields = Json::object())
{
    write(Level::Error, area, message, std::move(fields));
}

}
