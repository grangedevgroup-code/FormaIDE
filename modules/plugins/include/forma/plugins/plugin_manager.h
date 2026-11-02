#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/build/build_system.h"
#include "forma/codegen/generator.h"
#include "forma/core/json.h"
#include "forma/editor/language_registry.h"
#include "forma/project/project.h"

namespace forma::plugins {

constexpr int kPluginApiVersion = 1;

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string publisher;
    std::string description;
    int apiVersion = 0;
    std::string minimumHostVersion;
    std::vector<std::string> permissions;
    std::vector<std::string> dependencies;
    Json contributes = Json::object();
};

Result<PluginManifest> parseManifest(const Json& json);
const std::vector<std::string>& knownPermissions();
std::string describePermission(std::string_view permission);

struct InstalledPlugin {
    PluginManifest manifest;
    std::filesystem::path directory;
    bool enabled = false;
    std::vector<std::string> grantedPermissions;
    std::string contentHash;
    bool tampered = false;
    std::string loadError;
};

struct Registries {
    codegen::GeneratorRegistry* generators = nullptr;
    project::TemplateRegistry* templates = nullptr;
    editor::LanguageRegistry* languages = nullptr;
    build::BuildSystemRegistry* builders = nullptr;
    ir::ComponentCatalog* components = nullptr;
};

class PluginManager {
public:
    explicit PluginManager(std::filesystem::path pluginsDirectory);

    Status scan();
    const std::vector<InstalledPlugin>& plugins() const { return plugins_; }
    const InstalledPlugin* find(std::string_view id) const;

    Result<PluginManifest> inspect(const std::filesystem::path& source) const;
    Status install(const std::filesystem::path& source, const std::vector<std::string>& grantedPermissions);
    Status uninstall(std::string_view id);
    Status setEnabled(std::string_view id, bool enabled);

    void activate(const Registries& registries);
    std::vector<Json> contributedThemes() const;

private:
    Status saveState() const;

    std::filesystem::path directory_;
    std::vector<InstalledPlugin> plugins_;
};

std::string hashDirectory(const std::filesystem::path& directory);

std::unique_ptr<codegen::ICodeGenerator> createExternalGenerator(const Json& definition, const std::filesystem::path& pluginDirectory, std::string pluginId);

}
