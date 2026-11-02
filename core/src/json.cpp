#include "forma/core/json.h"

#include "forma/core/text.h"

namespace forma {

Result<Json> parseJson(std::string_view text)
{
    Json value = Json::parse(text.begin(), text.end(), nullptr, false, true);
    if (value.is_discarded()) {
        try {
            (void)Json::parse(text.begin(), text.end(), nullptr, true, true);
        } catch (const std::exception& error) {
            return makeError("json.parse", error.what());
        }
        return makeError("json.parse", "invalid JSON");
    }
    return value;
}

std::string dumpJson(const Json& value, int indent)
{
    std::string text = value.dump(indent, ' ', false, Json::error_handler_t::replace);
    text.push_back('\n');
    return text;
}

std::string dumpJsonCompact(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::vector<std::uint8_t> toCbor(const Json& value)
{
    return Json::to_cbor(value);
}

Result<Json> fromCbor(const std::vector<std::uint8_t>& bytes)
{
    Json value = Json::from_cbor(bytes, true, false);
    if (value.is_discarded())
        return makeError("cbor.parse", "invalid CBOR");
    return value;
}

std::string jsonString(const Json& object, std::string_view key, std::string_view fallback)
{
    if (object.is_object()) {
        auto it = object.find(std::string(key));
        if (it != object.end() && it->is_string())
            return it->get<std::string>();
    }
    return std::string(fallback);
}

double jsonNumber(const Json& object, std::string_view key, double fallback)
{
    if (object.is_object()) {
        auto it = object.find(std::string(key));
        if (it != object.end() && it->is_number())
            return it->get<double>();
    }
    return fallback;
}

int jsonInt(const Json& object, std::string_view key, int fallback)
{
    if (object.is_object()) {
        auto it = object.find(std::string(key));
        if (it != object.end() && it->is_number())
            return it->get<int>();
    }
    return fallback;
}

bool jsonBool(const Json& object, std::string_view key, bool fallback)
{
    if (object.is_object()) {
        auto it = object.find(std::string(key));
        if (it != object.end() && it->is_boolean())
            return it->get<bool>();
    }
    return fallback;
}

std::vector<std::string> jsonStrings(const Json& object, std::string_view key)
{
    std::vector<std::string> values;
    if (object.is_object()) {
        auto it = object.find(std::string(key));
        if (it != object.end() && it->is_array()) {
            for (const auto& item : *it) {
                if (item.is_string())
                    values.push_back(item.get<std::string>());
            }
        }
    }
    return values;
}

const Json* jsonFindPath(const Json& root, std::string_view dottedPath)
{
    const Json* node = &root;
    for (const auto& part : text::split(dottedPath, '.')) {
        if (!node->is_object())
            return nullptr;
        auto it = node->find(part);
        if (it == node->end())
            return nullptr;
        node = &*it;
    }
    return node;
}

void jsonSetPath(Json& root, std::string_view dottedPath, const Json& value)
{
    Json* node = &root;
    auto parts = text::split(dottedPath, '.');
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!node->is_object())
            *node = Json::object();
        node = &(*node)[parts[i]];
    }
    if (!node->is_object())
        *node = Json::object();
    (*node)[parts.back()] = value;
}

void jsonErasePath(Json& root, std::string_view dottedPath)
{
    Json* node = &root;
    auto parts = text::split(dottedPath, '.');
    std::vector<std::pair<Json*, std::string>> chain;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!node->is_object())
            return;
        auto it = node->find(parts[i]);
        if (it == node->end())
            return;
        chain.emplace_back(node, parts[i]);
        node = &*it;
    }
    if (node->is_object())
        node->erase(parts.back());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Json& child = (*it->first)[it->second];
        if (child.is_object() && child.empty())
            it->first->erase(it->second);
        else
            break;
    }
}

}
