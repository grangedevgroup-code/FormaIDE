#include <gtest/gtest.h>

#include <filesystem>
#include <set>

#include "forma/build/setup.h"
#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace forma;
using namespace forma::build;

namespace {

std::filesystem::path scratchDirectory(const char* name)
{
    auto directory = std::filesystem::temp_directory_path() / (std::string("forma-setup-") + name);
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    return directory;
}

}

TEST(Setup, EveryStackToolHasAPlanOrAnExplanation)
{
    ToolchainDetector tools;
    std::set<std::string> seen;
    for (const auto& stack : toolStacksForHost()) {
        EXPECT_FALSE(stack.name.empty());
        for (const auto* list : {&stack.required, &stack.optional}) {
            for (const auto& id : *list) {
                if (!seen.insert(id).second)
                    continue;
                InstallPlan plan = planToolInstall(id, tools);
                EXPECT_TRUE(plan.automatic() || !plan.note.empty() || !plan.manualUrl.empty()) << id;
                EXPECT_FALSE(plan.name.empty()) << id;
            }
        }
    }
    EXPECT_GE(seen.size(), 8u);
}

TEST(Setup, InstallCommandsAreParsedWithoutAShell)
{
    InstallPlan npm = planCommandInstall("Server", "npm install -g pyright");
    ASSERT_EQ(npm.steps.size(), 1u);
    EXPECT_EQ(npm.steps[0].process.program, "npm");
    EXPECT_EQ(npm.steps[0].process.arguments, (std::vector<std::string>{"install", "-g", "pyright"}));
    EXPECT_EQ(npm.dependencies, (std::vector<std::string>{"node"}));

    InstallPlan pip = planCommandInstall("Server", "pip install python-lsp-server");
    ASSERT_EQ(pip.steps.size(), 1u);
    EXPECT_EQ(pip.steps[0].process.program, "python");
    EXPECT_EQ(pip.steps[0].process.arguments.front(), "-m");

    InstallPlan tool = planCommandInstall("Server", "dotnet tool install --global csharp-ls");
    ASSERT_EQ(tool.steps.size(), 1u);
    EXPECT_EQ(tool.steps[0].process.arguments[1], "update");
    EXPECT_EQ(tool.steps[0].process.arguments.back(), "https://api.nuget.org/v3/index.json");

    EXPECT_FALSE(planCommandInstall("Server", "Install it from the website").automatic());
    EXPECT_FALSE(planCommandInstall("Server", "rm -rf /").automatic());
    EXPECT_FALSE(planCommandInstall("Server", "curl https://example.com/x.sh").automatic());
}

TEST(Setup, DependenciesComeBeforeWhatNeedsThem)
{
    ToolchainDetector tools;
    auto order = installOrder({"android-sdk"}, tools);
    ASSERT_FALSE(order.empty());
    EXPECT_EQ(order.back(), "android-sdk");
    if (!tools.isAvailable("jdk")) {
        EXPECT_EQ(order.front(), "jdk");
    }
    auto repeated = installOrder({"android-sdk", "android-sdk"}, tools);
    EXPECT_EQ(repeated, order);
}

TEST(Setup, AJobReportsItsStepsAndStopsAtTheFirstFailure)
{
    InstallPlan plan;
    plan.id = "demo";
    plan.name = "Demo";
    SetupStep first;
    first.label = "Passes";
    bool firstRan = false;
    first.action = [&firstRan]() -> Status {
        firstRan = true;
        return {};
    };
    plan.steps.push_back(first);
    SetupStep failing;
    failing.label = "Fails";
    failing.action = []() -> Status { return makeError("demo.failed", "This step failed"); };
    plan.steps.push_back(failing);
    SetupStep never;
    never.label = "Never";
    bool neverRan = false;
    never.action = [&neverRan]() -> Status {
        neverRan = true;
        return {};
    };
    plan.steps.push_back(never);
    std::vector<std::string> labels;
    bool success = true;
    std::string message;
    SetupCallbacks callbacks;
    callbacks.onStep = [&](std::string_view label) { labels.emplace_back(label); };
    callbacks.onFinished = [&](bool ok, std::string_view text) {
        success = ok;
        message = std::string(text);
    };
    SetupJob job;
    ASSERT_TRUE(job.start(plan, callbacks));
    job.wait();
    EXPECT_TRUE(firstRan);
    EXPECT_FALSE(neverRan);
    EXPECT_FALSE(success);
    EXPECT_EQ(message, "This step failed");
    EXPECT_EQ(labels, (std::vector<std::string>{"Passes", "Fails"}));
}

