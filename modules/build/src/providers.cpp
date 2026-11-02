#include <algorithm>
#include <regex>

#include "forma/build/build_system.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma::build {

void BuildSystemRegistry::add(std::unique_ptr<IBuildProvider> provider)
{
    std::string id = provider->id();
    auto existing = std::find_if(providers_.begin(), providers_.end(), [&](const auto& item) { return item->id() == id; });
    if (existing != providers_.end())
        *existing = std::move(provider);
    else
        providers_.push_back(std::move(provider));
}

const IBuildProvider* BuildSystemRegistry::find(std::string_view id) const
{
    auto it = std::find_if(providers_.begin(), providers_.end(), [&](const auto& item) { return item->id() == id; });
    return it == providers_.end() ? nullptr : it->get();
}

const IBuildProvider* BuildSystemRegistry::detect(const fs::path& projectDirectory) const
{
    for (const auto& provider : providers_) {
        if (provider->detect(projectDirectory))
            return provider.get();
    }
    return nullptr;
}

std::vector<const IBuildProvider*> BuildSystemRegistry::all() const
{
    std::vector<const IBuildProvider*> output;
    for (const auto& provider : providers_)
        output.push_back(provider.get());
    return output;
}

namespace {

bool hasFile(const fs::path& directory, const char* name)
{
    std::error_code ec;
    return fs::exists(directory / name, ec);
}

bool hasExtension(const fs::path& directory, const char* extension)
{
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == extension)
            return true;
    }
    return false;
}

std::string configName(const BuildConfiguration& configuration)
{
    if (configuration.kind == "release")
        return "Release";
    if (configuration.kind == "profile")
        return "RelWithDebInfo";
    return "Debug";
}

std::string buildDirectoryFor(const BuildConfiguration& configuration)
{
    if (!configuration.outputDirectory.empty())
        return configuration.outputDirectory;
    return "build/" + text::toLower(configuration.id.empty() ? std::string("debug") : configuration.id);
}

ProcessSpec baseProcess(const BuildRequest& request, std::string program, std::vector<std::string> arguments)
{
    ProcessSpec spec;
    spec.program = std::move(program);
    spec.arguments = std::move(arguments);
    spec.workingDirectory = request.projectDirectory;
    spec.environment = request.configuration.environment;
    spec.origin = Origin::Build;
    spec.lowerPriority = request.lowerPriority;
    return spec;
}

Status requireTool(ToolchainDetector& tools, const char* id)
{
    ToolInfo tool = tools.detect(id);
    if (tool.available)
        return {};
    return makeError("toolchain.missing", tool.name + " is not installed. " + tool.installHint);
}

