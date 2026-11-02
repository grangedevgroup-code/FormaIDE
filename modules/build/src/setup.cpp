#include "forma/build/setup.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <regex>
#include <set>

#include "forma/core/hash.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"
#include "forma/resources/resources.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace forma::build {

namespace {

constexpr const char* kAndroidPlatform = "platforms;android-37.0";
constexpr const char* kAndroidBuildTools = "build-tools;37.0.0";
constexpr const char* kAndroidImage = "system-images;android-36;google_apis;x86_64";
constexpr const char* kAvdName = "Forma_Phone";
constexpr const char* kQtVersion = "6.12.0";

struct Download {
    const char* url;
    const char* sha256;
};

ProcessSpec commandSpec(std::string program, std::vector<std::string> arguments)
{
    ProcessSpec spec;
    spec.program = std::move(program);
    spec.arguments = std::move(arguments);
    spec.origin = Origin::Build;
    spec.mergeStderr = true;
    return spec;
}

SetupStep commandStep(std::string label, std::string program, std::vector<std::string> arguments)
{
    SetupStep step;
    step.label = std::move(label);
    step.process = commandSpec(std::move(program), std::move(arguments));
    return step;
}

std::string hostKey()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

Download androidToolsDownload()
{
#if defined(_WIN32)
    return {"https://dl.google.com/android/repository/commandlinetools-win-16111833_latest.zip", "e5885e2e59038c0778a85fc19f01de7ce7c567c105553f7617b936b1e0e87b2b"};
#elif defined(__APPLE__) && defined(__aarch64__)
    return {"https://dl.google.com/android/repository/commandlinetools-mac_arm64-16111833_latest.zip", "58da8f215781cc0208743e88b65ae259780c55e7eb40e50373d377cf369232d6"};
#elif defined(__APPLE__)
    return {"https://dl.google.com/android/repository/commandlinetools-mac_x86_64-16111833_latest.zip", "7e601c04e7173754b3d051a87bb4c3a8243c65a9d7bfa9356ed6499fb8953fa3"};
#else
    return {"https://dl.google.com/android/repository/commandlinetools-linux-16111833_latest.zip", "0877a1d048fe4a24efe2eff536ca4223f7adeb58648bb81909d33c446918cfa8"};
#endif
}

Download gradleDownload()
{
    return {"https://services.gradle.org/distributions/gradle-9.8.2-bin.zip", "21bd5da2de49b642904077f38c0efdc7778e92b3ed0889392dca61b97e389607"};
}

Download kotlinServerDownload()
{
    return {"https://github.com/fwcd/kotlin-language-server/releases/download/1.3.13/server.zip", "4fe7d71d087b307c7869036171bd9d8c6a4284cd7c25b89098b0a24eb2d9b6d2"};
}

fs::path defaultAndroidSdk()
{
#if defined(_WIN32)
    return paths::fromUtf8(paths::env("LOCALAPPDATA").value_or(paths::toUtf8(paths::homeDir()))) / "Android" / "Sdk";
#elif defined(__APPLE__)
    return paths::homeDir() / "Library" / "Android" / "sdk";
#else
    return paths::homeDir() / "Android" / "Sdk";
#endif
}

std::string batchName(const char* name)
{
#if defined(_WIN32)
    return std::string(name) + ".bat";
#else
    return name;
#endif
}

std::string exeFile(const char* name)
{
#if defined(_WIN32)
    return std::string(name) + ".exe";
#else
    return name;
#endif
}

SetupStep verifyStep(const fs::path& file, std::string expected)
{
    SetupStep step;
    step.label = "Check the download";
    step.action = [file, expected]() -> Status {
        auto content = paths::readWholeFile(file);
        if (!content)
            return makeError("setup.download", "The download is missing: " + paths::toUtf8(file));
        std::string actual = sha256Hex(*content);
        if (actual != expected)
            return makeError("setup.checksum", "The download doesn't match its expected checksum, so it wasn't used. Expected " + expected + ", got " + actual + ".");
        return {};
    };
    return step;
}

std::string systemTool(const char* name)
{
#if defined(_WIN32)
    return paths::toUtf8(paths::fromUtf8(paths::env("SystemRoot").value_or("C:\\Windows")) / "System32" / (std::string(name) + ".exe"));
#else
    return name;
#endif
}

std::string pythonProgram()
{
    if (findExecutable("python"))
        return "python";
    if (findExecutable("python3"))
        return "python3";
    return "python";
}

void addDownload(InstallPlan& plan, const Download& download, const fs::path& archive, const fs::path& destination)
{
    SetupStep prepare;
    prepare.label = "Prepare folders";
    prepare.action = [archive, destination]() -> Status {
        std::error_code ec;
        fs::create_directories(archive.parent_path(), ec);
        fs::create_directories(destination, ec);
        if (ec)
            return makeError("setup.folder", "Cannot create " + paths::toUtf8(destination) + ": " + ec.message());
        return {};
    };
    plan.steps.push_back(std::move(prepare));
    plan.steps.push_back(commandStep("Download", systemTool("curl"), {"-fL", "--retry", "3", "--progress-bar", "-o", paths::toUtf8(archive), download.url}));
    plan.steps.push_back(verifyStep(archive, download.sha256));
#if defined(_WIN32)
    plan.steps.push_back(commandStep("Unpack", systemTool("tar"), {"-xf", paths::toUtf8(archive), "-C", paths::toUtf8(destination)}));
#else
    plan.steps.push_back(commandStep("Unpack", "unzip", {"-qo", paths::toUtf8(archive), "-d", paths::toUtf8(destination)}));
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::optional<std::string> linuxPackageManager(std::vector<std::string>& command)
{
    if (findExecutable("apt-get")) {
        command = {"pkexec", "apt-get", "install", "-y"};
        return "apt";
    }
    if (findExecutable("dnf")) {
        command = {"pkexec", "dnf", "install", "-y"};
        return "dnf";
    }
    if (findExecutable("pacman")) {
        command = {"pkexec", "pacman", "-S", "--noconfirm", "--needed"};
        return "pacman";
    }
    if (findExecutable("zypper")) {
        command = {"pkexec", "zypper", "--non-interactive", "install"};
        return "zypper";
    }
    return std::nullopt;
}
#endif

struct PackageNames {
    const char* winget;
    const char* brew;
    const char* apt;
    const char* dnf;
    const char* pacman;
};

const std::map<std::string, PackageNames>& packages()
{
    static const std::map<std::string, PackageNames> table = {
        {"cmake", {"Kitware.CMake", "cmake", "cmake", "cmake", "cmake"}},
        {"ninja", {"Ninja-build.Ninja", "ninja", "ninja-build", "ninja-build", "ninja"}},
        {"make", {"", "make", "make", "make", "make"}},
        {"meson", {"", "meson", "meson", "meson", "meson"}},
        {"gcc", {"", "gcc", "g++", "gcc-c++", "gcc"}},
        {"clang", {"LLVM.LLVM", "llvm", "clang", "clang", "clang"}},
        {"clangd", {"LLVM.LLVM", "llvm", "clangd", "clang-tools-extra", "clang"}},
        {"lldb-dap", {"LLVM.LLVM", "llvm", "lldb", "lldb", "lldb"}},
        {"gdb", {"", "gdb", "gdb", "gdb", "gdb"}},
        {"jdk", {"Microsoft.OpenJDK.25", "openjdk", "default-jdk", "java-latest-openjdk-devel", "jdk-openjdk"}},
        {"maven", {"Apache.Maven", "maven", "maven", "maven", "maven"}},
        {"kotlin", {"JetBrains.Kotlin.Compiler", "kotlin", "", "", "kotlin"}},
        {"dotnet", {"Microsoft.DotNet.SDK.10", "dotnet", "dotnet-sdk-10.0", "dotnet-sdk-10.0", "dotnet-sdk"}},
        {"node", {"OpenJS.NodeJS.LTS", "node", "nodejs", "nodejs", "nodejs"}},
        {"npm", {"OpenJS.NodeJS.LTS", "node", "npm", "npm", "npm"}},
        {"deno", {"DenoLand.Deno", "deno", "", "", "deno"}},
        {"python", {"Python.Python.3.14", "python", "python3", "python3", "python"}},
        {"go", {"GoLang.Go", "go", "golang-go", "golang", "go"}},
        {"cargo", {"Rustlang.Rustup", "rustup", "rustup", "rustup", "rustup"}},
        {"ruby", {"RubyInstallerTeam.Ruby.3.4", "ruby", "ruby-full", "ruby", "ruby"}},
        {"php", {"PHP.PHP.8.4", "php", "php-cli", "php-cli", "php"}},
        {"lua", {"DEVCOM.Lua", "lua", "lua5.4", "lua", "lua"}},
        {"perl", {"StrawberryPerl.StrawberryPerl", "perl", "perl", "perl", "perl"}},
        {"zig", {"zig.zig", "zig", "", "zig", "zig"}},
        {"git", {"Git.Git", "git", "git", "git", "git"}},
        {"swift", {"Swift.Toolchain", "", "", "", ""}},
    };
    return table;
}

void addWinget(InstallPlan& plan, const std::string& packageId, const std::vector<std::string>& extra = {})
{
    std::vector<std::string> arguments = {"install", "--id", packageId, "--exact", "--silent", "--accept-source-agreements", "--accept-package-agreements", "--disable-interactivity"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    SetupStep step = commandStep("Install " + plan.name, "winget", std::move(arguments));
    step.successCodes = {0, 3010, -1978335189, -1978335135};
    plan.steps.push_back(std::move(step));
    plan.mayAskForAdmin = true;
}

fs::path androidSdkFor(ToolchainDetector& tools)
{
    auto sdk = tools.androidSdk();
    return sdk ? *sdk : defaultAndroidSdk();
}

fs::path sdkManager(const fs::path& sdk)
{
    return sdk / "cmdline-tools" / "latest" / "bin" / batchName("sdkmanager");
}

std::map<std::string, std::string> androidEnvironment(ToolchainDetector& tools, const fs::path& sdk)
{
    std::map<std::string, std::string> environment;
    environment["ANDROID_HOME"] = paths::toUtf8(sdk);
    environment["ANDROID_SDK_ROOT"] = paths::toUtf8(sdk);
    if (auto java = tools.javaHome())
        environment["JAVA_HOME"] = paths::toUtf8(*java);
    return environment;
}

SetupStep sdkManagerStep(ToolchainDetector& tools, const fs::path& sdk, std::string label, std::vector<std::string> packageList)
{
    packageList.insert(packageList.begin(), "--sdk_root=" + paths::toUtf8(sdk));
    SetupStep step = commandStep(std::move(label), paths::toUtf8(sdkManager(sdk)), std::move(packageList));
    step.process.environment = androidEnvironment(tools, sdk);
    return step;
}

SetupStep sdkLicensesStep(ToolchainDetector& tools, const fs::path& sdk)
{
    SetupStep step = sdkManagerStep(tools, sdk, "Accept the Android SDK licenses", {"--licenses"});
    std::string answers;
    for (int i = 0; i < 40; ++i)
        answers += "y\n";
    step.input = answers;
    return step;
}

void addAndroidCommandLineTools(InstallPlan& plan, ToolchainDetector& tools, const fs::path& sdk)
{
    std::error_code ec;
    if (fs::exists(sdkManager(sdk), ec))
        return;
    fs::path archive = paths::userCacheDir() / "downloads" / "android-commandline-tools.zip";
    fs::path unpack = sdk / "cmdline-tools" / ".unpack";
    addDownload(plan, androidToolsDownload(), archive, unpack);
    SetupStep place;
    place.label = "Put the Android tools in place";
    place.action = [unpack, sdk]() -> Status {
        std::error_code inner;
        fs::path target = sdk / "cmdline-tools" / "latest";
        fs::remove_all(target, inner);
        fs::rename(unpack / "cmdline-tools", target, inner);
        if (inner)
            return makeError("setup.folder", "Cannot move the Android tools into place: " + inner.message());
        fs::remove_all(unpack, inner);
        return {};
    };
    plan.steps.push_back(std::move(place));
    (void)tools;
}

void planAndroidSdk(InstallPlan& plan, ToolchainDetector& tools)
{
    fs::path sdk = androidSdkFor(tools);
    plan.dependencies = {"jdk"};
    plan.note = "Installs to " + paths::toUtf8(sdk) + ". This downloads about 1 GB.";
    addAndroidCommandLineTools(plan, tools, sdk);
    plan.steps.push_back(sdkLicensesStep(tools, sdk));
    plan.steps.back().successCodes = {0, 1};
    plan.steps.push_back(sdkManagerStep(tools, sdk, "Install the SDK platform and build tools", {"platform-tools", kAndroidPlatform, kAndroidBuildTools}));
}

void planEmulator(InstallPlan& plan, ToolchainDetector& tools)
{
    fs::path sdk = androidSdkFor(tools);
    plan.dependencies = {"jdk", "android-sdk"};
    plan.note = "Downloads the emulator and a phone system image, about 3 GB, and creates a phone called " + std::string(kAvdName) + ".";
    addAndroidCommandLineTools(plan, tools, sdk);
    plan.steps.push_back(sdkLicensesStep(tools, sdk));
    plan.steps.back().successCodes = {0, 1};
    plan.steps.push_back(sdkManagerStep(tools, sdk, "Install the emulator and a phone image", {"emulator", kAndroidImage}));
    SetupStep create = commandStep("Create a virtual phone", paths::toUtf8(sdk / "cmdline-tools" / "latest" / "bin" / batchName("avdmanager")),
                                   {"create", "avd", "--force", "--name", kAvdName, "--package", kAndroidImage, "--device", "pixel_7"});
    create.process.environment = androidEnvironment(tools, sdk);
    create.input = "no\n";
    plan.steps.push_back(std::move(create));
}

void planAcceleration(InstallPlan& plan, ToolchainDetector& tools)
{
#if defined(_WIN32)
    fs::path sdk = androidSdkFor(tools);
    plan.dependencies = {"jdk", "android-sdk"};
    plan.note = "Installs Google's Android Emulator hypervisor driver so virtual phones run at a usable speed. Windows asks for permission. It needs virtualization turned on in your PC's firmware.";
    plan.mayAskForAdmin = true;
    addAndroidCommandLineTools(plan, tools, sdk);
    plan.steps.push_back(sdkManagerStep(tools, sdk, "Download the hypervisor driver", {"extras;google;Android_Emulator_Hypervisor_Driver"}));
    fs::path script = sdk / "extras" / "google" / "Android_Emulator_Hypervisor_Driver" / "silent_install.bat";
    SetupStep driver = commandStep("Install the hypervisor driver", "powershell",
                                   {"-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference = 'Stop'; $p = Start-Process -FilePath '" + paths::toUtf8(script) + "' -WorkingDirectory '" + paths::toUtf8(script.parent_path()) + "' -Verb RunAs -Wait -PassThru; exit $p.ExitCode"});
    plan.steps.push_back(std::move(driver));
#elif defined(__APPLE__)
    (void)tools;
    plan.note = "macOS already has what the emulator needs.";
#else
    (void)tools;
    plan.note = "Virtual phones use KVM. Turn on virtualization in your firmware, and add yourself to the kvm group.";
#endif
}

void planQt(InstallPlan& plan)
{
    plan.dependencies = {"python"};
    fs::path destination = paths::homeDir() / "Qt" / kQtVersion;
    fs::path script = paths::userCacheDir() / "setup" / "fetch_qt.py";
    plan.note = "Installs Qt " + std::string(kQtVersion) + " to " + paths::toUtf8(destination) + " from download.qt.io and checks every archive. Qt is open source under the LGPL. This downloads about 1 GB.";
    SetupStep prepare;
    prepare.label = "Prepare the Qt installer";
    prepare.action = [script]() -> Status {
        auto data = resources::find("setup/fetch_qt.py");
        if (!data)
            return makeError("setup.resource", "The Qt installer script is missing from this build of FormaIDE");
        std::error_code ec;
        fs::create_directories(script.parent_path(), ec);
        return paths::writeFileAtomic(script, std::string(data->data(), data->size()));
    };
    plan.steps.push_back(std::move(prepare));
    plan.steps.push_back(commandStep("Get the archive reader", pythonProgram(), {"-m", "pip", "install", "--user", "--upgrade", "py7zr"}));
    plan.steps.push_back(commandStep("Install Qt", pythonProgram(), {paths::toUtf8(script), paths::toUtf8(destination), kQtVersion}));
}

void planMsvc(InstallPlan& plan)
{
#if defined(_WIN32)
    plan.note = "Installs the Visual Studio Build Tools with the C++ compiler and Windows SDK. This downloads several GB and Windows asks for permission.";
    addWinget(plan, "Microsoft.VisualStudio.BuildTools", {"--override", "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"});
#else
    (void)plan;
#endif
}

void planGradle(InstallPlan& plan)
{
    fs::path destination = managedToolsDir();
    fs::path archive = paths::userCacheDir() / "downloads" / "gradle-9.8.2-bin.zip";
    addDownload(plan, gradleDownload(), archive, destination);
    plan.note = "Installs Gradle to " + paths::toUtf8(destination) + ".";
}

void planKotlinServer(InstallPlan& plan)
{
    plan.dependencies = {"jdk"};
    fs::path destination = managedToolsDir() / "kotlin-language-server-1.3.13";
    fs::path archive = paths::userCacheDir() / "downloads" / "kotlin-language-server.zip";
    addDownload(plan, kotlinServerDownload(), archive, destination);
    plan.note = "Installs the Kotlin language server to " + paths::toUtf8(destination) + ".";
}

}

std::string InstallPlan::preview() const
{
    std::string output;
    for (const auto& step : steps) {
        if (step.process.program.empty())
            continue;
        std::string line = step.process.program;
        for (const auto& argument : step.process.arguments)
            line += argument.find(' ') != std::string::npos ? " \"" + argument + "\"" : " " + argument;
        output += line + "\n";
    }
    return output;
}

const std::vector<ToolStack>& toolStacks()
{
    static const std::vector<ToolStack> stacks = {
        {"android", "Android apps", "Jetpack Compose apps that run on a phone or emulator", {"jdk", "android-sdk"}, {"emulator", "android-acceleration", "kotlin-language-server"}, {"windows", "macos", "linux"}},
        {"qt", "Qt desktop apps", "Native C++ apps for Windows, macOS and Linux", {"cmake", "ninja", "cpp-compiler", "qt"}, {"clangd", "lldb-dap"}, {"windows", "macos", "linux"}},
        {"windows", "Windows apps", "WPF apps in C#", {"dotnet"}, {"csharp-ls"}, {"windows"}},
        {"apple", "Apple apps", "SwiftUI apps for the Mac", {"swift"}, {}, {"macos"}},
        {"web", "Web apps", "React and static sites", {"node"}, {"typescript-language-server", "html-language-server"}, {"windows", "macos", "linux"}},
        {"python", "Python", "Scripts and desktop apps in Python", {"python"}, {"pyright"}, {"windows", "macos", "linux"}},
        {"rust", "Rust", "Rust programs with Cargo", {"cargo"}, {"rust-analyzer"}, {"windows", "macos", "linux"}},
        {"go", "Go", "Go programs", {"go"}, {"gopls"}, {"windows", "macos", "linux"}},
        {"cpp", "C and C++", "Libraries and programs built with CMake", {"cmake", "ninja", "cpp-compiler"}, {"clangd", "lldb-dap"}, {"windows", "macos", "linux"}},
        {"version-control", "Git", "Version control", {"git"}, {}, {"windows", "macos", "linux"}},
    };
    return stacks;
}

std::vector<ToolStack> toolStacksForHost()
{
    std::vector<ToolStack> output;
    for (const auto& stack : toolStacks()) {
        if (std::find(stack.platforms.begin(), stack.platforms.end(), hostKey()) != stack.platforms.end())
            output.push_back(stack);
    }
    return output;
}

const ToolStack* findToolStack(std::string_view id)
{
    for (const auto& stack : toolStacks()) {
        if (stack.id == id)
            return &stack;
    }
    return nullptr;
}

InstallPlan planCommandInstall(std::string_view name, std::string_view command)
{
    InstallPlan plan;
    plan.id = std::string(name);
    plan.name = std::string(name);
    std::vector<std::string> parts;
    for (const auto& part : text::split(command, ' ')) {
        if (!part.empty())
            parts.push_back(part);
    }
    static const std::map<std::string, std::string> runners = {{"npm", "node"}, {"pip", "python"}, {"go", "go"}, {"rustup", "cargo"}, {"cargo", "cargo"}, {"dotnet", "dotnet"}, {"gem", "ruby"}, {"winget", ""}, {"brew", ""}};
    if (parts.size() < 2)
        return plan;
    auto runner = runners.find(parts.front());
    if (runner == runners.end())
        return plan;
    if (!runner->second.empty())
        plan.dependencies.push_back(runner->second);
    std::string program = parts.front();
    std::vector<std::string> arguments(parts.begin() + 1, parts.end());
    if (program == "dotnet" && arguments.size() >= 2 && arguments[0] == "tool" && arguments[1] == "install") {
        arguments[1] = "update";
        arguments.emplace_back("--add-source");
        arguments.emplace_back("https://api.nuget.org/v3/index.json");
    }
    if (program == "pip") {
        program = pythonProgram();
        arguments.insert(arguments.begin(), {"-m", "pip"});
    }
    SetupStep step = commandStep("Install " + plan.name, std::move(program), std::move(arguments));
    plan.steps.push_back(std::move(step));
    return plan;
}

InstallPlan planServerInstall(std::string_view languageName, std::string_view serverCommand, std::string_view installHint)
{
    std::string name = std::string(languageName) + " language server";
    ToolchainDetector detector;
    if (serverCommand == "clangd") {
        InstallPlan plan = planToolInstall("clangd", detector);
        plan.name = name;
        return plan;
    }
    if (serverCommand == "kotlin-language-server") {
        InstallPlan plan = planToolInstall("kotlin-language-server", detector);
        plan.name = name;
        return plan;
    }
    if (serverCommand == "lua-language-server" && hostKey() == "windows") {
        InstallPlan plan;
        plan.id = "lua-language-server";
        plan.name = name;
        addWinget(plan, "LuaLS.lua-language-server");
        return plan;
    }
    InstallPlan plan = planCommandInstall(name, installHint);
    plan.id = std::string(serverCommand);
    if (!plan.automatic())
        plan.note = std::string(installHint);
    return plan;
}

InstallPlan planToolInstall(std::string_view toolId, ToolchainDetector& tools)
{
    InstallPlan plan;
    plan.id = std::string(toolId);
    ToolInfo info = tools.detect(toolId);
    plan.name = info.name.empty() ? std::string(toolId) : info.name;
    std::string id(toolId);
    if (id == "cpp-compiler") {
#if defined(_WIN32)
        id = "msvc";
#elif defined(__APPLE__)
        plan.name = "Xcode command line tools";
        plan.steps.push_back(commandStep("Install the command line tools", "xcode-select", {"--install"}));
        return plan;
#else
        id = "gcc";
#endif
        plan.name = id == "msvc" ? "C++ compiler (MSVC)" : "C++ compiler (GCC)";
    }
    if (id == "msvc") {
        planMsvc(plan);
        return plan;
    }
    if (id == "android-sdk") {
        plan.name = "Android SDK";
        planAndroidSdk(plan, tools);
        return plan;
    }
    if (id == "emulator" || id == "adb" || id == "android-avd") {
        if (id == "adb") {
            plan.name = "Android SDK";
            planAndroidSdk(plan, tools);
            return plan;
        }
        plan.name = "Android Emulator";
        planEmulator(plan, tools);
        return plan;
    }
    if (id == "android-acceleration") {
        plan.name = "Emulator acceleration";
        planAcceleration(plan, tools);
        return plan;
    }
    if (id == "android-ndk") {
        fs::path sdk = androidSdkFor(tools);
        plan.dependencies = {"jdk", "android-sdk"};
        addAndroidCommandLineTools(plan, tools, sdk);
        plan.steps.push_back(sdkManagerStep(tools, sdk, "Install the Android NDK", {"ndk-bundle"}));
        return plan;
    }
    if (id == "qt") {
        plan.name = "Qt";
        planQt(plan);
        return plan;
    }
    if (id == "gradle") {
        plan.name = "Gradle";
        planGradle(plan);
        return plan;
    }
    if (id == "kotlin-language-server") {
        plan.name = "Kotlin language server";
        planKotlinServer(plan);
        return plan;
    }
    if (id == "pyright") {
        plan = planCommandInstall("Python language server", "npm install -g pyright");
        plan.id = id;
        return plan;
    }
    if (id == "typescript-language-server") {
        plan = planCommandInstall("TypeScript language server", "npm install -g typescript typescript-language-server");
        plan.id = id;
        return plan;
    }
    if (id == "html-language-server") {
        plan = planCommandInstall("HTML and CSS language servers", "npm install -g vscode-langservers-extracted");
        plan.id = id;
        return plan;
    }
    if (id == "csharp-ls") {
        plan = planCommandInstall("C# language server", "dotnet tool install --global csharp-ls");
        plan.id = id;
        return plan;
    }
    if (id == "gopls") {
        plan = planCommandInstall("Go language server", "go install golang.org/x/tools/gopls@latest");
        plan.id = id;
        return plan;
    }
    if (id == "rust-analyzer") {
        plan = planCommandInstall("Rust language server", "rustup component add rust-analyzer");
        plan.id = id;
        return plan;
    }
    if (id == "xcode") {
        plan.manualUrl = "https://apps.apple.com/app/xcode/id497799835";
        plan.note = "Xcode comes from the App Store.";
        return plan;
    }
    auto entry = packages().find(id);
    if (entry == packages().end()) {
        plan.note = info.installHint;
        return plan;
    }
    const PackageNames& names = entry->second;
#if defined(_WIN32)
    if (names.winget[0] != '\0') {
        addWinget(plan, names.winget);
        return plan;
    }
    plan.note = info.installHint;
    return plan;
#elif defined(__APPLE__)
    if (names.brew[0] != '\0' && findExecutable("brew")) {
        plan.steps.push_back(commandStep("Install " + plan.name, "brew", {"install", names.brew}));
        return plan;
    }
    plan.note = names.brew[0] != '\0' ? "Install Homebrew from brew.sh first, then FormaIDE can install this for you." : info.installHint;
    plan.manualUrl = "https://brew.sh";
    return plan;
#else
    std::vector<std::string> command;
    auto manager = linuxPackageManager(command);
    if (manager) {
        const char* package = *manager == "apt" ? names.apt : *manager == "dnf" ? names.dnf : *manager == "pacman" ? names.pacman : names.apt;
        if (package[0] != '\0') {
            std::string program = command.front();
            std::vector<std::string> arguments(command.begin() + 1, command.end());
            arguments.push_back(package);
            plan.steps.push_back(commandStep("Install " + plan.name, std::move(program), std::move(arguments)));
            plan.mayAskForAdmin = true;
            return plan;
        }
    }
    plan.note = info.installHint;
    return plan;
#endif
}

std::vector<std::string> installOrder(const std::vector<std::string>& toolIds, ToolchainDetector& tools)
{
    std::vector<std::string> output;
    std::set<std::string> seen;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (!seen.insert(id).second)
            return;
        InstallPlan plan = planToolInstall(id, tools);
        for (const auto& dependency : plan.dependencies) {
            if (!tools.isAvailable(dependency))
                visit(dependency);
        }
        output.push_back(id);
    };
    for (const auto& id : toolIds)
        visit(id);
    return output;
}

fs::path managedToolsDir()
{
    return paths::userDataDir() / "tools";
}

namespace {

void addToPath(std::vector<std::string>& entries, const std::string& entry)
{
    if (entry.empty())
        return;
    auto same = [&](const std::string& existing) {
#if defined(_WIN32)
        return text::toLower(existing) == text::toLower(entry);
#else
        return existing == entry;
#endif
    };
    if (std::find_if(entries.begin(), entries.end(), same) == entries.end())
        entries.push_back(entry);
}

#if defined(_WIN32)
std::string registryValue(HKEY root, const wchar_t* key, const wchar_t* name)
{
    DWORD size = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, raw.data(), &size) != ERROR_SUCCESS)
        return {};
    raw.resize(wcsnlen(raw.c_str(), raw.size()));
    DWORD needed = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    std::wstring expanded(needed, L'\0');
    ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), needed);
    expanded.resize(wcsnlen(expanded.c_str(), expanded.size()));
    return paths::toUtf8(fs::path(expanded));
}
#endif

}

