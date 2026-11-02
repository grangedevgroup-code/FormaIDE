#include "forma/project/project.h"

#include <fstream>
#include <algorithm>

#include "forma/codegen/template_engine.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"
#include "forma/design/design_engine.h"
#include "forma/ir/catalog.h"
#include "forma/ir/serialization.h"
#include "forma/resources/resources.h"

namespace fs = std::filesystem;

namespace forma::project {

const TargetConfig* ProjectDescriptor::findTarget(std::string_view id) const
{
    auto it = std::find_if(targets.begin(), targets.end(), [&](const TargetConfig& target) { return target.id == id; });
    return it == targets.end() ? nullptr : &*it;
}

const TargetConfig* ProjectDescriptor::active() const
{
    if (const TargetConfig* target = findTarget(activeTarget))
        return target;
    return targets.empty() ? nullptr : &targets.front();
}

Json descriptorToJson(const ProjectDescriptor& descriptor)
{
    Json json = Json::object();
    json["formaProject"] = kProjectFormat;
    json["name"] = descriptor.name;
    json["bundleId"] = descriptor.bundleId;
    json["version"] = descriptor.version;
    if (!descriptor.templateId.empty())
        json["template"] = descriptor.templateId;
    json["designDirectory"] = descriptor.designDirectory;
    json["activeTarget"] = descriptor.activeTarget;
    Json targets = Json::array();
    for (const auto& target : descriptor.targets) {
        Json entry = Json::object();
        entry["id"] = target.id;
        entry["label"] = target.label;
        entry["generator"] = target.generator;
        entry["outputDirectory"] = target.outputDirectory;
        entry["platforms"] = target.platforms;
        if (!target.buildProvider.empty())
            entry["buildProvider"] = target.buildProvider;
        if (!target.options.empty())
            entry["options"] = target.options;
        targets.push_back(entry);
    }
    json["targets"] = targets;
    for (auto item = descriptor.extra.begin(); item != descriptor.extra.end(); ++item)
        json[item.key()] = *item;
    return json;
}

Result<ProjectDescriptor> descriptorFromJson(const Json& json)
{
    if (!json.is_object())
        return makeError("project.parse", "project.json must be an object");
    int format = jsonInt(json, "formaProject", 0);
    if (format < 1)
        return makeError("project.parse", "This is not a FormaIDE project file");
    if (format > kProjectFormat)
        return makeError("project.version", "This project was made by a newer version of FormaIDE");
    ProjectDescriptor descriptor;
    descriptor.name = jsonString(json, "name");
    descriptor.bundleId = jsonString(json, "bundleId");
    descriptor.version = jsonString(json, "version", "0.1.0");
    descriptor.templateId = jsonString(json, "template");
    descriptor.designDirectory = jsonString(json, "designDirectory", "design");
    descriptor.activeTarget = jsonString(json, "activeTarget");
    if (auto status = validateRelativePath(descriptor.designDirectory); !status)
        return status.error();
    auto targets = json.find("targets");
    if (targets != json.end() && targets->is_array()) {
        for (const auto& item : *targets) {
            TargetConfig target;
            target.id = jsonString(item, "id");
            target.label = jsonString(item, "label", target.id);
            target.generator = jsonString(item, "generator");
            target.outputDirectory = jsonString(item, "outputDirectory", ".");
            target.platforms = jsonStrings(item, "platforms");
            target.buildProvider = jsonString(item, "buildProvider");
            if (item.contains("options"))
                target.options = item["options"];
            if (target.outputDirectory != ".") {
                if (auto status = validateRelativePath(target.outputDirectory); !status)
                    return status.error();
            }
            descriptor.targets.push_back(std::move(target));
        }
    }
    static const std::vector<std::string> known = {"formaProject", "name", "bundleId", "version", "template", "designDirectory", "activeTarget", "targets"};
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (std::find(known.begin(), known.end(), item.key()) == known.end())
            descriptor.extra[item.key()] = *item;
    }
    return descriptor;
}

