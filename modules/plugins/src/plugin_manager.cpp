#include "forma/plugins/plugin_manager.h"

#include <algorithm>
#include <regex>

#include "forma/core/hash.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"
#include "forma/ir/catalog.h"
#include "forma/ir/serialization.h"

namespace fs = std::filesystem;

namespace forma::plugins {

const std::vector<std::string>& knownPermissions()
{
    static const std::vector<std::string> permissions = {"read.design", "write.generated", "network", "filesystem.project", "process"};
    return permissions;
}

std::string describePermission(std::string_view permission)
{
    if (permission == "read.design")
        return "Read this project's design model";
    if (permission == "write.generated")
        return "Propose generated files inside the project";
    if (permission == "network")
        return "Reach the network";
    if (permission == "filesystem.project")
        return "Read and change any file in the project";
    if (permission == "process")
        return "Run other programs on this computer";
    return std::string(permission);
}

Result<PluginManifest> parseManifest(const Json& json)
{
    if (!json.is_object())
        return makeError("plugin.manifest", "The plugin manifest must be an object");
    PluginManifest manifest;
    manifest.id = jsonString(json, "id");
    manifest.name = jsonString(json, "name", manifest.id);
    manifest.version = jsonString(json, "version");
    manifest.publisher = jsonString(json, "publisher");
    manifest.description = jsonString(json, "description");
    manifest.apiVersion = jsonInt(json, "apiVersion");
    manifest.minimumHostVersion = jsonString(json, "minimumHostVersion");
    manifest.permissions = jsonStrings(json, "permissions");
    manifest.dependencies = jsonStrings(json, "dependencies");
    if (json.contains("contributes"))
        manifest.contributes = json["contributes"];
    static const std::regex idPattern("^[A-Za-z0-9][A-Za-z0-9._-]{1,99}$");
    if (!std::regex_match(manifest.id, idPattern))
        return makeError("plugin.manifest", "The plugin id must use letters, numbers, dots, dashes and underscores");
    if (manifest.version.empty())
        return makeError("plugin.manifest", "The plugin has no version");
    if (manifest.publisher.empty())
        return makeError("plugin.manifest", "The plugin has no publisher");
    if (manifest.apiVersion != kPluginApiVersion)
        return makeError("plugin.api", "This plugin needs API version " + std::to_string(manifest.apiVersion) + ", and this FormaIDE provides version " + std::to_string(kPluginApiVersion));
    for (const auto& permission : manifest.permissions) {
        const auto& known = knownPermissions();
        if (std::find(known.begin(), known.end(), permission) == known.end())
            return makeError("plugin.permission", "Unknown permission: " + permission);
    }
    return manifest;
}

std::string hashDirectory(const fs::path& directory)
{
    std::vector<std::string> entries;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        std::string relative = paths::toGeneric(it->path().lexically_relative(directory));
        if (relative == "__pycache__" || text::contains(relative, "__pycache__/"))
            continue;
        auto content = paths::readWholeFile(it->path());
        entries.push_back(relative + ":" + sha256Hex(content ? *content : std::string()));
    }
    std::sort(entries.begin(), entries.end());
    return sha256Hex(text::join(entries, "\n"));
}

PluginManager::PluginManager(fs::path pluginsDirectory) : directory_(std::move(pluginsDirectory)) {}