class CMakeProvider final : public IBuildProvider {
public:
    std::string id() const override { return "cmake"; }
    std::string label() const override { return "CMake"; }
    std::vector<std::string> requiredTools() const override { return {"cmake"}; }
    bool detect(const fs::path& directory) const override { return hasFile(directory, "CMakeLists.txt"); }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (auto status = requireTool(tools, "cmake"); !status)
            return status.error();
        std::string buildDirectory = buildDirectoryFor(request.configuration);
        std::map<std::string, std::string> environment = request.configuration.environment;
        std::vector<std::string> configure = {"-S", ".", "-B", buildDirectory, "-DCMAKE_BUILD_TYPE=" + configName(request.configuration), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
        bool useNinja = tools.isAvailable("ninja");
#if defined(_WIN32)
        auto msvc = tools.msvcEnvironment();
        bool haveMsvc = !msvc.empty();
        if (haveMsvc) {
            for (const auto& [key, value] : msvc) {
                if (environment.find(key) == environment.end())
                    environment[key] = value;
            }
        } else if (!tools.isAvailable("gcc") && !tools.isAvailable("clang")) {
            return makeError("toolchain.missing", "No C++ compiler was found. Install Visual Studio with the C++ workload, or LLVM.");
        }
#else
        if (!tools.isAvailable("gcc") && !tools.isAvailable("clang"))
            return makeError("toolchain.missing", "No C++ compiler was found. Install GCC or Clang.");
#endif
        if (useNinja) {
            configure.push_back("-G");
            configure.push_back("Ninja");
        }
        if (auto kit = tools.bestQtKit()) {
            std::error_code ec;
            std::string contents;
            auto listing = paths::readWholeFile(request.projectDirectory / "CMakeLists.txt");
            if (listing && text::contains(*listing, "Qt6"))
                configure.push_back("-DCMAKE_PREFIX_PATH=" + paths::toGeneric(kit->prefix));
        }
        for (const auto& flag : request.configuration.flags)
            configure.push_back(flag);

        BuildPlan plan;
        ProcessSpec configureStep = baseProcess(request, "cmake", configure);
        configureStep.environment = environment;
        plan.steps.push_back({"Configure", configureStep});
        ProcessSpec buildStep = baseProcess(request, "cmake", {"--build", buildDirectory, "--config", configName(request.configuration)});
        buildStep.environment = environment;
        plan.steps.push_back({"Compile", buildStep});
        plan.artifactDirectories = {buildDirectory};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        std::string buildDirectory = buildDirectoryFor(request.configuration);
        auto artifacts = collectArtifacts(request.projectDirectory, {buildDirectory});
        const Artifact* executable = nullptr;
        for (const auto& artifact : artifacts) {
            if (artifact.kind == "Windows executable" || artifact.kind == "Executable") {
                if (executable == nullptr || artifact.modifiedMillis > executable->modifiedMillis)
                    executable = &artifact;
            }
        }
#if defined(__APPLE__)
        for (const auto& artifact : artifacts) {
            if (artifact.kind == "macOS or iOS application") {
                fs::path app = request.projectDirectory / paths::fromUtf8(artifact.path);
                fs::path binary = app / "Contents" / "MacOS" / app.stem();
                ProcessSpec spec = baseProcess(request, paths::toUtf8(binary), {});
                return spec;
            }
        }
#endif
        if (executable == nullptr)
            return makeError("run.noartifact", "No executable was found in " + buildDirectory + ". Build the project first.");
        ProcessSpec spec = baseProcess(request, paths::toUtf8(request.projectDirectory / paths::fromUtf8(executable->path)), {});
#if defined(_WIN32)
        if (auto kit = tools.bestQtKit()) {
            std::string pathValue = paths::toUtf8(kit->prefix / "bin") + ";" + paths::env("PATH").value_or("");
            spec.environment["PATH"] = pathValue;
        }
#else
        (void)tools;
#endif
        return spec;
    }
};

class GradleProvider final : public IBuildProvider {
public:
    std::string id() const override { return "gradle"; }
    std::string label() const override { return "Gradle"; }
    std::vector<std::string> requiredTools() const override { return {"jdk", "gradle"}; }
    bool detect(const fs::path& directory) const override
    {
        return hasFile(directory, "settings.gradle.kts") || hasFile(directory, "settings.gradle") || hasFile(directory, "build.gradle.kts") || hasFile(directory, "build.gradle");
    }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (auto status = requireTool(tools, "jdk"); !status)
            return status.error();
        BuildPlan plan;
        std::string wrapper = localWrapper(request.projectDirectory);
        std::map<std::string, std::string> environment = environmentFor(request, tools);
        bool android = isAndroid(request.projectDirectory);
        if (android) {
            if (auto status = requireTool(tools, "android-sdk"); !status)
                return status.error();
        }
        if (wrapper.empty()) {
            auto gradle = tools.gradle();
            if (!gradle)
                return makeError("toolchain.missing", "This project has no Gradle wrapper and Gradle is not installed. " + tools.detect("gradle").installHint);
            ProcessSpec wrapperStep = baseProcess(request, paths::toUtf8(*gradle), {"wrapper", "--no-daemon"});
            wrapperStep.environment = environment;
            plan.steps.push_back({"Create the Gradle wrapper", wrapperStep});
            wrapper = paths::toUtf8(request.projectDirectory / wrapperName());
        }
        std::string task;
        if (android)
            task = request.configuration.kind == "release" ? "assembleRelease" : "assembleDebug";
        else
            task = "build";
        ProcessSpec buildStep = baseProcess(request, wrapper, {task, "--console=plain"});
        buildStep.environment = environment;
        plan.steps.push_back({"Build", buildStep});
        plan.artifactDirectories = {"app/build/outputs", "build/libs"};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (isAndroid(request.projectDirectory)) {
            auto adb = tools.detect("adb");
            if (!adb.available)
                return makeError("toolchain.missing", "adb was not found. " + adb.installHint);
            std::string wrapper = localWrapper(request.projectDirectory);
            if (wrapper.empty())
                return makeError("run.noartifact", "Build the project first so that the Gradle wrapper exists.");
            ProcessSpec spec = baseProcess(request, wrapper, {request.configuration.kind == "release" ? "installRelease" : "installDebug", "--console=plain"});
            spec.environment = environmentFor(request, tools);
            return spec;
        }
        std::string wrapper = localWrapper(request.projectDirectory);
        if (wrapper.empty())
            return makeError("run.noartifact", "Build the project first so that the Gradle wrapper exists.");
        ProcessSpec spec = baseProcess(request, wrapper, {"run", "--console=plain"});
        spec.environment = environmentFor(request, tools);
        return spec;
    }