TemplateRegistry::TemplateRegistry()
{
    auto data = resources::find("templates/templates.json");
    if (!data)
        return;
    auto parsed = parseJson(*data);
    if (!parsed || !parsed->is_array())
        return;
    for (const auto& item : *parsed) {
        auto status = addFromJson(item, "builtin");
        (void)status;
    }
}

void TemplateRegistry::add(ProjectTemplate item)
{
    auto existing = std::find_if(templates_.begin(), templates_.end(), [&](const ProjectTemplate& entry) { return entry.id == item.id; });
    if (existing != templates_.end())
        *existing = std::move(item);
    else
        templates_.push_back(std::move(item));
}

Status TemplateRegistry::addFromJson(const Json& json, std::string source)
{
    ProjectTemplate item;
    item.id = jsonString(json, "id");
    if (item.id.empty())
        return makeError("template.parse", "A template needs an id");
    item.name = jsonString(json, "name", item.id);
    item.description = jsonString(json, "description");
    item.category = jsonString(json, "category", "Custom");
    item.platforms = jsonStrings(json, "platforms");
    item.language = jsonString(json, "language", "Any");
    item.framework = jsonString(json, "framework", "None");
    item.generator = jsonString(json, "generator");
    item.tools = jsonStrings(json, "tools");
    item.starter = jsonString(json, "starter", "none");
    item.preview = jsonString(json, "preview", "none");
    if (json.contains("files") && json["files"].is_object())
        item.files = json["files"];
    item.source = std::move(source);
    add(std::move(item));
    return {};
}

Status TemplateRegistry::addFromDirectory(const fs::path& directory)
{
    auto content = paths::readWholeFile(directory / "forma-template.json");
    if (!content)
        return content.error();
    auto parsed = parseJson(*content);
    if (!parsed)
        return parsed.error();
    Json definition = *parsed;
    Json files = definition.contains("files") ? definition["files"] : Json::object();
    std::error_code ec;
    fs::path filesRoot = directory / "files";
    if (fs::is_directory(filesRoot, ec)) {
        for (fs::recursive_directory_iterator it(filesRoot, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            auto body = paths::readWholeFile(it->path());
            if (body)
                files[paths::toGeneric(it->path().lexically_relative(filesRoot))] = *body;
        }
    }
    definition["files"] = files;
    return addFromJson(definition, paths::toUtf8(directory));
}

const ProjectTemplate* TemplateRegistry::find(std::string_view id) const
{
    auto it = std::find_if(templates_.begin(), templates_.end(), [&](const ProjectTemplate& entry) { return entry.id == id; });
    return it == templates_.end() ? nullptr : &*it;
}

std::string suggestBundleId(std::string_view name)
{
    std::string slug;
    for (char ch : text::toLower(name)) {
        if (std::isalnum(static_cast<unsigned char>(ch)))
            slug.push_back(ch);
    }
    if (slug.empty() || !std::isalpha(static_cast<unsigned char>(slug[0])))
        slug = "app" + slug;
    return "com.example." + slug;
}

Status validateProjectName(std::string_view name)
{
    std::string trimmed = text::trim(name);
    if (trimmed.empty())
        return makeError("project.name", "Give the project a name");
    if (trimmed.size() > 100)
        return makeError("project.name", "That name is too long");
    if (!std::isalpha(static_cast<unsigned char>(trimmed[0])))
        return makeError("project.name", "The name must start with a letter");
    for (char ch : trimmed) {
        auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != ' ' && ch != '-' && ch != '_')
            return makeError("project.name", "Use letters, numbers, spaces, dashes and underscores");
    }
    return {};
}

