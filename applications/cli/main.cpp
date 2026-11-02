#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "forma/build/build_system.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "forma/ir/validation.h"
#include "forma/project/project.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

using namespace forma;

namespace {

void usage()
{
    std::cout << "forma: work with FormaIDE projects from a terminal\n\n"
                 "  forma new <name> [--template <id>] [--dir <path>] [--bundle-id <id>] [--git]\n"
                 "  forma templates\n"
                 "  forma generators\n"
                 "  forma tools [--json]\n"
                 "  forma validate [dir]\n"
                 "  forma generate [dir] [--dry-run]\n"
                 "  forma sync [dir]\n"
                 "  forma build [dir] [--release]\n"
                 "  forma run [dir] [--release]\n"
                 "  forma import <dir>\n"
                 "  forma history [dir]\n";
}

std::string optionValue(std::vector<std::string>& arguments, const std::string& name, const std::string& fallback = {})
{
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == name) {
            std::string value = arguments[i + 1];
            arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(i), arguments.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            return value;
        }
    }
    return fallback;
}

bool flag(std::vector<std::string>& arguments, const std::string& name)
{
    auto it = std::find(arguments.begin(), arguments.end(), name);
    if (it == arguments.end())
        return false;
    arguments.erase(it);
    return true;
}

std::filesystem::path projectArgument(const std::vector<std::string>& arguments)
{
    if (!arguments.empty())
        return paths::fromUtf8(arguments[0]);
    return std::filesystem::current_path();
}

int fail(const Error& error)
{
    std::cerr << "error: " << error.message << "\n";
    return 1;
}

void printDiagnostics(const std::vector<ir::Diagnostic>& diagnostics)
{
    for (const auto& item : diagnostics) {
        std::cout << ir::severityName(item.severity) << ": " << item.message;
        if (!item.screen.empty())
            std::cout << " (" << item.screen << (item.component.empty() ? "" : "/" + item.component) << ")";
        std::cout << " [" << item.code << "]\n";
    }
}

int commandNew(std::vector<std::string> arguments, const project::TemplateRegistry& templates, const codegen::GeneratorRegistry& generators)
{
    project::NewProjectOptions options;
    options.templateId = optionValue(arguments, "--template", "blank");
    options.bundleId = optionValue(arguments, "--bundle-id");
    std::string directory = optionValue(arguments, "--dir");
    options.generatorOverride = optionValue(arguments, "--generator");
    options.initGit = flag(arguments, "--git");
    if (arguments.empty()) {
        usage();
        return 2;
    }
    options.name = arguments[0];
    options.location = directory.empty() ? std::filesystem::current_path() / paths::fromUtf8(options.name) : paths::fromUtf8(directory);
    auto session = project::ProjectSession::create(options, templates, generators);
    if (!session)
        return fail(session.error());
    std::cout << "Created " << options.name << " at " << paths::toUtf8((*session)->root()) << "\n";
    return 0;
}

int commandValidate(const std::vector<std::string>& arguments)
{
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    auto diagnostics = ir::validateModel((*session)->model());
    printDiagnostics(diagnostics);
    std::cout << diagnostics.size() << " finding(s)\n";
    return ir::hasErrors(diagnostics) ? 1 : 0;
}

int commandGenerate(std::vector<std::string> arguments, const codegen::GeneratorRegistry& generators)
{
    bool dryRun = flag(arguments, "--dry-run");
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    auto report = (*session)->generate(generators, {}, dryRun);
    printDiagnostics(report.diagnostics);
    if (report.blockedByErrors) {
        std::cerr << "Generation stopped because of the errors above.\n";
        return 1;
    }
    for (const auto& file : report.files) {
        if (file.outcome == codegen::FileOutcome::Unchanged || file.outcome == codegen::FileOutcome::KeptUserFile)
            continue;
        std::cout << codegen::fileOutcomeName(file.outcome) << ": " << file.path;
        if (!file.detail.empty())
            std::cout << " (" << file.detail << ")";
        std::cout << "\n";
    }
    std::cout << report.files.size() << " file(s) checked, " << report.count(codegen::FileOutcome::Conflict) << " conflict(s)\n";
    return report.hasConflicts() ? 3 : 0;
}

int commandSync(const std::vector<std::string>& arguments)
{
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    auto analysis = (*session)->reconcileFromCode();
    if (!analysis)
        return fail(analysis.error());
    for (const auto& update : analysis->updates)
        std::cout << "design updated: " << update.screen << "/" << update.component << " " << update.key << " = " << dumpJsonCompact(update.value) << "\n";
    for (const auto& edit : analysis->unmapped)
        std::cout << "kept as code: " << edit.file << ":" << edit.startLine << (edit.component.empty() ? "" : " in " + edit.component) << "\n";
    for (const auto& conflict : analysis->conflicts)
        std::cout << "changed on both sides: " << conflict.component << " " << conflict.key << "\n";
    if (auto status = (*session)->saveDesign(); !status)
        return fail(status.error());
    return 0;
}

build::BuildRequest requestFor(project::ProjectSession& session, const project::TargetConfig& target, bool release)
{
    build::BuildRequest request;
    request.projectDirectory = session.targetDirectory(target);
    request.configuration.id = release ? "release" : "debug";
    request.configuration.kind = release ? "release" : "debug";
    if (!target.platforms.empty()) {
        request.targetPlatform = target.platforms.front();
        for (const auto& platform : target.platforms) {
            if (platform == build::hostPlatform())
                request.targetPlatform = platform;
        }
    }
    return request;
}