private:
    static std::string wrapperName()
    {
#if defined(_WIN32)
        return "gradlew.bat";
#else
        return "gradlew";
#endif
    }

    static std::string localWrapper(const fs::path& directory)
    {
        std::error_code ec;
        fs::path path = directory / wrapperName();
        if (fs::exists(path, ec))
            return paths::toUtf8(path);
        return {};
    }

    static bool isAndroid(const fs::path& directory)
    {
        auto settings = paths::readWholeFile(directory / "app" / "build.gradle.kts");
        if (settings && text::contains(*settings, "com.android."))
            return true;
        auto groovy = paths::readWholeFile(directory / "app" / "build.gradle");
        return groovy && text::contains(*groovy, "com.android.");
    }

    static std::map<std::string, std::string> environmentFor(const BuildRequest& request, ToolchainDetector& tools)
    {
        std::map<std::string, std::string> environment = request.configuration.environment;
        if (auto java = tools.javaHome())
            environment["JAVA_HOME"] = paths::toUtf8(*java);
        if (auto sdk = tools.androidSdk()) {
            environment["ANDROID_HOME"] = paths::toUtf8(*sdk);
            environment["ANDROID_SDK_ROOT"] = paths::toUtf8(*sdk);
        }
        return environment;
    }
};

class DotnetProvider final : public IBuildProvider {
public:
    std::string id() const override { return "dotnet"; }
    std::string label() const override { return ".NET"; }
    std::vector<std::string> requiredTools() const override { return {"dotnet"}; }
    bool detect(const fs::path& directory) const override
    {
        return hasExtension(directory, ".csproj") || hasExtension(directory, ".sln") || hasExtension(directory, ".slnx") || hasExtension(directory, ".fsproj");
    }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (auto status = requireTool(tools, "dotnet"); !status)
            return status.error();
        BuildPlan plan;
        std::string config = request.configuration.kind == "release" ? "Release" : "Debug";
        std::vector<std::string> arguments = {"build", "-c", config, "--nologo", "-consoleLoggerParameters:NoSummary"};
        for (const auto& flag : request.configuration.flags)
            arguments.push_back(flag);
        plan.steps.push_back({"Build", baseProcess(request, "dotnet", arguments)});
        plan.artifactDirectories = {"bin/" + config};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector&) const override
    {
        std::string config = request.configuration.kind == "release" ? "Release" : "Debug";
        return baseProcess(request, "dotnet", {"run", "-c", config, "--no-build"});
    }
};