ir::ApplicationModel buildStarterModel(const ProjectDescriptor& descriptor, std::string_view starter)
{
    ir::ApplicationModel model = ir::makeEmptyApplication(descriptor.name, descriptor.bundleId);
    model.meta.version = descriptor.version;
    if (starter == "none" || starter.empty())
        return model;

    double width = 390;
    double height = 844;
    std::string device = "iphone-16";
    if (starter == "desktop") {
        width = 960;
        height = 640;
        device = "window-small";
    } else if (starter == "web") {
        width = 1280;
        height = 800;
        device = "desktop-hd";
    } else if (std::find(descriptor.targets.front().platforms.begin(), descriptor.targets.front().platforms.end(), "android") != descriptor.targets.front().platforms.end()) {
        width = 412;
        height = 924;
        device = "pixel-9";
    } else {
        width = 393;
        height = 852;
    }
    ir::Screen home = ir::makeScreen("home", "Home", width, height);
    home.device = device;
    ir::Component title = ir::builtinCatalog().instantiate("text", "title");
    title.name = "Title";
    title.properties["content"] = descriptor.name;
    title.typography.ref = "heading";
    home.root.children.push_back(title);
    ir::Component body = ir::builtinCatalog().instantiate("text", "subtitle");
    body.name = "Subtitle";
    body.properties["content"] = "Edit this screen on the canvas, then press Run.";
    body.typography.color = "$color.textMuted";
    home.root.children.push_back(body);
    ir::Component button = ir::builtinCatalog().instantiate("button", "continue_button");
    button.name = "Continue";
    button.properties["label"] = "Continue";
    button.events["onClick"] = "open_details";
    button.alignSelf = ir::Align::Start;
    home.root.children.push_back(button);

    ir::Screen details = ir::makeScreen("details", "Details", width, height);
    details.device = device;
    details.canvas.x = width + 120;
    ir::Component back = ir::builtinCatalog().instantiate("button", "back_button");
    back.name = "Back";
    back.properties["label"] = "Back";
    back.properties["variant"] = "secondary";
    back.events["onClick"] = "go_back";
    back.alignSelf = ir::Align::Start;
    ir::Component heading = ir::builtinCatalog().instantiate("text", "details_title");
    heading.name = "Title";
    heading.properties["content"] = "Details";
    heading.typography.ref = "subheading";
    details.root.children.push_back(heading);
    details.root.children.push_back(back);

    model.screens.push_back(std::move(home));
    model.screens.push_back(std::move(details));
    model.navigation.start = "home";
    ir::Action open;
    open.id = "open_details";
    open.kind = "navigate";
    open.target = "details";
    ir::Action goBack;
    goBack.id = "go_back";
    goBack.kind = "back";
    model.actions = {open, goBack};
    model.navigation.routes.push_back({"home_to_details", "home", "details", ""});
    return model;
}

ProjectSession::~ProjectSession()
{
    close();
}

void ProjectSession::close()
{
    if (closed_)
        return;
    closed_ = true;
    if (recovery_) {
        if (!editor_.isDirty()) {
            auto cleared = recovery_->clearDesignState();
            (void)cleared;
        }
        recovery_->endSession();
    }
}

Status ProjectSession::initialize()
{
    filesystem_ = createLocalFilesystemService(root_);
    root_ = filesystem_->root();
    if (auto status = filesystem_->createDirectories(".forma"); !status)
        return status;
    if (std::error_code ec; !fs::exists(root_ / ".forma" / ".gitignore", ec)) {
        std::ofstream ignore(root_ / ".forma" / ".gitignore", std::ios::binary);
        ignore << "*\n!.gitignore\n!generated/\n!generated/**\n";
    }
    if (auto status = filesystem_->createDirectories(".forma"); !status)
        return status;
    auto opened = ChangeHistory::open(root_ / ".forma" / "project.db");
    if (!opened)
        return opened.error();
    history_ = std::move(opened.value());
    snapshots_ = std::make_unique<SnapshotStore>(*filesystem_);
    recovery_ = std::make_unique<RecoveryStore>(root_);
    recovery_->beginSession();
    editor_.setHistoryRecorder([this](const std::string& label, const std::vector<ir::EditOperation>& operations) {
        Json details = Json::object();
        details["operations"] = operations.size();
        std::vector<std::string> components;
        for (const auto& operation : operations) {
            if (!operation.component.empty() && std::find(components.begin(), components.end(), operation.component) == components.end())
                components.push_back(operation.component);
        }
        details["components"] = components;
        auto status = history_->record("design", "user", label, details);
        (void)status;
    });
    filesystem_->setOperationListener([this](const FileOperation& operation) {
        if (operation.origin == Origin::User && text::startsWith(operation.path, ".forma/"))
            return;
        if (text::startsWith(operation.path, ".forma/"))
            return;
        Json details = Json::object();
        details["path"] = operation.path;
        details["action"] = operation.action;
        if (!operation.recoveryId.empty())
            details["recovery"] = operation.recoveryId;
        std::string kind = operation.origin == Origin::Generator ? "generated" : operation.origin == Origin::AI ? "ai" : "file";
        auto status = history_->record(kind, originName(operation.origin), operation.action + " " + operation.path, details);
        (void)status;
    });
    return {};
}