int commandBuild(std::vector<std::string> arguments, build::BuildSystemRegistry& builders, build::ToolchainDetector& tools)
{
    bool release = flag(arguments, "--release");
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    const project::TargetConfig* target = (*session)->descriptor().active();
    if (target == nullptr) {
        std::cerr << "error: the project has no targets\n";
        return 1;
    }
    const build::IBuildProvider* provider = (*session)->buildProviderFor(builders, *target);
    if (provider == nullptr) {
        std::cerr << "error: no build system was recognized in " << paths::toUtf8((*session)->targetDirectory(*target)) << "\n";
        return 1;
    }
    auto request = requestFor(**session, *target, release);
    auto plan = provider->plan(request, tools);
    if (!plan)
        return fail(plan.error());
    build::BuildCallbacks callbacks;
    callbacks.onStep = [](std::string_view label) { std::cout << "== " << label << " ==\n"; };
    callbacks.onOutput = [](std::string_view line, bool isError) { (isError ? std::cerr : std::cout) << line << "\n"; };
    auto result = build::runBuildBlocking(std::move(plan.value()), request.projectDirectory, callbacks);
    int errors = 0;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == build::DiagnosticSeverity::Error)
            ++errors;
    }
    std::cout << "Build " << (result.state == build::BuildState::Succeeded ? "succeeded" : "failed") << " in " << result.durationMillis / 1000.0 << " s, " << errors << " error(s)\n";
    for (const auto& artifact : result.artifacts)
        std::cout << "artifact: " << artifact.path << " (" << artifact.kind << ")\n";
    return result.state == build::BuildState::Succeeded ? 0 : 1;
}

int commandRun(std::vector<std::string> arguments, build::BuildSystemRegistry& builders, build::ToolchainDetector& tools)
{
    bool release = flag(arguments, "--release");
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    const project::TargetConfig* target = (*session)->descriptor().active();
    if (target == nullptr)
        return 1;
    const build::IBuildProvider* provider = (*session)->buildProviderFor(builders, *target);
    if (provider == nullptr) {
        std::cerr << "error: no build system was recognized\n";
        return 1;
    }
    auto request = requestFor(**session, *target, release);
    auto command = provider->runCommand(request, tools);
    if (!command)
        return fail(command.error());
    auto process = Process::start(
        *command, [](std::string_view data) { std::cout.write(data.data(), static_cast<std::streamsize>(data.size())); std::cout.flush(); },
        [](std::string_view data) { std::cerr.write(data.data(), static_cast<std::streamsize>(data.size())); std::cerr.flush(); });
    if (!process)
        return fail(process.error());
    return (*process)->wait();
}

int commandTools(std::vector<std::string> arguments, build::ToolchainDetector& tools)
{
    bool json = flag(arguments, "--json");
    auto detected = tools.detectAll();
    if (json) {
        Json list = Json::array();
        for (const auto& tool : detected)
            list.push_back(build::ToolchainDetector::toJson(tool));
        std::cout << dumpJson(list);
        return 0;
    }
    std::string category;
    for (const auto& tool : detected) {
        if (tool.category != category) {
            category = tool.category;
            std::cout << "\n" << category << "\n";
        }
        std::printf("  %-22s %-10s ", tool.name.c_str(), tool.available ? "found" : "missing");
        if (tool.available)
            std::cout << tool.version << "  " << tool.path;
        else
            std::cout << tool.installHint;
        std::cout << "\n";
    }
    return 0;
}

int commandHistory(const std::vector<std::string>& arguments)
{
    auto session = project::ProjectSession::open(projectArgument(arguments));
    if (!session)
        return fail(session.error());
    auto entries = (*session)->history().list(50);
    if (!entries)
        return fail(entries.error());
    for (const auto& entry : *entries)
        std::cout << entry.time << "  " << entry.kind << "  " << entry.actor << "  " << entry.summary << "\n";
    return 0;
}

}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> arguments;
#if defined(_WIN32)
    int wideCount = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    for (int i = 1; i < wideCount; ++i)
        arguments.push_back(paths::toUtf8(std::filesystem::path(wide[i])));
    LocalFree(wide);
    (void)argc;
    (void)argv;
#else
    for (int i = 1; i < argc; ++i)
        arguments.emplace_back(argv[i]);
#endif
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "help") {
        usage();
        return arguments.empty() ? 2 : 0;
    }
    std::string command = arguments[0];
    arguments.erase(arguments.begin());

    codegen::GeneratorRegistry generators;
    generators::registerBuiltinGenerators(generators);
    project::TemplateRegistry templates;
    build::BuildSystemRegistry builders;
    build::registerBuiltinBuildProviders(builders);
    build::ToolchainDetector tools;

    if (command == "new")
        return commandNew(arguments, templates, generators);
    if (command == "templates") {
        for (const auto& item : templates.all())
            std::printf("%-20s %-40s %s\n", item.id.c_str(), item.name.c_str(), item.generator.empty() ? "" : item.generator.c_str());
        return 0;
    }
    if (command == "generators") {
        for (const auto* generator : generators.all()) {
            auto info = generator->info();
            std::cout << info.id << "  " << info.label << "  " << text::join(info.platforms, ", ") << "\n";
        }
        return 0;
    }
    if (command == "tools")
        return commandTools(arguments, tools);
    if (command == "validate")
        return commandValidate(arguments);
    if (command == "generate")
        return commandGenerate(arguments, generators);
    if (command == "sync")
        return commandSync(arguments);
    if (command == "build")
        return commandBuild(arguments, builders, tools);
    if (command == "run")
        return commandRun(arguments, builders, tools);
    if (command == "history")
        return commandHistory(arguments);
    if (command == "import") {
        if (arguments.empty()) {
            usage();
            return 2;
        }
        auto session = project::ProjectSession::importExisting(paths::fromUtf8(arguments[0]));
        if (!session)
            return fail(session.error());
        std::cout << "Imported " << (*session)->descriptor().name << "\n";
        return 0;
    }
    usage();
    return 2;
}
