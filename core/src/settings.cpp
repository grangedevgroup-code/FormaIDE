#include "forma/core/settings.h"

#include "forma/core/log.h"
#include "forma/core/paths.h"

namespace forma {

Settings::Settings(std::filesystem::path file) : file_(std::move(file)) {}

Status Settings::load()
{
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec))
        return {};
    auto content = paths::readWholeFile(file_);
    if (!content)
        return content.error();
    auto parsed = parseJson(*content);
    if (!parsed || !parsed->is_object()) {
        auto aside = file_;
        aside += ".damaged-" + paths::timestampId();
        std::filesystem::rename(file_, aside, ec);
        log::warning("settings", "moved unreadable settings aside", Json{{"path", paths::toUtf8(aside)}});
        std::lock_guard lock(mutex_);
        values_ = Json::object();
        return {};
    }
    std::lock_guard lock(mutex_);
    values_ = std::move(parsed.value());
    return {};
}

Status Settings::save()
{
    std::string text;
    {
        std::lock_guard lock(mutex_);
        text = dumpJson(values_);
    }
    return paths::writeFileAtomic(file_, text);
}

Json Settings::get(std::string_view key, const Json& fallback) const
{
    std::lock_guard lock(mutex_);
    const Json* found = jsonFindPath(values_, key);
    return found != nullptr ? *found : fallback;
}

void Settings::set(std::string_view key, const Json& value)
{
    std::lock_guard lock(mutex_);
    jsonSetPath(values_, key, value);
}

void Settings::erase(std::string_view key)
{
    std::lock_guard lock(mutex_);
    jsonErasePath(values_, key);
}

std::string Settings::getString(std::string_view key, std::string_view fallback) const
{
    Json value = get(key);
    return value.is_string() ? value.get<std::string>() : std::string(fallback);
}

bool Settings::getBool(std::string_view key, bool fallback) const
{
    Json value = get(key);
    return value.is_boolean() ? value.get<bool>() : fallback;
}

int Settings::getInt(std::string_view key, int fallback) const
{
    Json value = get(key);
    return value.is_number() ? value.get<int>() : fallback;
}

}