void refreshEnvironmentFromSystem()
{
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> entries;
    for (const auto& item : text::split(paths::env("PATH").value_or(""), separator))
        addToPath(entries, item);
#if defined(_WIN32)
    for (const auto& item : text::split(registryValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Path"), separator))
        addToPath(entries, item);
    for (const auto& item : text::split(registryValue(HKEY_CURRENT_USER, L"Environment", L"Path"), separator))
        addToPath(entries, item);
#endif
    fs::path home = paths::homeDir();
    for (const fs::path& extra : {home / ".cargo" / "bin", home / "go" / "bin", home / ".dotnet" / "tools", home / ".local" / "bin"})
        addToPath(entries, paths::toUtf8(extra));
#if defined(_WIN32)
    if (auto appData = paths::env("APPDATA"))
        addToPath(entries, paths::toUtf8(paths::fromUtf8(*appData) / "npm"));
#endif
    std::error_code ec;
    for (fs::directory_iterator it(managedToolsDir(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code inner;
        for (const fs::path& candidate : {it->path() / "bin", it->path() / "server" / "bin"}) {
            if (fs::is_directory(candidate, inner))
                addToPath(entries, paths::toUtf8(candidate));
        }
    }
    std::string joined;
    for (const auto& entry : entries) {
        if (!joined.empty())
            joined += separator;
        joined += entry;
    }
#if defined(_WIN32)
    _wputenv_s(L"PATH", paths::fromUtf8(joined).wstring().c_str());
#else
    setenv("PATH", joined.c_str(), 1);
#endif
}

SetupJob::SetupJob() = default;

SetupJob::~SetupJob()
{
    cancel();
    wait();
}

Status SetupJob::start(InstallPlan plan, SetupCallbacks callbacks)
{
    if (running_.load())
        return makeError("setup.busy", "An install is already running");
    wait();
    running_ = true;
    cancelled_ = false;
    worker_ = std::thread([this, plan = std::move(plan), callbacks = std::move(callbacks)]() mutable {
        std::string failure;
        for (auto& step : plan.steps) {
            if (cancelled_.load()) {
                failure = "Cancelled";
                break;
            }
            if (callbacks.onStep)
                callbacks.onStep(step.label);
            if (step.action) {
                auto status = step.action();
                if (!status) {
                    failure = status.error().message;
                    break;
                }
                continue;
            }
            if (!step.input.empty())
                step.process.keepStdinOpen = true;
            auto emit = [&callbacks](std::string_view data) {
                if (callbacks.onOutput)
                    callbacks.onOutput(data, false);
            };
            auto started = Process::start(step.process, emit, emit);
            if (!started) {
                failure = started.error().message;
                break;
            }
            {
                std::lock_guard lock(mutex_);
                current_ = started->get();
            }
            if (!step.input.empty()) {
                auto wrote = (*started)->writeStdin(step.input);
                (void)wrote;
                (*started)->closeStdin();
            }
            int code = (*started)->wait();
            {
                std::lock_guard lock(mutex_);
                current_ = nullptr;
            }
            if (std::find(step.successCodes.begin(), step.successCodes.end(), code) == step.successCodes.end()) {
                failure = cancelled_.load() ? "Cancelled" : step.label + " stopped with code " + std::to_string(code);
                break;
            }
        }
        refreshEnvironmentFromSystem();
        running_ = false;
        if (callbacks.onFinished)
            callbacks.onFinished(failure.empty(), failure);
    });
    return {};
}

void SetupJob::cancel()
{
    cancelled_ = true;
    std::lock_guard lock(mutex_);
    if (current_ != nullptr)
        current_->terminate();
}

void SetupJob::wait()
{
    if (worker_.joinable())
        worker_.join();
}

std::optional<fs::path> adbPath(ToolchainDetector& tools)
{
    if (auto sdk = tools.androidSdk()) {
        std::error_code ec;
        fs::path candidate = *sdk / "platform-tools" / exeFile("adb");
        if (fs::exists(candidate, ec))
            return candidate;
    }
    return findExecutable("adb");
}

std::optional<fs::path> emulatorPath(ToolchainDetector& tools)
{
    if (auto sdk = tools.androidSdk()) {
        std::error_code ec;
        fs::path candidate = *sdk / "emulator" / exeFile("emulator");
        if (fs::exists(candidate, ec))
            return candidate;
    }
    return findExecutable("emulator");
}

std::vector<AndroidDevice> androidDevices(ToolchainDetector& tools)
{
    std::vector<AndroidDevice> devices;
    auto adb = adbPath(tools);
    if (!adb)
        return devices;
    ProcessSpec spec;
    spec.program = paths::toUtf8(*adb);
    spec.arguments = {"devices", "-l"};
    spec.origin = Origin::System;
    auto result = runProcess(spec, std::chrono::seconds(10));
    if (!result)
        return devices;
    std::regex model("model:([^ ]+)");
    for (const auto& line : text::splitLines(result->out)) {
        auto parts = text::split(line, ' ');
        std::vector<std::string> fields;
        for (const auto& part : parts) {
            if (!part.empty())
                fields.push_back(part);
        }
        if (fields.size() < 2 || fields[0] == "List")
            continue;
        AndroidDevice device;
        device.serial = fields[0];
        device.ready = fields[1] == "device";
        std::smatch match;
        std::string label = device.serial;
        if (std::regex_search(line, match, model))
            label = text::replaceAll(match[1].str(), "_", " ");
        device.label = device.ready ? label : label + " (" + fields[1] + ")";
        devices.push_back(std::move(device));
    }
    return devices;
}

std::vector<std::string> androidVirtualDevices(ToolchainDetector& tools)
{
    std::vector<std::string> names;
    auto emulator = emulatorPath(tools);
    if (!emulator)
        return names;
    ProcessSpec spec;
    spec.program = paths::toUtf8(*emulator);
    spec.arguments = {"-list-avds"};
    spec.origin = Origin::System;
    auto result = runProcess(spec, std::chrono::seconds(15));
    if (!result)
        return names;
    for (const auto& line : text::splitLines(result->out)) {
        std::string name = text::trim(line);
        if (name.empty() || text::contains(name, " ") || text::startsWith(name, "INFO") || text::contains(name, "|"))
            continue;
        names.push_back(name);
    }
    return names;
}

std::string androidAcceleration(ToolchainDetector& tools)
{
    auto emulator = emulatorPath(tools);
    if (!emulator)
        return "The Android Emulator isn't installed.";
    ProcessSpec spec;
    spec.program = paths::toUtf8(*emulator);
    spec.arguments = {"-accel-check"};
    spec.origin = Origin::System;
    auto result = runProcess(spec, std::chrono::seconds(20));
    if (!result)
        return result.error().message;
    if (result->exitCode == 0)
        return {};
    std::string message;
    for (const auto& line : text::splitLines(result->out + result->err)) {
        std::string trimmed = text::trim(line);
        if (trimmed.empty() || trimmed == "accel:" || trimmed == "accel" || std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        message = trimmed;
        break;
    }
    return message.empty() ? "Emulator acceleration isn't available." : message;
}

std::string androidPackage(const fs::path& projectDirectory)
{
    for (const char* file : {"build.gradle.kts", "build.gradle"}) {
        auto content = paths::readWholeFile(projectDirectory / "app" / file);
        if (!content)
            continue;
        std::smatch match;
        std::regex pattern("applicationId\\s*=?\\s*[\"']([^\"']+)[\"']");
        if (std::regex_search(*content, match, pattern))
            return match[1];
    }
    return {};
}

std::vector<RunDevice> androidRunDevices(ToolchainDetector& tools)
{
    std::vector<RunDevice> output;
    std::set<std::string> taken;
    for (const auto& device : androidDevices(tools)) {
        output.push_back({"serial:" + device.serial, device.label, "android-device", device.ready});
        taken.insert(device.serial);
    }
    for (const auto& name : androidVirtualDevices(tools))
        output.push_back({"avd:" + name, text::replaceAll(name, "_", " ") + " (emulator)", "android-avd", true});
    return output;
}

int freeEmulatorPort(ToolchainDetector& tools)
{
    std::set<std::string> taken;
    for (const auto& device : androidDevices(tools))
        taken.insert(device.serial);
    for (int port = 5554; port < 5680; port += 2) {
        if (taken.count("emulator-" + std::to_string(port)) == 0)
            return port;
    }
    return 5554;
}

}