Status PluginManager::scan()
{
    plugins_.clear();
    std::error_code ec;
    fs::create_directories(directory_, ec);
    Json state = Json::object();
    auto stateText = paths::readWholeFile(directory_ / "state.json");
    if (stateText) {
        auto parsed = parseJson(*stateText);
        if (parsed)
            state = *parsed;
    }
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;
        InstalledPlugin plugin;
        plugin.directory = it->path();
        auto content = paths::readWholeFile(it->path() / "forma-plugin.json");
        if (!content)
            continue;
        auto parsed = parseJson(*content);
        if (!parsed)
            continue;
        auto manifest = parseManifest(*parsed);
        if (!manifest) {
            plugin.manifest.id = paths::toUtf8(it->path().filename());
            plugin.manifest.name = plugin.manifest.id;
            plugin.loadError = manifest.error().message;
            plugins_.push_back(std::move(plugin));
            continue;
        }
        plugin.manifest = std::move(manifest.value());
        Json record = state.contains(plugin.manifest.id) ? state[plugin.manifest.id] : Json::object();
        plugin.enabled = jsonBool(record, "enabled");
        plugin.grantedPermissions = jsonStrings(record, "granted");
        plugin.contentHash = jsonString(record, "hash");
        std::string current = hashDirectory(it->path());
        plugin.tampered = !plugin.contentHash.empty() && plugin.contentHash != current;
        if (plugin.tampered)
            plugin.enabled = false;
        plugins_.push_back(std::move(plugin));
    }
    std::sort(plugins_.begin(), plugins_.end(), [](const auto& a, const auto& b) { return a.manifest.name < b.manifest.name; });
    return {};
}

const InstalledPlugin* PluginManager::find(std::string_view id) const
{
    auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](const InstalledPlugin& plugin) { return plugin.manifest.id == id; });
    return it == plugins_.end() ? nullptr : &*it;
}

Status PluginManager::saveState() const
{
    Json state = Json::object();
    for (const auto& plugin : plugins_) {
        if (plugin.manifest.id.empty() || !plugin.loadError.empty())
            continue;
        state[plugin.manifest.id] = Json{{"enabled", plugin.enabled}, {"granted", plugin.grantedPermissions}, {"hash", plugin.contentHash}};
    }
    return paths::writeFileAtomic(directory_ / "state.json", dumpJson(state));
}

Result<PluginManifest> PluginManager::inspect(const fs::path& source) const
{
    auto content = paths::readWholeFile(source / "forma-plugin.json");
    if (!content)
        return makeError("plugin.manifest", "This folder has no forma-plugin.json");
    auto parsed = parseJson(*content);
    if (!parsed)
        return parsed.error();
    return parseManifest(*parsed);
}

Status PluginManager::install(const fs::path& source, const std::vector<std::string>& grantedPermissions)
{
    auto manifest = inspect(source);
    if (!manifest)
        return manifest.error();
    for (const auto& permission : manifest->permissions) {
        if (std::find(grantedPermissions.begin(), grantedPermissions.end(), permission) == grantedPermissions.end())
            return makeError("plugin.permission", "The plugin needs a permission you did not grant: " + describePermission(permission));
    }
    for (const auto& dependency : manifest->dependencies) {
        if (find(dependency) == nullptr)
            return makeError("plugin.dependency", "Install " + dependency + " first");
    }
    std::error_code ec;
    fs::path destination = directory_ / paths::fromUtf8(manifest->id);
    fs::path staging = directory_ / paths::fromUtf8(".installing-" + manifest->id);
    fs::remove_all(staging, ec);
    fs::copy(source, staging, fs::copy_options::recursive, ec);
    if (ec)
        return makeError("plugin.install", "Cannot copy the plugin: " + ec.message());
    fs::remove_all(destination, ec);
    fs::rename(staging, destination, ec);
    if (ec)
        return makeError("plugin.install", "Cannot install the plugin: " + ec.message());
    if (auto status = scan(); !status)
        return status;
    auto entry = std::find_if(plugins_.begin(), plugins_.end(), [&](const InstalledPlugin& plugin) { return plugin.manifest.id == manifest->id; });
    if (entry == plugins_.end())
        return makeError("plugin.install", "The plugin did not load after installing");
    entry->enabled = true;
    entry->tampered = false;
    entry->grantedPermissions = grantedPermissions;
    entry->contentHash = hashDirectory(destination);
    return saveState();
}

Status PluginManager::uninstall(std::string_view id)
{
    auto entry = std::find_if(plugins_.begin(), plugins_.end(), [&](const InstalledPlugin& plugin) { return plugin.manifest.id == id; });
    if (entry == plugins_.end())
        return makeError("plugin.missing", "No such plugin");
    std::error_code ec;
    fs::remove_all(entry->directory, ec);
    if (ec)
        return makeError("plugin.uninstall", ec.message());
    plugins_.erase(entry);
    return saveState();
}