Result<std::unique_ptr<ProjectSession>> ProjectSession::create(const NewProjectOptions& options, const TemplateRegistry& templates, const codegen::GeneratorRegistry& generators)
{
    if (auto status = validateProjectName(options.name); !status)
        return status.error();
    const ProjectTemplate* chosen = templates.find(options.templateId);
    if (chosen == nullptr)
        return makeError("project.template", "No such template: " + options.templateId);
    std::error_code ec;
    fs::path directory = options.location;
    if (fs::exists(directory, ec) && !fs::is_empty(directory, ec))
        return makeError("project.exists", "The folder already exists and is not empty: " + paths::toUtf8(directory));
    fs::create_directories(directory, ec);
    if (ec)
        return makeError("project.location", "Cannot create " + paths::toUtf8(directory) + ": " + ec.message());

    std::unique_ptr<ProjectSession> session(new ProjectSession());
    session->root_ = directory;
    if (auto status = session->initialize(); !status)
        return status.error();

    ProjectDescriptor& descriptor = session->descriptor_;
    descriptor.name = text::trim(options.name);
    descriptor.bundleId = options.bundleId.empty() ? suggestBundleId(descriptor.name) : options.bundleId;
    descriptor.templateId = chosen->id;

    std::string generatorId = options.generatorOverride.empty() ? chosen->generator : options.generatorOverride;
    TargetConfig target;
    target.id = "main";
    if (!generatorId.empty()) {
        const codegen::ICodeGenerator* generator = generators.find(generatorId);
        if (generator == nullptr)
            return makeError("project.generator", "No such code generator: " + generatorId);
        target.label = generator->info().label;
        target.generator = generatorId;
        target.platforms = options.platforms.empty() ? generator->info().platforms : options.platforms;
        target.buildProvider = generator->info().buildSystem;
    } else {
        target.label = chosen->name;
        target.platforms = options.platforms.empty() ? chosen->platforms : options.platforms;
    }
    descriptor.targets.push_back(target);
    descriptor.activeTarget = target.id;

    Json context = Json::object();
    context["name"] = descriptor.name;
    context["slug"] = text::toKebabCase(descriptor.name);
    context["snake"] = text::toSnakeCase(descriptor.name);
    context["class"] = text::toPascalCase(descriptor.name);
    context["bundleId"] = descriptor.bundleId;
    for (auto item = chosen->files.begin(); item != chosen->files.end(); ++item) {
        auto path = codegen::renderTemplate(item.key(), context);
        if (!path)
            return path.error();
        auto body = codegen::renderTemplate(item->get<std::string>(), context);
        if (!body)
            return body.error();
        if (auto status = session->filesystem_->writeFile(*path, *body); !status)
            return status.error();
    }

    ir::ApplicationModel model = buildStarterModel(descriptor, chosen->starter);
    session->editor_.reset(std::move(model));
    if (auto status = session->saveAll(); !status)
        return status.error();

    if (!generatorId.empty()) {
        auto report = session->generate(generators);
        if (report.blockedByErrors)
            return makeError("project.generate", "The starter design did not pass validation");
    }

    if (options.initGit) {
        ProcessSpec git;
        git.program = "git";
        git.arguments = {"init", "-q", "-b", "main"};
        git.workingDirectory = session->root_;
        git.origin = Origin::System;
        auto result = runProcess(git, std::chrono::seconds(30));
        if (result && result->exitCode == 0) {
            auto ignore = session->filesystem_->readFile(".gitignore");
            std::string contents = ignore ? *ignore : std::string();
            if (!text::contains(contents, ".forma/")) {
                contents += ".forma/\n";
                auto status = session->filesystem_->writeFile(".gitignore", contents);
                (void)status;
            }
        } else {
            log::warning("project", "git init failed");
        }
    }
    auto recorded = session->history_->record("project", "user", "Created project from template " + chosen->name);
    (void)recorded;
    return session;
}