class SwiftPackageProvider final : public IBuildProvider {
public:
    std::string id() const override { return "swiftpm"; }
    std::string label() const override { return "Swift Package Manager"; }
    std::vector<std::string> requiredTools() const override { return {"swift"}; }
    bool detect(const fs::path& directory) const override { return hasFile(directory, "Package.swift") && !hasExtension(directory, ".xcodeproj"); }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (auto status = requireTool(tools, "swift"); !status)
            return status.error();
        BuildPlan plan;
        std::vector<std::string> arguments = {"build", "-c", request.configuration.kind == "release" ? "release" : "debug"};
        plan.steps.push_back({"Build", baseProcess(request, "swift", arguments)});
        plan.artifactDirectories = {".build"};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector&) const override
    {
        return baseProcess(request, "swift", {"run", "-c", request.configuration.kind == "release" ? "release" : "debug"});
    }
};

class XcodeProvider final : public IBuildProvider {
public:
    std::string id() const override { return "xcode"; }
    std::string label() const override { return "Xcode"; }
    std::vector<std::string> requiredTools() const override { return {"xcode"}; }
    bool detect(const fs::path& directory) const override { return hasExtension(directory, ".xcodeproj") || hasExtension(directory, ".xcworkspace"); }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        std::string reason;
        if (!hostCanBuildFor(request.targetPlatform.empty() ? "macos" : request.targetPlatform, &reason))
            return makeError("toolchain.host", reason);
        if (auto status = requireTool(tools, "xcode"); !status)
            return status.error();
        std::string project = findProject(request.projectDirectory);
        if (project.empty())
            return makeError("build.noproject", "No Xcode project was found");
        std::string destination = destinationFor(request.targetPlatform);
        std::string scheme = fs::path(project).stem().string();
        BuildPlan plan;
        plan.steps.push_back({"Build", baseProcess(request, "xcodebuild",
                                                   {"-project", project, "-scheme", scheme, "-configuration", request.configuration.kind == "release" ? "Release" : "Debug",
                                                    "-destination", destination, "-derivedDataPath", "build/derived", "CODE_SIGNING_ALLOWED=NO", "build"})});
        plan.artifactDirectories = {"build/derived/Build/Products"};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector&) const override
    {
        auto artifacts = collectArtifacts(request.projectDirectory, {"build/derived/Build/Products"});
        for (const auto& artifact : artifacts) {
            if (artifact.kind == "macOS or iOS application" && (request.targetPlatform.empty() || request.targetPlatform == "macos")) {
                fs::path app = request.projectDirectory / paths::fromUtf8(artifact.path);
                return baseProcess(request, paths::toUtf8(app / "Contents" / "MacOS" / app.stem()), {});
            }
        }
        return makeError("run.simulator", "Run this target from a simulator: select a device under Devices, or open the project in Xcode.");
    }

private:
    static std::string findProject(const fs::path& directory)
    {
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".xcodeproj")
                return paths::toUtf8(it->path().filename());
        }
        return {};
    }

    static std::string destinationFor(const std::string& platform)
    {
        if (platform == "ios" || platform == "ipados")
            return "generic/platform=iOS Simulator";
        if (platform == "tvos")
            return "generic/platform=tvOS Simulator";
        if (platform == "watchos")
            return "generic/platform=watchOS Simulator";
        if (platform == "visionos")
            return "generic/platform=visionOS Simulator";
        return "generic/platform=macOS";
    }
};

class NodeProvider final : public IBuildProvider {
public:
    std::string id() const override { return "node"; }
    std::string label() const override { return "Node.js"; }
    std::vector<std::string> requiredTools() const override { return {"node", "npm"}; }
    bool detect(const fs::path& directory) const override { return hasFile(directory, "package.json"); }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        if (auto status = requireTool(tools, "node"); !status)
            return status.error();
        std::string manager = packageManager(request.projectDirectory, tools);
        BuildPlan plan;
        BuildStep install{"Install dependencies", baseProcess(request, manager, {"install"})};
        install.skipIfExists = true;
        install.existsPath = request.projectDirectory / "node_modules";
        plan.steps.push_back(install);
        if (hasScript(request.projectDirectory, "build"))
            plan.steps.push_back({"Build", baseProcess(request, manager, {"run", "build"})});
        plan.artifactDirectories = {"dist", "build"};
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        std::string manager = packageManager(request.projectDirectory, tools);
        for (const char* script : {"dev", "start"}) {
            if (hasScript(request.projectDirectory, script))
                return baseProcess(request, manager, {"run", script});
        }
        return makeError("run.noscript", "package.json has no dev or start script");
    }

