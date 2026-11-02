#include "forma/editor/language_registry.h"

#include <algorithm>

#include "forma/core/text.h"
#include "forma/resources/resources.h"

namespace forma::editor {

LanguageRegistry::LanguageRegistry()
{
    auto data = resources::find("languages/languages.json");
    if (data) {
        auto status = addFile(*data, "builtin");
        (void)status;
    }
    if (find("text") == nullptr) {
        LanguageDefinition plain;
        plain.id = "text";
        plain.name = "Plain text";
        add(std::move(plain));
    }
}

Status LanguageRegistry::addFile(std::string_view jsonText, std::string source)
{
    auto parsed = parseJson(jsonText);
    if (!parsed)
        return parsed.error();
    if (parsed->is_array()) {
        for (const auto& item : *parsed) {
            if (auto status = addFromJson(item, source); !status)
                return status;
        }
        return {};
    }
    return addFromJson(*parsed, std::move(source));
}

Status LanguageRegistry::addFromJson(const Json& json, std::string source)
{
    if (!json.is_object())
        return makeError("language.parse", "A language definition must be an object");
    LanguageDefinition definition;
    definition.id = jsonString(json, "id");
    if (definition.id.empty())
        return makeError("language.parse", "A language definition needs an id");
    definition.name = jsonString(json, "name", definition.id);
    definition.extensions = jsonStrings(json, "extensions");
    definition.filenames = jsonStrings(json, "filenames");
    definition.shebangs = jsonStrings(json, "shebangs");
    definition.lexer = jsonString(json, "lexer", "null");
    definition.keywords = jsonStrings(json, "keywords");
    definition.lineComment = jsonString(json, "comment");
    auto block = jsonStrings(json, "blockComment");
    if (block.size() == 2) {
        definition.blockCommentStart = block[0];
        definition.blockCommentEnd = block[1];
    }
    auto properties = json.find("properties");
    if (properties != json.end() && properties->is_object()) {
        for (auto item = properties->begin(); item != properties->end(); ++item) {
            if (item->is_string())
                definition.lexerProperties[item.key()] = item->get<std::string>();
        }
    }
    auto servers = json.find("servers");
    if (servers != json.end() && servers->is_array()) {
        for (const auto& item : *servers) {
            LanguageServerSpec server;
            server.command = jsonStrings(item, "command");
            server.install = jsonString(item, "install");
            if (!server.command.empty())
                definition.servers.push_back(std::move(server));
        }
    }
    definition.formatter = jsonStrings(json, "formatter");
    definition.debugAdapters = jsonStrings(json, "debug");
    definition.run = jsonStrings(json, "run");
    definition.indent = jsonInt(json, "indent", 4);
    definition.tabs = jsonBool(json, "tabs", false);
    definition.wrap = jsonBool(json, "wrap", false);
    definition.source = std::move(source);
    add(std::move(definition));
    return {};
}

void LanguageRegistry::add(LanguageDefinition definition)
{
    std::size_t index;
    auto existing = byId_.find(definition.id);
    if (existing != byId_.end()) {
        index = existing->second;
        languages_[index] = std::move(definition);
    } else {
        index = languages_.size();
        byId_[definition.id] = index;
        languages_.push_back(std::move(definition));
    }
    const LanguageDefinition& stored = languages_[index];
    for (const auto& extension : stored.extensions)
        byExtension_.emplace(text::toLower(extension), index);
    for (const auto& filename : stored.filenames)
        byFilename_.emplace(filename, index);
    for (const auto& shebang : stored.shebangs)
        byShebang_.emplace(shebang, index);
}

const LanguageDefinition* LanguageRegistry::find(std::string_view id) const
{
    auto it = byId_.find(std::string(id));
    return it == byId_.end() ? nullptr : &languages_[it->second];
}

const LanguageDefinition* LanguageRegistry::forPath(std::string_view path) const
{
    std::string normalized = text::replaceAll(std::string(path), "\\", "/");
    std::string name = normalized.substr(normalized.find_last_of('/') + 1);
    auto byName = byFilename_.find(name);
    if (byName != byFilename_.end())
        return &languages_[byName->second];
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        std::string extension = name.substr(dot + 1);
        auto exact = byExtension_.find(extension);
        if (exact != byExtension_.end())
            return &languages_[exact->second];
        auto lower = byExtension_.find(text::toLower(extension));
        if (lower != byExtension_.end())
            return &languages_[lower->second];
    }
    return nullptr;
}

const LanguageDefinition* LanguageRegistry::forShebang(std::string_view firstLine) const
{
    if (!text::startsWith(firstLine, "#!"))
        return nullptr;
    std::string line = text::trim(firstLine.substr(2));
    auto parts = text::split(line, ' ');
    if (parts.empty())
        return nullptr;
    std::string program = parts[0].substr(parts[0].find_last_of('/') + 1);
    if (program == "env" && parts.size() > 1)
        program = parts[1];
    auto it = byShebang_.find(program);
    if (it != byShebang_.end())
        return &languages_[it->second];
    while (!program.empty() && (std::isdigit(static_cast<unsigned char>(program.back())) || program.back() == '.'))
        program.pop_back();
    it = byShebang_.find(program);
    return it == byShebang_.end() ? nullptr : &languages_[it->second];
}

const LanguageDefinition& LanguageRegistry::plainText() const
{
    return *find("text");
}

}
