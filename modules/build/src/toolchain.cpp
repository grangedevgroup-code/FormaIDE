#include "forma/build/toolchain.h"

#include <algorithm>
#include <regex>

#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma::build {

namespace {

struct ToolSpec {
    const char* id;
    const char* name;
    const char* category;
    std::vector<const char*> executables;
    std::vector<const char*> versionArgs;
    const char* versionPattern;
    const char* installWindows;
    const char* installMac;
    const char* installLinux;
    std::vector<const char*> usedFor;
};

const std::vector<ToolSpec>& toolSpecs()
{
    static const std::vector<ToolSpec> specs = {
        {"cmake", "CMake", "Build systems", {"cmake"}, {"--version"}, "version ([0-9.]+)", "winget install Kitware.CMake", "brew install cmake", "Install cmake with your package manager", {"C++", "Qt"}},
        {"ninja", "Ninja", "Build systems", {"ninja"}, {"--version"}, "([0-9.]+)", "winget install Ninja-build.Ninja", "brew install ninja", "Install ninja-build with your package manager", {"C++", "Qt"}},
        {"make", "Make", "Build systems", {"make", "gmake"}, {"--version"}, "Make ([0-9.]+)", "Available through MSYS2", "xcode-select --install", "Install make with your package manager", {"C", "C++"}},
        {"meson", "Meson", "Build systems", {"meson"}, {"--version"}, "([0-9.]+)", "pip install meson", "brew install meson", "Install meson with your package manager", {"C", "C++", "Vala"}},
        {"gcc", "GCC", "C and C++ compilers", {"g++", "gcc"}, {"--version"}, "([0-9]+\\.[0-9]+(\\.[0-9]+)?)", "Available through MSYS2", "brew install gcc", "Install gcc with your package manager", {"C", "C++"}},
        {"clang", "Clang", "C and C++ compilers", {"clang++", "clang"}, {"--version"}, "version ([0-9.]+)", "winget install LLVM.LLVM", "xcode-select --install", "Install clang with your package manager", {"C", "C++", "Objective-C"}},
        {"msvc", "MSVC", "C and C++ compilers", {}, {}, "", "Install Visual Studio with the Desktop development with C++ workload", "", "", {"C", "C++", "Windows"}},
        {"windows-sdk", "Windows SDK", "Platform SDKs", {}, {}, "", "Install it with Visual Studio or from developer.microsoft.com/windows/downloads/windows-sdk", "", "", {"Windows"}},
        {"qt", "Qt", "Frameworks", {}, {}, "", "Install from qt.io/download or with aqtinstall", "Install from qt.io/download or brew install qt", "Install qt6-base with your package manager or from qt.io/download", {"Qt"}},
        {"xcode", "Xcode", "Platform SDKs", {"xcodebuild"}, {"-version"}, "Xcode ([0-9.]+)", "", "Install Xcode from the App Store", "", {"iOS", "iPadOS", "macOS", "watchOS", "tvOS", "visionOS"}},
        {"swift", "Swift", "Languages", {"swift"}, {"--version"}, "Swift version ([0-9.]+)", "Install from swift.org/install", "xcode-select --install", "Install from swift.org/install", {"Swift"}},
        {"android-sdk", "Android SDK", "Platform SDKs", {}, {}, "", "Install Android Studio or the command line tools from developer.android.com", "Install Android Studio or the command line tools from developer.android.com", "Install Android Studio or the command line tools from developer.android.com", {"Android"}},
        {"android-ndk", "Android NDK", "Platform SDKs", {}, {}, "", "sdkmanager ndk-bundle", "sdkmanager ndk-bundle", "sdkmanager ndk-bundle", {"Android native code"}},
        {"adb", "Android Debug Bridge", "Devices", {"adb"}, {"version"}, "version ([0-9.]+)", "Comes with the Android SDK platform tools", "Comes with the Android SDK platform tools", "Comes with the Android SDK platform tools", {"Android"}},
        {"emulator", "Android Emulator", "Devices", {"emulator"}, {"-version"}, "version ([0-9.]+)", "sdkmanager emulator", "sdkmanager emulator", "sdkmanager emulator", {"Android"}},
        {"jdk", "JDK", "Languages", {}, {}, "", "winget install Microsoft.OpenJDK.21", "brew install openjdk", "Install an OpenJDK package", {"Java", "Kotlin", "Android"}},
        {"gradle", "Gradle", "Build systems", {}, {}, "", "winget install Gradle.Gradle", "brew install gradle", "Install gradle or use sdkman", {"Android", "Java", "Kotlin"}},
        {"maven", "Maven", "Build systems", {"mvn"}, {"--version"}, "Apache Maven ([0-9.]+)", "winget install Apache.Maven", "brew install maven", "Install maven with your package manager", {"Java"}},
        {"kotlin", "Kotlin compiler", "Languages", {"kotlinc"}, {"-version"}, "kotlinc-jvm ([0-9.]+)", "winget install JetBrains.Kotlin.Compiler", "brew install kotlin", "Use sdkman or your package manager", {"Kotlin"}},
        {"flutter", "Flutter SDK", "Frameworks", {"flutter"}, {"--version"}, "Flutter ([0-9.]+)", "Install from flutter.dev", "Install from flutter.dev", "Install from flutter.dev", {"Flutter", "Dart"}},
        {"dart", "Dart SDK", "Languages", {"dart"}, {"--version"}, "version: ([0-9.]+)", "Comes with Flutter or dart.dev/get-dart", "Comes with Flutter or dart.dev/get-dart", "Comes with Flutter or dart.dev/get-dart", {"Dart"}},
        {"dotnet", ".NET SDK", "Languages", {"dotnet"}, {"--version"}, "([0-9]+\\.[0-9.]+)", "winget install Microsoft.DotNet.SDK.10", "brew install dotnet", "Install dotnet-sdk with your package manager", {"C#", "F#", "Windows"}},
        {"node", "Node.js", "Languages", {"node"}, {"--version"}, "v([0-9.]+)", "winget install OpenJS.NodeJS", "brew install node", "Install nodejs with your package manager", {"JavaScript", "TypeScript", "React", "React Native"}},
        {"npm", "npm", "Package managers", {"npm"}, {"--version"}, "([0-9.]+)", "Comes with Node.js", "Comes with Node.js", "Comes with Node.js", {"JavaScript", "TypeScript"}},
        {"pnpm", "pnpm", "Package managers", {"pnpm"}, {"--version"}, "([0-9.]+)", "npm install -g pnpm", "npm install -g pnpm", "npm install -g pnpm", {"JavaScript", "TypeScript"}},
        {"yarn", "Yarn", "Package managers", {"yarn"}, {"--version"}, "([0-9.]+)", "npm install -g yarn", "npm install -g yarn", "npm install -g yarn", {"JavaScript", "TypeScript"}},
        {"bun", "Bun", "Languages", {"bun"}, {"--version"}, "([0-9.]+)", "Install from bun.sh", "Install from bun.sh", "Install from bun.sh", {"JavaScript", "TypeScript"}},
        {"deno", "Deno", "Languages", {"deno"}, {"--version"}, "deno ([0-9.]+)", "winget install DenoLand.Deno", "brew install deno", "Install from deno.com", {"JavaScript", "TypeScript"}},
        {"python", "Python", "Languages", {"python3", "python", "py"}, {"--version"}, "Python ([0-9.]+)", "winget install Python.Python.3", "brew install python", "Install python3 with your package manager", {"Python"}},
        {"go", "Go", "Languages", {"go"}, {"version"}, "go([0-9.]+)", "winget install GoLang.Go", "brew install go", "Install go with your package manager", {"Go"}},
        {"cargo", "Rust (Cargo)", "Languages", {"cargo"}, {"--version"}, "cargo ([0-9.]+)", "winget install Rustlang.Rustup", "brew install rustup", "Install from rustup.rs", {"Rust"}},
        {"ruby", "Ruby", "Languages", {"ruby"}, {"--version"}, "ruby ([0-9.]+)", "winget install RubyInstallerTeam.Ruby.3.3", "brew install ruby", "Install ruby with your package manager", {"Ruby"}},
        {"php", "PHP", "Languages", {"php"}, {"--version"}, "PHP ([0-9.]+)", "winget install PHP.PHP", "brew install php", "Install php with your package manager", {"PHP"}},
        {"lua", "Lua", "Languages", {"lua", "lua5.4"}, {"-v"}, "Lua ([0-9.]+)", "winget install DEVCOM.Lua", "brew install lua", "Install lua with your package manager", {"Lua"}},
        {"perl", "Perl", "Languages", {"perl"}, {"--version"}, "v([0-9.]+)", "winget install StrawberryPerl.StrawberryPerl", "brew install perl", "Install perl with your package manager", {"Perl"}},
        {"zig", "Zig", "Languages", {"zig"}, {"version"}, "([0-9.]+)", "winget install zig.zig", "brew install zig", "Install from ziglang.org", {"Zig"}},
        {"nim", "Nim", "Languages", {"nim"}, {"--version"}, "Version ([0-9.]+)", "Install from nim-lang.org", "brew install nim", "Install from nim-lang.org", {"Nim"}},
        {"ghc", "GHC", "Languages", {"ghc"}, {"--version"}, "version ([0-9.]+)", "Install with ghcup", "Install with ghcup", "Install with ghcup", {"Haskell"}},
        {"elixir", "Elixir", "Languages", {"elixir"}, {"--version"}, "Elixir ([0-9.]+)", "Install from elixir-lang.org", "brew install elixir", "Install from elixir-lang.org", {"Elixir"}},
        {"scala", "Scala", "Languages", {"scala"}, {"--version"}, "version ([0-9.]+)", "Install with coursier", "brew install scala", "Install with coursier", {"Scala"}},
        {"git", "Git", "Version control", {"git"}, {"--version"}, "version ([0-9.]+)", "winget install Git.Git", "xcode-select --install", "Install git with your package manager", {"Version control"}},
        {"clangd", "clangd", "Language servers", {"clangd"}, {"--version"}, "version ([0-9.]+)", "winget install LLVM.LLVM", "brew install llvm", "Install clangd with your package manager", {"C", "C++"}},
        {"lldb-dap", "lldb-dap", "Debuggers", {"lldb-dap", "lldb-vscode"}, {"--version"}, "version ([0-9.]+)", "winget install LLVM.LLVM", "Comes with Xcode", "Install lldb with your package manager", {"C", "C++", "Swift", "Rust"}},
        {"gdb", "GDB", "Debuggers", {"gdb"}, {"--version"}, "([0-9]+\\.[0-9.]+)", "Available through MSYS2", "brew install gdb", "Install gdb with your package manager", {"C", "C++", "Rust"}},
    };
    return specs;
}

const ToolSpec* findSpec(std::string_view id)
{
    for (const auto& spec : toolSpecs()) {
        if (id == spec.id)
            return &spec;
    }
    return nullptr;
}

std::string runForOutput(const fs::path& program, const std::vector<std::string>& arguments, int timeoutMs = 8000)
{
    ProcessSpec spec;
    spec.program = paths::toUtf8(program);
    spec.arguments = arguments;
    spec.origin = Origin::System;
    auto result = runProcess(spec, std::chrono::milliseconds(timeoutMs));
    if (!result)
        return {};
    return result->out + result->err;
}

std::vector<fs::path> subdirectories(const fs::path& parent)
{
    std::vector<fs::path> entries;
    std::error_code ec;
    if (!fs::is_directory(parent, ec))
        return entries;
    for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec))
            entries.push_back(it->path());
    }
    return entries;
}

