#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"
#include "forma/ir/model.h"
#include "forma/ir/validation.h"

namespace forma::codegen {

enum class FileCategory { Generated, User, Protected };

enum class ValueKind { String, Number, Boolean, Color };
enum class LiteralStyle { CLike, Xml, Html, Python, Raw };

struct SourceBinding {
    int line = 0;
    std::string screen;
    std::string component;
    std::string key;
    std::string prefix;
    std::string suffix;
    ValueKind kind = ValueKind::String;
    LiteralStyle style = LiteralStyle::CLike;
};

struct ComponentAnchor {
    std::string screen;
    std::string component;
    int startLine = 0;
    int endLine = 0;
};

struct GeneratedFile {
    std::string path;
    std::string content;
    FileCategory category = FileCategory::Generated;
    bool executable = false;
    std::vector<SourceBinding> bindings;
    std::vector<ComponentAnchor> anchors;
};

struct GeneratorCapabilities {
    std::vector<std::string> components;
    std::vector<std::string> layouts;
    std::vector<std::string> events;
    std::vector<std::string> features;
};

struct GeneratorInfo {
    std::string id;
    std::string label;
    std::string language;
    std::string framework;
    std::string description;
    std::vector<std::string> platforms;
    std::vector<std::string> requiredTools;
    std::string buildSystem;
    std::string approach = "native";
    std::string preview;
};

struct ProjectConfiguration {
    std::string name;
    std::string bundleId;
    std::vector<std::string> platforms;
    Json options = Json::object();
};

struct GenerationResult {
    std::vector<GeneratedFile> files;
    std::vector<ir::Diagnostic> diagnostics;
};

class ICodeGenerator {
public:
    virtual ~ICodeGenerator() = default;
    virtual GeneratorInfo info() const = 0;
    virtual GeneratorCapabilities capabilities() const = 0;
    virtual std::vector<ir::Diagnostic> validate(const ir::ApplicationModel& model) const = 0;
    virtual GenerationResult generate(const ir::ApplicationModel& model, const ProjectConfiguration& config) const = 0;
};

class GeneratorRegistry {
public:
    void add(std::unique_ptr<ICodeGenerator> generator);
    const ICodeGenerator* find(std::string_view id) const;
    std::vector<const ICodeGenerator*> all() const;

private:
    std::vector<std::unique_ptr<ICodeGenerator>> generators_;
};

const char* fileCategoryName(FileCategory category);
FileCategory fileCategoryFromName(std::string_view name);

std::vector<ir::Diagnostic> checkCapabilities(const ir::ApplicationModel& model, const GeneratorCapabilities& capabilities, std::string_view generatorLabel);

}