Result<std::unique_ptr<ProjectSession>> ProjectSession::open(const fs::path& directory)
{
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return makeError("project.missing", "No such folder: " + paths::toUtf8(directory));
    auto content = paths::readWholeFile(directory / "project.json");
    if (!content)
        return makeError("project.missing", "This folder has no project.json. Use Import to work on an existing folder.");
    auto parsed = parseJson(*content);
    if (!parsed)
        return makeError("project.parse", "project.json is damaged: " + parsed.error().message);
    auto descriptor = descriptorFromJson(*parsed);
    if (!descriptor)
        return descriptor.error();

    std::unique_ptr<ProjectSession> session(new ProjectSession());
    session->root_ = directory;
    if (auto status = session->initialize(); !status)
        return status.error();
    session->descriptor_ = std::move(descriptor.value());

    if (session->filesystem_->exists(session->descriptor_.designDirectory + "/application.ir")) {
        auto model = ir::loadApplication(*session->filesystem_, session->descriptor_.designDirectory);
        if (!model)
            return model.error();
        session->editor_.reset(std::move(model.value()));
    } else {
        session->editor_.reset(ir::makeEmptyApplication(session->descriptor_.name, session->descriptor_.bundleId));
    }
    return session;
}

Result<std::unique_ptr<ProjectSession>> ProjectSession::importExisting(const fs::path& directory)
{
    std::error_code ec;
    if (!fs::is_directory(directory, ec))
        return makeError("project.missing", "No such folder: " + paths::toUtf8(directory));
    if (fs::exists(directory / "project.json", ec)) {
        auto opened = open(directory);
        if (opened)
            return opened;
        if (opened.error().code != "project.parse")
            return opened;
        return makeError("project.exists", "This folder has a project.json that belongs to another tool. Rename it to import the folder.");
    }
    std::unique_ptr<ProjectSession> session(new ProjectSession());
    session->root_ = directory;
    if (auto status = session->initialize(); !status)
        return status.error();
    ProjectDescriptor& descriptor = session->descriptor_;
    descriptor.name = paths::toUtf8(fs::weakly_canonical(directory, ec).filename());
    std::string cleanName;
    for (char ch : descriptor.name) {
        auto c = static_cast<unsigned char>(ch);
        cleanName.push_back(std::isalnum(c) || ch == ' ' || ch == '-' || ch == '_' ? ch : '-');
    }
    descriptor.name = cleanName.empty() ? "Imported" : cleanName;
    descriptor.bundleId = suggestBundleId(descriptor.name);
    descriptor.templateId = "imported";
    TargetConfig target;
    target.id = "main";
    target.label = "This folder";
    descriptor.targets.push_back(target);
    descriptor.activeTarget = "main";
    session->editor_.reset(ir::makeEmptyApplication(descriptor.name, descriptor.bundleId));
    if (auto status = session->saveDescriptor(); !status)
        return status.error();
    auto recorded = session->history_->record("project", "user", "Imported an existing folder");
    (void)recorded;
    return session;
}

Status ProjectSession::saveDescriptor()
{
    WriteOptions options;
    options.origin = Origin::System;
    options.backup = false;
    return filesystem_->writeFile("project.json", dumpJson(descriptorToJson(descriptor_)), options);
}

Status ProjectSession::saveDesign()
{
    if (auto status = ir::saveApplication(*filesystem_, editor_.model(), descriptor_.designDirectory, Origin::System); !status)
        return status;
    editor_.markClean();
    return recovery_->clearDesignState();
}

Status ProjectSession::saveAll()
{
    if (auto status = saveDescriptor(); !status)
        return status;
    return saveDesign();
}