private:
    static std::string packageManager(const fs::path& directory, ToolchainDetector& tools)
    {
        if (hasFile(directory, "pnpm-lock.yaml") && tools.isAvailable("pnpm"))
            return "pnpm";
        if (hasFile(directory, "yarn.lock") && tools.isAvailable("yarn"))
            return "yarn";
        if (hasFile(directory, "bun.lock") && tools.isAvailable("bun"))
            return "bun";
        return "npm";
    }

    static bool hasScript(const fs::path& directory, const char* name)
    {
        auto content = paths::readWholeFile(directory / "package.json");
        if (!content)
            return false;
        auto parsed = parseJson(*content);
        if (!parsed)
            return false;
        auto scripts = parsed->find("scripts");
        return scripts != parsed->end() && scripts->is_object() && scripts->contains(name);
    }
};

class DataBuildProvider final : public IBuildProvider {
public:
    explicit DataBuildProvider(Json definition) : definition_(std::move(definition)) {}

    std::string id() const override { return jsonString(definition_, "id"); }
    std::string label() const override { return jsonString(definition_, "label", id()); }
    std::vector<std::string> requiredTools() const override { return jsonStrings(definition_, "tools"); }

    bool detect(const fs::path& directory) const override
    {
        for (const auto& marker : jsonStrings(definition_, "markers")) {
            if (text::startsWith(marker, "*.")) {
                if (hasExtension(directory, marker.substr(1).c_str()))
                    return true;
            } else if (hasFile(directory, marker.c_str())) {
                return true;
            }
        }
        return false;
    }

    Result<BuildPlan> plan(const BuildRequest& request, ToolchainDetector& tools) const override
    {
        for (const auto& tool : requiredTools()) {
            if (auto status = requireTool(tools, tool.c_str()); !status)
                return status.error();
        }
        BuildPlan plan;
        std::string key = request.configuration.kind == "release" ? "buildRelease" : "build";
        auto steps = definition_.find(key);
        if (steps == definition_.end())
            steps = definition_.find("build");
        if (steps != definition_.end() && steps->is_array()) {
            for (const auto& step : *steps) {
                auto command = jsonStrings(step, "command");
                if (command.empty())
                    continue;
                std::vector<std::string> arguments(command.begin() + 1, command.end());
                plan.steps.push_back({jsonString(step, "label", "Build"), baseProcess(request, command.front(), arguments)});
            }
        }
        plan.artifactDirectories = jsonStrings(definition_, "artifacts");
        return plan;
    }

    Result<ProcessSpec> runCommand(const BuildRequest& request, ToolchainDetector&) const override
    {
        auto command = jsonStrings(definition_, "run");
        if (command.empty())
            return makeError("run.nocommand", label() + " has no run command");
        std::vector<std::string> arguments(command.begin() + 1, command.end());
        return baseProcess(request, command.front(), arguments);
    }

private:
    Json definition_;
};

}

std::unique_ptr<IBuildProvider> createDataBuildProvider(const Json& definition)
{
    return std::make_unique<DataBuildProvider>(definition);
}

