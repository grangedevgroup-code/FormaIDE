#pragma once

#include <QObject>
#include <memory>

#include "forma/build/build_system.h"
#include "forma/codegen/generator.h"
#include "forma/core/credentials.h"
#include "forma/core/settings.h"
#include "forma/editor/language_registry.h"
#include "forma/ir/catalog.h"
#include "forma/plugins/plugin_manager.h"
#include "forma/project/project.h"

namespace forma::ui {

class AppContext {
public:
    AppContext();

    static AppContext& instance();

    Settings& settings() { return *settings_; }
    codegen::GeneratorRegistry& generators() { return generators_; }
    build::BuildSystemRegistry& builders() { return builders_; }
    build::ToolchainDetector& tools() { return tools_; }
    project::TemplateRegistry& templates() { return templates_; }
    project::RecentProjects& recents() { return *recents_; }
    editor::LanguageRegistry& languages() { return languages_; }
    ICredentialStore& credentials() { return *credentials_; }
    plugins::PluginManager& plugins() { return *plugins_; }
    ir::ComponentCatalog& components() { return components_; }

    void reloadPlugins();
    void saveSettings();

private:
    std::unique_ptr<Settings> settings_;
    codegen::GeneratorRegistry generators_;
    build::BuildSystemRegistry builders_;
    build::ToolchainDetector tools_;
    project::TemplateRegistry templates_;
    std::unique_ptr<project::RecentProjects> recents_;
    editor::LanguageRegistry languages_;
    std::unique_ptr<ICredentialStore> credentials_;
    std::unique_ptr<plugins::PluginManager> plugins_;
    ir::ComponentCatalog components_;
};

}
