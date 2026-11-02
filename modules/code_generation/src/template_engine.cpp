#include "forma/codegen/template_engine.h"

#include <vector>

#include "forma/core/text.h"

namespace forma::codegen {

namespace {

const Json* lookup(const std::vector<const Json*>& scopes, std::string_view path)
{
    if (path == "this" || path == ".")
        return scopes.back();
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (const Json* found = jsonFindPath(**scope, path))
            return found;
    }
    return nullptr;
}

bool truthy(const Json* value)
{
    if (value == nullptr || value->is_null())
        return false;
    if (value->is_boolean())
        return value->get<bool>();
    if (value->is_string())
        return !value->get<std::string>().empty();
    if (value->is_array() || value->is_object())
        return !value->empty();
    if (value->is_number())
        return value->get<double>() != 0;
    return true;
}

std::string stringify(const Json* value)
{
    if (value == nullptr || value->is_null())
        return {};
    if (value->is_string())
        return value->get<std::string>();
    if (value->is_number())
        return text::formatNumber(value->get<double>());
    if (value->is_boolean())
        return value->get<bool>() ? "true" : "false";
    return dumpJsonCompact(*value);
}

class Renderer {
public:
    explicit Renderer(std::string_view source) : source_(source) {}

    Result<std::string> render(const Json& context)
    {
        std::vector<const Json*> scopes{&context};
        std::string output;
        std::size_t position = 0;
        if (auto status = block(position, scopes, output, true, {}); !status)
            return status.error();
        return output;
    }

private:
    Status block(std::size_t& position, std::vector<const Json*>& scopes, std::string& output, bool active, std::string_view closing)
    {
        while (position < source_.size()) {
            std::size_t open = source_.find("{{", position);
            if (open == std::string_view::npos) {
                if (active)
                    output.append(source_.substr(position));
                position = source_.size();
                break;
            }
            std::size_t close = source_.find("}}", open + 2);
            if (close == std::string_view::npos)
                return makeError("template.tag", "A template tag is never closed");
            std::size_t tagEnd = close + 2;
            std::string tag = text::trim(source_.substr(open + 2, close - open - 2));
            bool standalone = isStandalone(open, tagEnd, tag);
            std::size_t before = standalone ? lineStart(open) : open;
            if (active)
                output.append(source_.substr(position, before - position));
            std::size_t after = standalone ? skipNewline(tagEnd) : tagEnd;

            if (text::startsWith(tag, "/")) {
                if (tag.substr(1) != closing)
                    return makeError("template.tag", "Unexpected closing tag: " + tag);
                position = after;
                return {};
            }
            if (text::startsWith(tag, "#if ") || text::startsWith(tag, "#unless ")) {
                bool isIf = text::startsWith(tag, "#if ");
                std::string name = text::trim(tag.substr(isIf ? 4 : 8));
                bool condition = truthy(lookup(scopes, name));
                if (!isIf)
                    condition = !condition;
                position = after;
                if (auto status = block(position, scopes, output, active && condition, isIf ? "if" : "unless"); !status)
                    return status;
                continue;
            }
            if (text::startsWith(tag, "#each ")) {
                std::string name = text::trim(tag.substr(6));
                const Json* list = lookup(scopes, name);
                std::size_t bodyStart = after;
                if (active && list != nullptr && list->is_array() && !list->empty()) {
                    std::size_t end = bodyStart;
                    for (std::size_t i = 0; i < list->size(); ++i) {
                        Json item = (*list)[i];
                        Json decorated = item.is_object() ? item : Json{{"this", item}};
                        decorated["@first"] = i == 0;
                        decorated["@last"] = i + 1 == list->size();
                        decorated["@index"] = i;
                        scopes.push_back(item.is_object() ? &decorated : &item);
                        Json holder = Json{{"@first", i == 0}, {"@last", i + 1 == list->size()}, {"@index", i}};
                        scopes.insert(scopes.end() - 1, &holder);
                        end = bodyStart;
                        if (auto status = block(end, scopes, output, true, "each"); !status)
                            return status;
                        scopes.pop_back();
                        scopes.pop_back();
                    }
                    position = end;
                } else {
                    position = bodyStart;
                    if (auto status = block(position, scopes, output, false, "each"); !status)
                        return status;
                }
                continue;
            }
            if (active)
                output += stringify(lookup(scopes, tag));
            position = after;
        }
        if (!closing.empty())
            return makeError("template.tag", "Missing closing tag for " + std::string(closing));
        return {};
    }

    bool isStandalone(std::size_t open, std::size_t tagEnd, const std::string& tag) const
    {
        if (tag.empty() || (tag[0] != '#' && tag[0] != '/'))
            return false;
        std::size_t start = lineStart(open);
        for (std::size_t i = start; i < open; ++i) {
            if (source_[i] != ' ' && source_[i] != '\t')
                return false;
        }
        std::size_t i = tagEnd;
        while (i < source_.size() && (source_[i] == ' ' || source_[i] == '\t'))
            ++i;
        return i >= source_.size() || source_[i] == '\n' || source_[i] == '\r';
    }

    std::size_t lineStart(std::size_t position) const
    {
        while (position > 0 && source_[position - 1] != '\n')
            --position;
        return position;
    }

    std::size_t skipNewline(std::size_t position) const
    {
        while (position < source_.size() && (source_[position] == ' ' || source_[position] == '\t'))
            ++position;
        if (position < source_.size() && source_[position] == '\r')
            ++position;
        if (position < source_.size() && source_[position] == '\n')
            ++position;
        return position;
    }

    std::string_view source_;
};

}

Result<std::string> renderTemplate(std::string_view source, const Json& context)
{
    return Renderer(source).render(context);
}

}
