#include "app/app_context.h"

#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/generators/registry.h"

namespace forma::ui {

AppContext::AppContext()
{
    settings_ = std::make_unique<Settings>(paths::userConfigDir() / "settings.json");
    auto loaded = settings_->load();
    (void)loaded;
    log::setLogFile(paths::userDataDir() / "logs" / "formaide.log");
    generators::registerBuiltinGenerators(generators_);
    build::registerBuiltinBuildProviders(builders_);
    recents_ = std::make_unique<project::RecentProjects>(paths::userDataDir() / "recent-projects.json");
    credentials_ = createPlatformCredentialStore();
    components_ = ir::builtinCatalog();
    plugins_ = std::make_unique<plugins::PluginManager>(paths::userDataDir() / "plugins");
    reloadPlugins();
}

AppContext& AppContext::instance()
{
    static AppContext context;
    return context;
}

void AppContext::reloadPlugins()
{
    auto scanned = plugins_->scan();
    (void)scanned;
    plugins::Registries registries;
    registries.generators = &generators_;
    registries.templates = &templates_;
    registries.languages = &languages_;
    registries.builders = &builders_;
    registries.components = &components_;
    plugins_->activate(registries);
}

void AppContext::saveSettings()
{
    auto saved = settings_->save();
    (void)saved;
}

}
