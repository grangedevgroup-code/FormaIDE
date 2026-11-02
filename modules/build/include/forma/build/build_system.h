#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "forma/build/toolchain.h"
#include "forma/core/json.h"
#include "forma/core/process.h"

namespace forma::build {

enum class DiagnosticSeverity { Note, Warning, Error };

struct BuildDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string file;
    int line = 0;
    int column = 0;
    std::string code;
    std::string message;
    std::string tool;
};

class DiagnosticParser {
public:
    std::optional<BuildDiagnostic> parseLine(std::string_view line);

private:
    bool hasPendingRust_ = false;
    DiagnosticSeverity pendingRustSeverity_ = DiagnosticSeverity::Error;
    std::string pendingRustCode_;
    std::string pendingRustMessage_;
};

struct Artifact {
    std::string path;
    std::string kind;
    std::uint64_t size = 0;
    std::int64_t modifiedMillis = 0;
};

const char* artifactKindForPath(std::string_view path);
std::vector<Artifact> collectArtifacts(const std::filesystem::path& root, const std::vector<std::string>& searchDirectories, std::int64_t notBeforeMillis = 0);

struct BuildConfiguration {
    std::string id = "debug";
    std::string kind = "debug";
    std::string architecture;
    std::string minimumOs;
    std::string compiler;
    std::string optimization;
    std::vector<std::string> flags;
    std::map<std::string, std::string> environment;
    std::string outputDirectory;
};

struct BuildRequest {
    std::filesystem::path projectDirectory;
    BuildConfiguration configuration;
    std::string targetPlatform;
    std::string startupTarget;
    bool lowerPriority = false;
};

struct BuildStep {
    std::string label;
    ProcessSpec process;
    bool skipIfExists = false;
    std::filesystem::path existsPath;
};

struct BuildPlan {
    std::vector<BuildStep> steps;
    std::vector<std::string> artifactDirectories;
};

class IBuildProvider {
public:
    virtual ~IBuildProvider() = default;
    virtual std::string id() const = 0;
    virtual std::string label() const = 0;
    virtual std::vector<std::string> requiredTools() const = 0;
    virtual bool detect(const std::filesystem::path& projectDirectory) const = 0;
    virtual Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const = 0;
    virtual Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector& tools) const = 0;
};

class BuildSystemRegistry {
public:
    void add(std::unique_ptr<IBuildProvider> provider);
    const IBuildProvider* find(std::string_view id) const;
    const IBuildProvider* detect(const std::filesystem::path& projectDirectory) const;
    std::vector<const IBuildProvider*> all() const;

private:
    std::vector<std::unique_ptr<IBuildProvider>> providers_;
};

void registerBuiltinBuildProviders(BuildSystemRegistry& registry);
std::unique_ptr<IBuildProvider> createDataBuildProvider(const Json& definition);

enum class BuildState { Idle, Running, Succeeded, Failed, Cancelled };

struct BuildResult {
    BuildState state = BuildState::Idle;
    int exitCode = 0;
    std::vector<BuildDiagnostic> diagnostics;
    std::vector<Artifact> artifacts;
    std::int64_t durationMillis = 0;
    std::string failedStep;
};

struct BuildCallbacks {
    std::function<void(std::string_view label)> onStep;
    std::function<void(std::string_view line, bool isError)> onOutput;
    std::function<void(const BuildDiagnostic&)> onDiagnostic;
    std::function<void(const BuildResult&)> onFinished;
};

class BuildJob {
public:
    BuildJob();
    ~BuildJob();
    BuildJob(const BuildJob&) = delete;
    BuildJob& operator=(const BuildJob&) = delete;

    Status start(BuildPlan plan, std::filesystem::path root, BuildCallbacks callbacks);
    void cancel();
    void wait();
    bool running() const;
    const BuildResult& result() const { return result_; }

private:
    void run(BuildPlan plan, std::filesystem::path root, BuildCallbacks callbacks);

    std::thread thread_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::unique_ptr<Process> current_;
    BuildResult result_;
};

BuildResult runBuildBlocking(BuildPlan plan, const std::filesystem::path& root, BuildCallbacks callbacks = {});

}