Status PluginManager::setEnabled(std::string_view id, bool enabled)
{
    auto entry = std::find_if(plugins_.begin(), plugins_.end(), [&](const InstalledPlugin& plugin) { return plugin.manifest.id == id; });
    if (entry == plugins_.end())
        return makeError("plugin.missing", "No such plugin");
    entry->enabled = enabled;
    if (enabled && entry->tampered) {
        entry->contentHash = hashDirectory(entry->directory);
        entry->tampered = false;
    }
    return saveState();
}

void PluginManager::activate(const Registries& registries)
{
    for (auto& plugin : plugins_) {
        if (!plugin.enabled || !plugin.loadError.empty())
            continue;
        const Json& contributes = plugin.manifest.contributes;
        std::string source = "plugin:" + plugin.manifest.id;
        auto eachItem = [&](const char* key, const std::function<void(const Json&)>& handler) {
            auto it = contributes.find(key);
            if (it == contributes.end() || !it->is_array())
                return;
            for (const auto& item : *it)
                handler(item);
        };
        eachItem("languages", [&](const Json& item) {
            if (registries.languages == nullptr)
                return;
            auto status = registries.languages->addFromJson(item, source);
            if (!status)
                plugin.loadError = status.error().message;
        });
        eachItem("templates", [&](const Json& item) {
            if (registries.templates == nullptr)
                return;
            auto status = registries.templates->addFromJson(item, source);
            if (!status)
                plugin.loadError = status.error().message;
        });
        eachItem("buildSystems", [&](const Json& item) {
            if (registries.builders != nullptr)
                registries.builders->add(build::createDataBuildProvider(item));
        });
        eachItem("generators", [&](const Json& item) {
            if (registries.generators != nullptr)
                registries.generators->add(createExternalGenerator(item, plugin.directory, plugin.manifest.id));
        });
        eachItem("components", [&](const Json& item) {
            if (registries.components == nullptr)
                return;
            ir::ComponentSchema schema;
            schema.type = jsonString(item, "type");
            schema.label = jsonString(item, "label", schema.type);
            schema.category = jsonString(item, "category", plugin.manifest.name);
            schema.icon = jsonString(item, "icon", "custom");
            schema.container = jsonBool(item, "container");
            if (item.contains("defaults"))
                schema.defaults = item["defaults"];
            schema.events = jsonStrings(item, "events");
            if (!schema.type.empty())
                registries.components->add(std::move(schema));
        });
    }
}

std::vector<Json> PluginManager::contributedThemes() const
{
    std::vector<Json> themes;
    for (const auto& plugin : plugins_) {
        if (!plugin.enabled)
            continue;
        auto it = plugin.manifest.contributes.find("themes");
        if (it == plugin.manifest.contributes.end() || !it->is_array())
            continue;
        for (const auto& item : *it)
            themes.push_back(item);
    }
    return themes;
}

namespace {

class ExternalGenerator final : public codegen::ICodeGenerator {
public:
    ExternalGenerator(Json definition, fs::path directory, std::string pluginId)
        : definition_(std::move(definition)), directory_(std::move(directory)), pluginId_(std::move(pluginId))
    {
    }

    codegen::GeneratorInfo info() const override
    {
        codegen::GeneratorInfo value;
        value.id = jsonString(definition_, "id");
        value.label = jsonString(definition_, "label", value.id);
        value.language = jsonString(definition_, "language");
        value.framework = jsonString(definition_, "framework");
        value.description = jsonString(definition_, "description");
        value.platforms = jsonStrings(definition_, "platforms");
        value.requiredTools = jsonStrings(definition_, "tools");
        value.buildSystem = jsonString(definition_, "buildSystem");
        value.approach = "plugin";
        return value;
    }

    codegen::GeneratorCapabilities capabilities() const override { return {}; }
    std::vector<ir::Diagnostic> validate(const ir::ApplicationModel&) const override { return {}; }