Status ProjectSession::autosaveDesign()
{
    if (!editor_.isDirty())
        return {};
    return recovery_->saveDesignState(ir::applicationToFullJson(editor_.model()));
}

bool ProjectSession::restoreAutosave()
{
    auto state = recovery_->designState();
    if (!state)
        return false;
    auto model = ir::applicationFromFullJson(*state);
    if (!model)
        return false;
    editor_.reset(std::move(model.value()));
    return true;
}

codegen::ProjectConfiguration ProjectSession::configurationFor(const TargetConfig& target) const
{
    codegen::ProjectConfiguration configuration;
    configuration.name = descriptor_.name;
    configuration.bundleId = descriptor_.bundleId;
    configuration.platforms = target.platforms;
    configuration.options = target.options;
    return configuration;
}

codegen::GenerationOptions ProjectSession::generationOptionsFor(const TargetConfig& target, bool dryRun) const
{
    codegen::GenerationOptions options;
    options.target = target.id;
    options.outputDirectory = target.outputDirectory;
    options.dryRun = dryRun;
    return options;
}

codegen::GenerationReport ProjectSession::generate(const codegen::GeneratorRegistry& generators, std::string_view targetId, bool dryRun)
{
    codegen::GenerationReport report;
    const TargetConfig* target = targetId.empty() ? descriptor_.active() : descriptor_.findTarget(targetId);
    if (target == nullptr || target->generator.empty()) {
        report.blockedByErrors = true;
        report.diagnostics.push_back({ir::Severity::Error, "generator.none", "This target has no code generator", {}, {}, {}});
        return report;
    }
    const codegen::ICodeGenerator* generator = generators.find(target->generator);
    if (generator == nullptr) {
        report.blockedByErrors = true;
        report.diagnostics.push_back({ir::Severity::Error, "generator.missing", "The code generator " + target->generator + " is not available", {}, {}, {}});
        return report;
    }
    report = codegen::runGeneration(*filesystem_, *generator, editor_.model(), configurationFor(*target), generationOptionsFor(*target, dryRun));
    if (!dryRun && !report.blockedByErrors && report.filesChanged()) {
        Json details = Json::object();
        details["generator"] = target->generator;
        Json files = Json::array();
        for (const auto& file : report.files) {
            if (file.outcome != codegen::FileOutcome::Unchanged)
                files.push_back(Json{{"path", file.path}, {"outcome", codegen::fileOutcomeName(file.outcome)}});
        }
        details["files"] = files;
        auto status = history_->record("generated", "generator", "Generated " + generator->info().label + " code", details);
        (void)status;
    }
    return report;
}

Result<codegen::SyncAnalysis> ProjectSession::reconcileFromCode(std::string_view targetId)
{
    const TargetConfig* target = targetId.empty() ? descriptor_.active() : descriptor_.findTarget(targetId);
    if (target == nullptr)
        return makeError("project.target", "No such target");
    codegen::SyncAnalysis analysis = codegen::analyzeCodeChanges(*filesystem_, editor_.model(), target->id);
    if (auto status = codegen::applyCodeChanges(editor_, analysis); !status)
        return status.error();
    return analysis;
}

Status ProjectSession::resolveGenerationConflict(const codegen::GeneratorRegistry& generators, std::string_view path, codegen::ConflictChoice choice, std::string_view targetId)
{
    const TargetConfig* target = targetId.empty() ? descriptor_.active() : descriptor_.findTarget(targetId);
    if (target == nullptr)
        return makeError("project.target", "No such target");
    const codegen::ICodeGenerator* generator = generators.find(target->generator);
    if (generator == nullptr)
        return makeError("generator.missing", "The code generator is not available");
    return codegen::resolveConflict(*filesystem_, *generator, editor_.model(), configurationFor(*target), generationOptionsFor(*target), path, choice);
}

fs::path ProjectSession::targetDirectory(const TargetConfig& target) const
{
    if (target.outputDirectory.empty() || target.outputDirectory == ".")
        return root_;
    return root_ / paths::fromUtf8(target.outputDirectory);
}