void registerBuiltinBuildProviders(BuildSystemRegistry& registry)
{
    registry.add(std::make_unique<XcodeProvider>());
    registry.add(std::make_unique<CMakeProvider>());
    registry.add(std::make_unique<GradleProvider>());
    registry.add(std::make_unique<DotnetProvider>());
    registry.add(std::make_unique<SwiftPackageProvider>());
    registry.add(std::make_unique<NodeProvider>());

    const Json definitions = Json::parse(R"JSON([
      {"id":"flutter","label":"Flutter","markers":["pubspec.yaml"],"tools":["flutter"],
       "build":[{"label":"Get packages","command":["flutter","pub","get"]},{"label":"Analyze","command":["flutter","analyze","--no-fatal-infos"]}],
       "buildRelease":[{"label":"Get packages","command":["flutter","pub","get"]},{"label":"Analyze","command":["flutter","analyze","--no-fatal-infos"]}],
       "run":["flutter","run"],"artifacts":["build"]},
      {"id":"cargo","label":"Cargo","markers":["Cargo.toml"],"tools":["cargo"],
       "build":[{"label":"Build","command":["cargo","build"]}],"buildRelease":[{"label":"Build","command":["cargo","build","--release"]}],
       "run":["cargo","run"],"artifacts":["target/debug","target/release"]},
      {"id":"go","label":"Go","markers":["go.mod"],"tools":["go"],
       "build":[{"label":"Build","command":["go","build","./..."]}],"run":["go","run","."],"artifacts":["."]},
      {"id":"maven","label":"Maven","markers":["pom.xml"],"tools":["maven","jdk"],
       "build":[{"label":"Package","command":["mvn","-q","package"]}],"run":["mvn","-q","exec:java"],"artifacts":["target"]},
      {"id":"meson","label":"Meson","markers":["meson.build"],"tools":["meson"],
       "build":[{"label":"Setup","command":["meson","setup","--reconfigure","build"]},{"label":"Compile","command":["meson","compile","-C","build"]}],"artifacts":["build"]},
      {"id":"make","label":"Make","markers":["Makefile","makefile","GNUmakefile"],"tools":["make"],
       "build":[{"label":"Make","command":["make"]}],"run":["make","run"],"artifacts":["."]},
      {"id":"zig","label":"Zig","markers":["build.zig"],"tools":["zig"],
       "build":[{"label":"Build","command":["zig","build"]}],"run":["zig","build","run"],"artifacts":["zig-out"]},
      {"id":"mix","label":"Mix","markers":["mix.exs"],"tools":["elixir"],
       "build":[{"label":"Compile","command":["mix","compile"]}],"run":["mix","run"],"artifacts":["_build"]},
      {"id":"cabal","label":"Cabal","markers":["*.cabal","cabal.project"],"tools":["ghc"],
       "build":[{"label":"Build","command":["cabal","build"]}],"run":["cabal","run"],"artifacts":["dist-newstyle"]},
      {"id":"python","label":"Python","markers":["pyproject.toml","requirements.txt","setup.py"],"tools":["python"],
       "build":[{"label":"Check syntax","command":["python","-m","compileall","-q","."]}],"run":["python","main.py"],"artifacts":[]},
      {"id":"static","label":"Static site","markers":["index.html"],"tools":["python"],
       "build":[],"run":["python","-u","-m","http.server","5173","--bind","127.0.0.1"],"artifacts":["."]},
      {"id":"bundler","label":"Bundler","markers":["Gemfile"],"tools":["ruby"],
       "build":[{"label":"Install gems","command":["bundle","install"]}],"run":["bundle","exec","ruby","main.rb"],"artifacts":[]},
      {"id":"composer","label":"Composer","markers":["composer.json"],"tools":["php"],
       "build":[{"label":"Install packages","command":["composer","install"]}],"run":["php","-S","127.0.0.1:8080"],"artifacts":["vendor"]},
      {"id":"nimble","label":"Nimble","markers":["*.nimble"],"tools":["nim"],
       "build":[{"label":"Build","command":["nimble","build"]}],"run":["nimble","run"],"artifacts":["."]},
      {"id":"dart","label":"Dart","markers":["analysis_options.yaml"],"tools":["dart"],
       "build":[{"label":"Analyze","command":["dart","analyze"]}],"run":["dart","run"],"artifacts":["build"]},
      {"id":"sbt","label":"sbt","markers":["build.sbt"],"tools":["scala","jdk"],
       "build":[{"label":"Compile","command":["sbt","compile"]}],"run":["sbt","run"],"artifacts":["target"]}
    ])JSON");
    for (const auto& definition : definitions)
        registry.add(createDataBuildProvider(definition));
}

}