    codegen::GenerationResult generate(const ir::ApplicationModel& model, const codegen::ProjectConfiguration& config) const override
    {
        codegen::GenerationResult result;
        auto fail = [&](std::string message) {
            result.diagnostics.push_back({ir::Severity::Error, "plugin.generator", "Plugin " + pluginId_ + ": " + std::move(message), {}, {}, {}});
            return result;
        };
        auto command = jsonStrings(definition_, "command");
        if (command.empty())
            return fail("The generator has no command");
        ProcessSpec spec;
        spec.program = command[0];
        spec.arguments.assign(command.begin() + 1, command.end());
        spec.workingDirectory = directory_;
        spec.origin = Origin::Plugin;
        spec.keepStdinOpen = true;
        spec.memoryLimitBytes = 2ull * 1024 * 1024 * 1024;
        spec.inheritEnvironment = true;
        spec.environment["FORMA_PLUGIN_API"] = std::to_string(kPluginApiVersion);
        for (const char* secret : {"ANTHROPIC_API_KEY", "OPENAI_API_KEY", "GITHUB_TOKEN", "GH_TOKEN", "GITLAB_TOKEN", "AWS_SECRET_ACCESS_KEY"})
            spec.environment[secret] = "";
        Json request = Json::object();
        request["apiVersion"] = kPluginApiVersion;
        request["name"] = config.name;
        request["bundleId"] = config.bundleId;
        request["platforms"] = config.platforms;
        request["options"] = config.options;
        request["model"] = ir::applicationToFullJson(model);
        std::string out;
        std::string err;
        std::mutex mutex;
        auto process = Process::start(
            spec,
            [&](std::string_view data) {
                std::lock_guard lock(mutex);
                out.append(data);
            },
            [&](std::string_view data) {
                std::lock_guard lock(mutex);
                err.append(data);
            });
        if (!process)
            return fail(process.error().message);
        auto wrote = (*process)->writeStdin(dumpJsonCompact(request));
        (void)wrote;
        (*process)->closeStdin();
        if (!(*process)->waitFor(std::chrono::seconds(jsonInt(definition_, "timeoutSeconds", 120)))) {
            (*process)->terminate();
            (*process)->wait();
            return fail("The generator took too long and was stopped");
        }
        int code = (*process)->wait();
        if (code != 0)
            return fail("The generator failed with exit code " + std::to_string(code) + ": " + err.substr(0, 1000));
        auto parsed = parseJson(out);
        if (!parsed)
            return fail("The generator did not return valid JSON");
        auto files = parsed->find("files");
        if (files == parsed->end() || !files->is_array())
            return fail("The generator's reply has no files list");
        for (const auto& item : *files) {
            codegen::GeneratedFile file;
            file.path = jsonString(item, "path");
            if (auto status = validateRelativePath(file.path); !status)
                return fail("It tried to write outside the project: " + file.path);
            if (text::startsWith(normalizeRelativePath(file.path), ".forma/") || text::startsWith(normalizeRelativePath(file.path), ".git/") || text::startsWith(normalizeRelativePath(file.path), "design/"))
                return fail("It tried to write to a protected folder: " + file.path);
            file.content = jsonString(item, "content");
            file.category = codegen::fileCategoryFromName(jsonString(item, "category", "generated"));
            result.files.push_back(std::move(file));
        }
        auto diagnostics = parsed->find("diagnostics");
        if (diagnostics != parsed->end() && diagnostics->is_array()) {
            for (const auto& item : *diagnostics) {
                std::string severity = jsonString(item, "severity", "warning");
                result.diagnostics.push_back({severity == "error" ? ir::Severity::Error : severity == "info" ? ir::Severity::Info : ir::Severity::Warning, "plugin.generator", jsonString(item, "message"), jsonString(item, "screen"), jsonString(item, "component"), {}});
            }
        }
        return result;
    }

private:
    Json definition_;
    fs::path directory_;
    std::string pluginId_;
};

}

std::unique_ptr<codegen::ICodeGenerator> createExternalGenerator(const Json& definition, const fs::path& pluginDirectory, std::string pluginId)
{
    return std::make_unique<ExternalGenerator>(definition, pluginDirectory, std::move(pluginId));
}

}
