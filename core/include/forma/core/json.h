#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/result.h"

namespace forma {

using Json = nlohmann::ordered_json;

Result<Json> parseJson(std::string_view text);
std::string dumpJson(const Json& value, int indent = 2);
std::string dumpJsonCompact(const Json& value);
std::vector<std::uint8_t> toCbor(const Json& value);
Result<Json> fromCbor(const std::vector<std::uint8_t>& bytes);

std::string jsonString(const Json& object, std::string_view key, std::string_view fallback = {});
double jsonNumber(const Json& object, std::string_view key, double fallback = 0);
int jsonInt(const Json& object, std::string_view key, int fallback = 0);
bool jsonBool(const Json& object, std::string_view key, bool fallback = false);
std::vector<std::string> jsonStrings(const Json& object, std::string_view key);

const Json* jsonFindPath(const Json& root, std::string_view dottedPath);
void jsonSetPath(Json& root, std::string_view dottedPath, const Json& value);
void jsonErasePath(Json& root, std::string_view dottedPath);

}
