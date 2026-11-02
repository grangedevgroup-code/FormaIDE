#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forma/build/build_system.h"
#include "forma/codegen/generator.h"
#include "forma/codegen/pipeline.h"
#include "forma/core/filesystem_service.h"
#include "forma/core/history.h"
#include "forma/core/json.h"
#include "forma/core/recovery.h"
#include "forma/ir/editor.h"

namespace forma::project {

constexpr int kProjectFormat = 1;

struct TargetConfig {
    std::string id;
    std::string label;
    std::string generator;
    std::string outputDirectory = ".";
    std::vector<std::string> platforms;
    std::string buildProvider;
    Json options = Json::object();
};

struct ProjectDescriptor {
    std::string name;
    std::string bundleId;
    std::string version = "0.1.0";
    std::string templateId;
    std::vector<TargetConfig> targets;
    std::string activeTarget;
    std::string designDirectory = "design";
    Json extra = Json::object();

    const TargetConfig* findTarget(std::string_view id) const;
    const TargetConfig* active() const;
};

Json descriptorToJson(const ProjectDescriptor& descriptor);
Result<ProjectDescriptor> descriptorFromJson(const Json& json);

struct ProjectTemplate {
    std::string id;
    std::string name;
    std::string description;
    std::string category;
    std::vector<std::string> platforms;
    std::string language;
    std::string framework;
    std::string generator;
    std::vector<std::string> tools;
    std::string starter = "none";
    std::string preview = "none";
    Json files = Json::object();
    std::string source = "builtin";
};

class TemplateRegistry {
public:
    TemplateRegistry();
    void add(ProjectTemplate item);
    Status addFromJson(const Json& json, std::string source);
    Status addFromDirectory(const std::filesystem::path& directory);
    const ProjectTemplate* find(std::string_view id) const;
    const std::vector<ProjectTemplate>& all() const { return templates_; }

private:
    std::vector<ProjectTemplate> templates_;
};

struct NewProjectOptions {
    std::string name;
    std::filesystem::path location;
    std::string bundleId;
    std::string templateId = "blank";
    std::string generatorOverride;
    std::vector<std::string> platforms;
    bool initGit = false;
};

class ProjectSession {
public:
    static Result<std::unique_ptr<ProjectSession>> create(const NewProjectOptions& options, const TemplateRegistry& templates, const codegen::GeneratorRegistry& generators);
    static Result<std::unique_ptr<ProjectSession>> open(const std::filesystem::path& directory);
    static Result<std::unique_ptr<ProjectSession>> importExisting(const std::filesystem::path& directory);

    ~ProjectSession();

    const std::filesystem::path& root() const { return root_; }
    IFilesystemService& filesystem() { return *filesystem_; }
    const IFilesystemService& filesystem() const { return *filesystem_; }
    ir::ModelEditor& editor() { return editor_; }
    const ir::ApplicationModel& model() const { return editor_.model(); }
    ChangeHistory& history() { return *history_; }
    SnapshotStore& snapshots() { return *snapshots_; }
    RecoveryStore& recovery() { return *recovery_; }
    ProjectDescriptor& descriptor() { return descriptor_; }
    const ProjectDescriptor& descriptor() const { return descriptor_; }

    Status saveDescriptor();
    Status saveDesign();
    Status saveAll();
    Status autosaveDesign();
    bool restoreAutosave();
    void close();

    codegen::ProjectConfiguration configurationFor(const TargetConfig& target) const;
    codegen::GenerationOptions generationOptionsFor(const TargetConfig& target, bool dryRun = false) const;
    codegen::GenerationReport generate(const codegen::GeneratorRegistry& generators, std::string_view targetId = {}, bool dryRun = false);
    Result<codegen::SyncAnalysis> reconcileFromCode(std::string_view targetId = {});
    Status resolveGenerationConflict(const codegen::GeneratorRegistry& generators, std::string_view path, codegen::ConflictChoice choice, std::string_view targetId = {});

    std::filesystem::path targetDirectory(const TargetConfig& target) const;
    const build::IBuildProvider* buildProviderFor(const build::BuildSystemRegistry& registry, const TargetConfig& target) const;

private:
    ProjectSession() = default;
    Status initialize();

    std::filesystem::path root_;
    std::unique_ptr<IFilesystemService> filesystem_;
    std::unique_ptr<ChangeHistory> history_;
    std::unique_ptr<SnapshotStore> snapshots_;
    std::unique_ptr<RecoveryStore> recovery_;
    ir::ModelEditor editor_;
    ProjectDescriptor descriptor_;
    bool closed_ = false;
};

ir::ApplicationModel buildStarterModel(const ProjectDescriptor& descriptor, std::string_view starter);
std::string suggestBundleId(std::string_view name);
Status validateProjectName(std::string_view name);

Status duplicateProject(const std::filesystem::path& source, const std::filesystem::path& destination);
Status renameProject(const std::filesystem::path& directory, std::string_view newName);

struct RecentProject {
    std::string path;
    std::string name;
    std::string lastOpened;
};

class RecentProjects {
public:
    explicit RecentProjects(std::filesystem::path file);
    std::vector<RecentProject> list() const;
    void add(const std::filesystem::path& directory, std::string_view name);
    void remove(const std::filesystem::path& directory);

private:
    std::filesystem::path file_;
};

}