std::vector<int> versionParts(std::string_view version)
{
    std::vector<int> parts;
    for (const auto& part : text::split(version, '.')) {
        try {
            parts.push_back(std::stoi(part));
        } catch (const std::exception&) {
            parts.push_back(0);
        }
    }
    return parts;
}

bool versionLess(const std::string& a, const std::string& b)
{
    return versionParts(a) < versionParts(b);
}

std::string exeName(std::string_view name)
{
#if defined(_WIN32)
    return std::string(name) + ".exe";
#else
    return std::string(name);
#endif
}

std::string batName(std::string_view name)
{
#if defined(_WIN32)
    return std::string(name) + ".bat";
#else
    return std::string(name);
#endif
}

std::string hintForHost(const ToolSpec& spec)
{
#if defined(_WIN32)
    return spec.installWindows;
#elif defined(__APPLE__)
    return spec.installMac;
#else
    return spec.installLinux;
#endif
}

}

std::string versionKey(std::string_view version)
{
    std::string output;
    for (char ch : version) {
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.')
            output.push_back(ch);
    }
    return output;
}

std::string hostPlatform()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__EMSCRIPTEN__)
    return "web";
#else
    return "linux";
#endif
}

bool hostCanBuildFor(std::string_view platform, std::string* reason)
{
    std::string host = hostPlatform();
    static const std::vector<std::string> apple = {"ios", "ipados", "macos", "watchos", "tvos", "visionos"};
    if (std::find(apple.begin(), apple.end(), platform) != apple.end() && host != "macos") {
        if (reason != nullptr)
            *reason = "Apple platforms need macOS and Xcode. The project can be generated here, but it has to be built on a Mac.";
        return false;
    }
    if (platform == "windows" && host != "windows") {
        if (reason != nullptr)
            *reason = "Windows applications need Windows and its SDK to build. Qt and .NET console projects are exceptions.";
        return false;
    }
    if (platform == "linux" && host != "linux") {
        if (reason != nullptr)
            *reason = "Linux applications need a Linux system or container to build.";
        return false;
    }
    return true;
}

