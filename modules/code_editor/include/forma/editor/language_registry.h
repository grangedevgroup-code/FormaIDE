#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"

namespace forma::editor {

struct LanguageServerSpec {
    std::vector<std::string> command;
    std::string install;
};

struct LanguageDefinition {
    std::string id;
    std::string name;
    std::vector<std::string> extensions;
    std::vector<std::string> filenames;
    std::vector<std::string> shebangs;
    std::string lexer = "null";
    std::vector<std::string> keywords;
    std::map<std::string, std::string> lexerProperties;
    std::string lineComment;
    std::string blockCommentStart;
    std::string blockCommentEnd;
    std::vector<LanguageServerSpec> servers;
    std::vector<std::string> formatter;
    std::vector<std::string> debugAdapters;
    std::vector<std::string> run;
    int indent = 4;
    bool tabs = false;
    bool wrap = false;
    std::string source = "builtin";
};

class LanguageRegistry {
public:
    LanguageRegistry();

    Status addFromJson(const Json& json, std::string source);
    Status addFile(std::string_view jsonText, std::string source);

    const std::vector<LanguageDefinition>& all() const { return languages_; }
    const LanguageDefinition* find(std::string_view id) const;
    const LanguageDefinition* forPath(std::string_view path) const;
    const LanguageDefinition* forShebang(std::string_view firstLine) const;
    const LanguageDefinition& plainText() const;

private:
    void add(LanguageDefinition definition);

    std::vector<LanguageDefinition> languages_;
    std::map<std::string, std::size_t> byId_;
    std::map<std::string, std::size_t> byExtension_;
    std::map<std::string, std::size_t> byFilename_;
    std::map<std::string, std::size_t> byShebang_;
};

}