TEST(Setup, AJobAcceptsTheSuccessCodesAStepAllows)
{
    InstallPlan plan;
    plan.id = "demo";
    plan.name = "Demo";
    SetupStep step;
    step.label = "Exits with 3";
#if defined(_WIN32)
    step.process.program = "cmd";
    step.process.arguments = {"/c", "exit", "3"};
#else
    step.process.program = "sh";
    step.process.arguments = {"-c", "exit 3"};
#endif
    step.successCodes = {0, 3};
    plan.steps.push_back(step);
    bool success = false;
    SetupCallbacks callbacks;
    callbacks.onFinished = [&](bool ok, std::string_view) { success = ok; };
    SetupJob job;
    ASSERT_TRUE(job.start(plan, callbacks));
    job.wait();
    EXPECT_TRUE(success);

    plan.steps[0].successCodes = {0};
    std::string message;
    callbacks.onFinished = [&](bool ok, std::string_view text) {
        success = ok;
        message = std::string(text);
    };
    SetupJob strict;
    ASSERT_TRUE(strict.start(plan, callbacks));
    strict.wait();
    EXPECT_FALSE(success);
    EXPECT_NE(message.find("code 3"), std::string::npos);
}

TEST(Setup, RefreshingTheEnvironmentKeepsThePathAndAddsManagedTools)
{
#if defined(_WIN32)
    const char separator = ';';
    auto key = [](const std::string& entry) { return text::toLower(entry); };
#else
    const char separator = ':';
    auto key = [](const std::string& entry) { return entry; };
#endif
    auto before = paths::env("PATH").value_or("");
    refreshEnvironmentFromSystem();
    auto after = paths::env("PATH").value_or("");
    std::set<std::string> kept;
    for (const auto& entry : text::split(after, separator))
        kept.insert(key(entry));
    for (const auto& entry : text::split(before, separator)) {
        if (!entry.empty())
            EXPECT_TRUE(kept.count(key(entry))) << entry;
    }
    EXPECT_TRUE(kept.count(key(paths::toUtf8(paths::homeDir() / ".cargo" / "bin"))));
}

#if defined(_WIN32)
TEST(Setup, AppExecutionAliasesCountAsPrograms)
{
    auto localAppData = paths::env("LOCALAPPDATA");
    if (!localAppData)
        GTEST_SKIP();
    std::filesystem::path alias = paths::fromUtf8(*localAppData) / "Microsoft" / "WindowsApps" / "winget.exe";
    if (GetFileAttributesW(alias.c_str()) == INVALID_FILE_ATTRIBUTES)
        GTEST_SKIP() << "winget isn't installed here";
    auto found = findExecutable("winget");
    ASSERT_TRUE(found.has_value());
    auto result = runProcess(ProcessSpec{paths::toUtf8(*found), {"--version"}}, std::chrono::seconds(30));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->exitCode, 0);
}
#endif

TEST(Setup, AndroidPackageComesFromTheGradleFile)
{
    auto directory = scratchDirectory("package");
    std::filesystem::create_directories(directory / "app");
    ASSERT_TRUE(paths::writeFileAtomic(directory / "app" / "build.gradle.kts", "android {\n    defaultConfig {\n        applicationId = \"com.example.demo\"\n    }\n}\n"));
    EXPECT_EQ(androidPackage(directory), "com.example.demo");
    auto empty = scratchDirectory("nopackage");
    EXPECT_EQ(androidPackage(empty), "");
}