ToolchainDetector::ToolchainDetector() = default;

std::vector<std::string> ToolchainDetector::knownToolIds() const
{
    std::vector<std::string> ids;
    for (const auto& spec : toolSpecs())
        ids.emplace_back(spec.id);
    return ids;
}

std::vector<ToolInfo> ToolchainDetector::detectAll(bool refresh)
{
    std::vector<ToolInfo> tools;
    for (const auto& spec : toolSpecs())
        tools.push_back(detect(spec.id, refresh));
    return tools;
}

ToolInfo ToolchainDetector::detect(std::string_view id, bool refresh)
{
    {
        std::lock_guard lock(mutex_);
        auto cached = cache_.find(std::string(id));
        if (cached != cache_.end() && !refresh)
            return cached->second;
    }
    ToolInfo info = probe(id);
    std::lock_guard lock(mutex_);
    cache_[std::string(id)] = info;
    return info;
}

bool ToolchainDetector::isAvailable(std::string_view id)
{
    return detect(id).available;
}

ToolInfo ToolchainDetector::probe(std::string_view id)
{
    ToolInfo info;
    info.id = std::string(id);
    const ToolSpec* spec = findSpec(id);
    if (spec == nullptr) {
        info.name = std::string(id);
        return info;
    }
    info.name = spec->name;
    info.category = spec->category;
    info.installHint = hintForHost(*spec);
    for (const char* entry : spec->usedFor)
        info.usedFor.emplace_back(entry);

    if (id == "msvc") {
#if defined(_WIN32)
        fs::path vswhere = paths::fromUtf8(paths::env("ProgramFiles(x86)").value_or("C:\\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
        std::error_code ec;
        if (fs::exists(vswhere, ec)) {
            ProcessSpec process;
            process.program = paths::toUtf8(vswhere);
            process.arguments = {"-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json", "-utf8"};
            process.origin = Origin::System;
            auto result = runProcess(process, std::chrono::seconds(15));
            if (result && result->exitCode == 0) {
                auto parsed = parseJson(result->out);
                if (parsed && parsed->is_array() && !parsed->empty()) {
                    const Json& first = (*parsed)[0];
                    info.available = true;
                    info.path = jsonString(first, "installationPath");
                    info.version = jsonString(first, "installationVersion");
                    info.note = jsonString(first, "displayName");
                }
            }
        }
#else
        info.note = "MSVC only runs on Windows";
#endif
        return info;
    }
    if (id == "windows-sdk") {
#if defined(_WIN32)
        fs::path root = paths::fromUtf8(paths::env("ProgramFiles(x86)").value_or("C:\\Program Files (x86)")) / "Windows Kits" / "10" / "Include";
        std::vector<std::string> versions;
        for (const auto& directory : subdirectories(root)) {
            auto name = paths::toUtf8(directory.filename());
            if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
                versions.push_back(name);
        }
        if (!versions.empty()) {
            std::sort(versions.begin(), versions.end(), versionLess);
            info.available = true;
            info.version = versions.back();
            info.path = paths::toUtf8(root.parent_path());
        }
#else
        info.note = "The Windows SDK only runs on Windows";
#endif
        return info;
    }
    if (id == "qt") {
        auto kit = bestQtKit();
        if (kit) {
            info.available = true;
            info.version = kit->version;
            info.path = paths::toUtf8(kit->prefix);
            info.note = kit->compiler;
        }
        return info;
    }
    if (id == "android-sdk") {
        auto sdk = androidSdk();
        if (sdk) {
            info.available = true;
            info.path = paths::toUtf8(*sdk);
            std::vector<std::string> versions;
            for (const auto& directory : subdirectories(*sdk / "platforms")) {
                std::string name = paths::toUtf8(directory.filename());
                if (text::startsWith(name, "android-"))
                    versions.push_back(name.substr(8));
            }
            std::sort(versions.begin(), versions.end(), versionLess);
            if (!versions.empty())
                info.version = "API " + versions.back();
        }
        return info;
    }
    if (id == "android-ndk") {
        auto sdk = androidSdk();
        if (sdk) {
            std::vector<std::string> versions;
            for (const auto& directory : subdirectories(*sdk / "ndk"))
                versions.push_back(paths::toUtf8(directory.filename()));
            std::sort(versions.begin(), versions.end(), versionLess);
            if (!versions.empty()) {
                info.available = true;
                info.version = versions.back();
                info.path = paths::toUtf8(*sdk / "ndk" / paths::fromUtf8(versions.back()));
            }
        }
        return info;
    }
    if (id == "jdk") {
        auto home = javaHome();
        if (home) {
            info.available = true;
            info.path = paths::toUtf8(*home);
            std::string output = runForOutput(*home / "bin" / exeName("javac"), {"-version"});
            std::smatch match;
            static const std::regex pattern("javac ([0-9][0-9._]*)");
            if (std::regex_search(output, match, pattern))
                info.version = match[1];
        }
        return info;
    }
    if (id == "gradle") {
        auto path = gradle();
        if (path) {
            info.available = true;
            info.path = paths::toUtf8(*path);
            auto version = path->parent_path().parent_path().filename();
            std::string name = paths::toUtf8(version);
            if (text::startsWith(name, "gradle-"))
                info.version = name.substr(7);
        }
        return info;
    }

    std::vector<fs::path> extra;
    if (id == "adb" || id == "emulator") {
        auto sdk = androidSdk();
        if (sdk) {
            extra.push_back(*sdk / "platform-tools");
            extra.push_back(*sdk / "emulator");
        }
    }
    if (id == "clangd" || id == "lldb-dap") {
#if defined(_WIN32)
        extra.push_back(paths::fromUtf8(paths::env("ProgramFiles").value_or("C:\\Program Files")) / "LLVM" / "bin");
#endif
    }
    for (const char* executable : spec->executables) {
        auto found = findExecutable(executable, extra);
        if (!found)
            continue;
#if defined(_WIN32)
        if (id == "python" && text::contains(paths::toUtf8(*found), "WindowsApps"))
            continue;
#endif
        std::vector<std::string> arguments;
        for (const char* argument : spec->versionArgs)
            arguments.emplace_back(argument);
        std::string output = runForOutput(*found, arguments);
        if (output.empty() && !arguments.empty())
            continue;
        info.available = true;
        info.path = paths::toUtf8(*found);
        if (spec->versionPattern != nullptr && spec->versionPattern[0] != '\0') {
            std::smatch match;
            std::regex pattern(spec->versionPattern);
            if (std::regex_search(output, match, pattern))
                info.version = match[1];
        }
        break;
    }
    return info;
}

std::vector<QtKit> ToolchainDetector::qtKits()
{
    std::vector<QtKit> kits;
    std::vector<fs::path> roots;
    if (auto dir = paths::env("QT_ROOT_DIR"))
        roots.push_back(paths::fromUtf8(*dir));
    if (auto dir = paths::env("QTDIR"))
        roots.push_back(paths::fromUtf8(*dir));
#if defined(_WIN32)
    roots.push_back("C:/Qt");
#else
    roots.push_back(paths::homeDir() / "Qt");
    roots.push_back("/opt/Qt");
#endif
    std::error_code ec;
    auto addKit = [&](const fs::path& prefix, const std::string& version, const std::string& compiler) {
        if (!fs::exists(prefix / "lib" / "cmake" / "Qt6", ec))
            return;
        for (const auto& existing : kits) {
            if (existing.prefix == prefix)
                return;
        }
        kits.push_back({version, compiler, prefix});
    };
    for (const auto& root : roots) {
        addKit(root, "", paths::toUtf8(root.filename()));
        for (const auto& versionDirectory : subdirectories(root)) {
            std::string version = paths::toUtf8(versionDirectory.filename());
            addKit(versionDirectory, "", version);
            if (version.empty() || !std::isdigit(static_cast<unsigned char>(version[0])))
                continue;
            for (const auto& kit : subdirectories(versionDirectory))
                addKit(kit, version, paths::toUtf8(kit.filename()));
        }
    }
    for (const char* name : {"qmake6", "qmake"}) {
        auto qmake = findExecutable(name);
        if (!qmake)
            continue;
        std::string output = runForOutput(*qmake, {"-query", "QT_INSTALL_PREFIX"});
        std::string prefix = text::trim(output);
        std::string versionOutput = text::trim(runForOutput(*qmake, {"-query", "QT_VERSION"}));
        if (!prefix.empty())
            addKit(paths::fromUtf8(prefix), versionOutput, "system");
    }
    if (auto cmakePrefix = paths::env("CMAKE_PREFIX_PATH")) {
#if defined(_WIN32)
        char separator = ';';
#else
        char separator = ':';
#endif
        for (const auto& item : text::split(*cmakePrefix, separator)) {
            if (!item.empty())
                addKit(paths::fromUtf8(item), "", "CMAKE_PREFIX_PATH");
        }
    }
    for (auto& kit : kits) {
        if (!kit.version.empty())
            continue;
        std::string output;
        auto qmake = findExecutable(paths::toUtf8(kit.prefix / "bin" / "qmake6"));
        if (qmake)
            kit.version = text::trim(runForOutput(*qmake, {"-query", "QT_VERSION"}));
    }
    std::sort(kits.begin(), kits.end(), [](const QtKit& a, const QtKit& b) { return versionLess(b.version, a.version); });
    return kits;
}

std::optional<QtKit> ToolchainDetector::bestQtKit()
{
    auto kits = qtKits();
    if (kits.empty())
        return std::nullopt;
#if defined(_WIN32)
    for (const auto& kit : kits) {
        if (text::contains(kit.compiler, "msvc"))
            return kit;
    }
#endif
    return kits.front();
}

std::map<std::string, std::string> ToolchainDetector::msvcEnvironment()
{
    {
        std::lock_guard lock(mutex_);
        if (msvcEnvironment_)
            return *msvcEnvironment_;
    }
    std::map<std::string, std::string> variables;
#if defined(_WIN32)
    ToolInfo tool = detect("msvc");
    if (tool.available) {
        fs::path script = paths::fromUtf8(tool.path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
        fs::path temp = paths::userCacheDir() / "vcvars-capture.cmd";
        std::string body = "@echo off\r\ncall \"" + paths::toUtf8(script) + "\" >nul 2>&1\r\nset\r\n";
        if (paths::writeFileAtomic(temp, body)) {
            ProcessSpec process;
            process.program = paths::env("ComSpec").value_or("cmd.exe");
            process.arguments = {"/d", "/c", paths::toUtf8(temp)};
            process.origin = Origin::System;
            auto result = runProcess(process, std::chrono::seconds(60));
            if (result && result->exitCode == 0) {
                for (const auto& line : text::splitLines(result->out)) {
                    auto separator = line.find('=');
                    if (separator != std::string::npos && separator > 0)
                        variables[line.substr(0, separator)] = line.substr(separator + 1);
                }
            }
            std::error_code ec;
            fs::remove(temp, ec);
        }
    }
#endif
    std::lock_guard lock(mutex_);
    msvcEnvironment_ = variables;
    return variables;
}

std::optional<fs::path> ToolchainDetector::androidSdk()
{
    std::error_code ec;
    std::vector<fs::path> candidates;
    if (auto dir = paths::env("ANDROID_HOME"))
        candidates.push_back(paths::fromUtf8(*dir));
    if (auto dir = paths::env("ANDROID_SDK_ROOT"))
        candidates.push_back(paths::fromUtf8(*dir));
#if defined(_WIN32)
    if (auto dir = paths::env("LOCALAPPDATA"))
        candidates.push_back(paths::fromUtf8(*dir) / "Android" / "Sdk");
#elif defined(__APPLE__)
    candidates.push_back(paths::homeDir() / "Library" / "Android" / "sdk");
#else
    candidates.push_back(paths::homeDir() / "Android" / "Sdk");
    candidates.push_back("/opt/android-sdk");
#endif
    for (const auto& candidate : candidates) {
        if (fs::is_directory(candidate / "platforms", ec) || fs::is_directory(candidate / "platform-tools", ec))
            return candidate;
    }
    return std::nullopt;
}

std::optional<fs::path> ToolchainDetector::javaHome()
{
    std::error_code ec;
    auto hasJavac = [&](const fs::path& home) { return fs::exists(home / "bin" / exeName("javac"), ec); };
    if (auto dir = paths::env("JAVA_HOME")) {
        fs::path home = paths::fromUtf8(*dir);
        if (hasJavac(home))
            return home;
    }
    if (auto javac = findExecutable("javac")) {
        std::error_code canonicalError;
        fs::path real = fs::canonical(*javac, canonicalError);
        if (!canonicalError)
            return real.parent_path().parent_path();
        return javac->parent_path().parent_path();
    }
    std::vector<fs::path> roots;
#if defined(_WIN32)
    fs::path programFiles = paths::fromUtf8(paths::env("ProgramFiles").value_or("C:\\Program Files"));
    roots = {programFiles / "Microsoft", programFiles / "Java", programFiles / "Eclipse Adoptium", programFiles / "Android" / "Android Studio"};
#elif defined(__APPLE__)
    roots = {"/Library/Java/JavaVirtualMachines"};
#else
    roots = {"/usr/lib/jvm"};
#endif
    std::vector<fs::path> found;
    for (const auto& root : roots) {
        for (const auto& directory : subdirectories(root)) {
            if (hasJavac(directory))
                found.push_back(directory);
            if (hasJavac(directory / "Contents" / "Home"))
                found.push_back(directory / "Contents" / "Home");
        }
    }
    std::sort(found.begin(), found.end());
    if (!found.empty())
        return found.back();
    return std::nullopt;
}

std::optional<fs::path> ToolchainDetector::gradle()
{
    if (auto path = findExecutable("gradle"))
        return path;
    std::error_code ec;
    if (auto dir = paths::env("GRADLE_HOME")) {
        fs::path candidate = paths::fromUtf8(*dir) / "bin" / batName("gradle");
        if (fs::exists(candidate, ec))
            return candidate;
    }
    fs::path dists = paths::homeDir() / ".gradle" / "wrapper" / "dists";
    std::vector<std::pair<std::string, fs::path>> found;
    for (const auto& distribution : subdirectories(dists)) {
        for (const auto& hash : subdirectories(distribution)) {
            for (const auto& home : subdirectories(hash)) {
                fs::path candidate = home / "bin" / batName("gradle");
                if (fs::exists(candidate, ec))
                    found.emplace_back(versionKey(paths::toUtf8(home.filename())), candidate);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return versionLess(a.first, b.first); });
    if (!found.empty())
        return found.back().second;
    return std::nullopt;
}

Json ToolchainDetector::toJson(const ToolInfo& tool)
{
    return Json{{"id", tool.id}, {"name", tool.name}, {"category", tool.category}, {"available", tool.available}, {"version", tool.version},
                {"path", tool.path}, {"note", tool.note}, {"installHint", tool.installHint}, {"usedFor", tool.usedFor}};
}

}
