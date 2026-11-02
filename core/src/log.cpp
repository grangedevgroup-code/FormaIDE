#include "forma/core/log.h"

#include <fstream>
#include <mutex>

#include "forma/core/paths.h"

namespace forma::log {

namespace {

struct State {
    std::mutex mutex;
    std::ofstream file;
    Sink sink;
    Level minimum = Level::Info;
};

State& state()
{
    static State instance;
    return instance;
}

const char* levelName(Level level)
{
    switch (level) {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warning:
        return "warning";
    case Level::Error:
        return "error";
    }
    return "info";
}

}

void setLogFile(const std::filesystem::path& path)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > 8u * 1024u * 1024u) {
        auto rotated = path;
        rotated += ".1";
        std::filesystem::rename(path, rotated, ec);
    }
    s.file.close();
    s.file.open(path, std::ios::app | std::ios::binary);
}

void setSink(Sink sink)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.sink = std::move(sink);
}

void setMinimumLevel(Level level)
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.minimum = level;
}

void write(Level level, std::string_view area, std::string_view message, Json fields)
{
    auto& s = state();
    Sink sink;
    Record record;
    {
        std::lock_guard lock(s.mutex);
        if (level < s.minimum)
            return;
        record.level = level;
        record.area = std::string(area);
        record.message = std::string(message);
        record.fields = std::move(fields);
        record.time = paths::nowIso8601();
        if (s.file.is_open()) {
            Json line = Json::object();
            line["time"] = record.time;
            line["level"] = levelName(level);
            line["area"] = record.area;
            line["message"] = record.message;
            if (!record.fields.empty())
                line["fields"] = record.fields;
            s.file << dumpJsonCompact(line) << '\n';
            s.file.flush();
        }
        sink = s.sink;
    }
    if (sink)
        sink(record);
}

}