const build::IBuildProvider* ProjectSession::buildProviderFor(const build::BuildSystemRegistry& registry, const TargetConfig& target) const
{
    if (!target.buildProvider.empty()) {
        if (const build::IBuildProvider* provider = registry.find(target.buildProvider))
            return provider;
    }
    return registry.detect(targetDirectory(target));
}

Status duplicateProject(const fs::path& source, const fs::path& destination)
{
    std::error_code ec;
    if (!fs::is_directory(source, ec))
        return makeError("project.missing", "No such project: " + paths::toUtf8(source));
    if (fs::exists(destination, ec))
        return makeError("project.exists", "The destination already exists: " + paths::toUtf8(destination));
    fs::create_directories(destination, ec);
    if (ec)
        return makeError("project.duplicate", ec.message());
    const auto& ignored = defaultIgnoredNames();
    std::function<Status(const fs::path&, const fs::path&)> copy = [&](const fs::path& from, const fs::path& to) -> Status {
        std::error_code inner;
        for (fs::directory_iterator it(from, inner), end; !inner && it != end; it.increment(inner)) {
            std::string name = paths::toUtf8(it->path().filename());
            if (std::find(ignored.begin(), ignored.end(), name) != ignored.end() && name != ".forma")
                continue;
            if (name == ".forma")
                continue;
            if (it->is_directory(inner)) {
                fs::create_directories(to / it->path().filename(), inner);
                if (auto status = copy(it->path(), to / it->path().filename()); !status)
                    return status;
            } else {
                fs::copy_file(it->path(), to / it->path().filename(), inner);
                if (inner)
                    return makeError("project.duplicate", inner.message());
            }
        }
        return {};
    };
    return copy(source, destination);
}

Status renameProject(const fs::path& directory, std::string_view newName)
{
    if (auto status = validateProjectName(newName); !status)
        return status;
    auto content = paths::readWholeFile(directory / "project.json");
    if (!content)
        return content.error();
    auto parsed = parseJson(*content);
    if (!parsed)
        return parsed.error();
    (*parsed)["name"] = text::trim(newName);
    return paths::writeFileAtomic(directory / "project.json", dumpJson(*parsed));
}

RecentProjects::RecentProjects(fs::path file) : file_(std::move(file)) {}

std::vector<RecentProject> RecentProjects::list() const
{
    std::vector<RecentProject> projects;
    auto content = paths::readWholeFile(file_);
    if (!content)
        return projects;
    auto parsed = parseJson(*content);
    if (!parsed || !parsed->is_array())
        return projects;
    for (const auto& item : *parsed)
        projects.push_back({jsonString(item, "path"), jsonString(item, "name"), jsonString(item, "lastOpened")});
    return projects;
}

void RecentProjects::add(const fs::path& directory, std::string_view name)
{
    std::error_code ec;
    std::string path = paths::toUtf8(fs::weakly_canonical(directory, ec));
    auto projects = list();
    projects.erase(std::remove_if(projects.begin(), projects.end(), [&](const RecentProject& item) { return item.path == path; }), projects.end());
    projects.insert(projects.begin(), {path, std::string(name), paths::nowIso8601()});
    if (projects.size() > 25)
        projects.resize(25);
    Json json = Json::array();
    for (const auto& item : projects)
        json.push_back(Json{{"path", item.path}, {"name", item.name}, {"lastOpened", item.lastOpened}});
    auto status = paths::writeFileAtomic(file_, dumpJson(json));
    (void)status;
}

void RecentProjects::remove(const fs::path& directory)
{
    std::error_code ec;
    std::string path = paths::toUtf8(fs::weakly_canonical(directory, ec));
    auto projects = list();
    projects.erase(std::remove_if(projects.begin(), projects.end(), [&](const RecentProject& item) { return item.path == path; }), projects.end());
    Json json = Json::array();
    for (const auto& item : projects)
        json.push_back(Json{{"path", item.path}, {"name", item.name}, {"lastOpened", item.lastOpened}});
    auto status = paths::writeFileAtomic(file_, dumpJson(json));
    (void)status;
}

}
