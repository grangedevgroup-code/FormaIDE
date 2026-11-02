#include <algorithm>
#include <chrono>

#include "forma/build/build_system.h"
#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma::build {

const char* artifactKindForPath(std::string_view path)
{
    std::string lower = text::toLower(path);
    struct Entry {
        const char* suffix;
        const char* kind;
    };
    static const Entry entries[] = {
        {".apk", "Android package (APK)"}, {".aab", "Android bundle (AAB)"}, {".ipa", "iOS archive (IPA)"}, {".exe", "Windows executable"},
        {".msi", "Windows installer (MSI)"}, {".msix", "Windows package (MSIX)"}, {".dmg", "macOS disk image"}, {".pkg", "macOS installer"},
        {".appimage", "Linux AppImage"}, {".deb", "Debian package"}, {".rpm", "RPM package"}, {".dll", "Dynamic library"}, {".so", "Dynamic library"},
        {".dylib", "Dynamic library"}, {".lib", "Static library"}, {".a", "Static library"}, {".jar", "Java archive"}, {".wasm", "WebAssembly module"},
    };
    for (const auto& entry : entries) {
        if (text::endsWith(lower, entry.suffix))
            return entry.kind;
    }
    if (text::contains(lower, ".app/") || text::endsWith(lower, ".app"))
        return "macOS or iOS application";
    return nullptr;
}

std::vector<Artifact> collectArtifacts(const fs::path& root, const std::vector<std::string>& searchDirectories, std::int64_t notBeforeMillis)
{
    std::vector<Artifact> artifacts;
    std::error_code ec;
    for (const auto& directory : searchDirectories) {
        fs::path start = root / paths::fromUtf8(directory);
        if (!fs::is_directory(start, ec))
            continue;
        for (fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = paths::toUtf8(it->path().filename());
            if (it->is_directory(ec)) {
                if (name == "CMakeFiles" || name == ".cmake" || name == "_deps" || name == "intermediates" || name == "tmp" || name == "obj" || name == "kotlin" || name == "generated" || name == "node_modules")
                    it.disable_recursion_pending();
                if (text::endsWith(name, ".app")) {
                    Artifact artifact;
                    artifact.path = paths::toGeneric(it->path().lexically_relative(root));
                    artifact.kind = "macOS or iOS application";
                    artifacts.push_back(std::move(artifact));
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            std::string relative = paths::toGeneric(it->path().lexically_relative(root));
            const char* kind = artifactKindForPath(relative);
            bool executable = false;
#if !defined(_WIN32)
            auto permissions = it->status(ec).permissions();
            executable = (permissions & fs::perms::owner_exec) != fs::perms::none && !it->path().has_extension();
#endif
            if (kind == nullptr && !executable)
                continue;
            if (text::endsWith(text::toLower(name), "cmakecxxcompilerid.exe") || text::startsWith(name, "CMake"))
                continue;
            Artifact artifact;
            artifact.path = relative;
            artifact.kind = kind != nullptr ? kind : "Executable";
            artifact.size = it->file_size(ec);
            auto time = it->last_write_time(ec);
            auto offset = time - fs::file_time_type::clock::now();
            auto system = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(offset);
            artifact.modifiedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
            if (notBeforeMillis > 0 && artifact.modifiedMillis < notBeforeMillis)
                continue;
            artifacts.push_back(std::move(artifact));
        }
    }
    return artifacts;
}

namespace {

class LineSplitter {
public:
    explicit LineSplitter(std::function<void(std::string_view)> onLine) : onLine_(std::move(onLine)) {}

    void feed(std::string_view data)
    {
        buffer_.append(data);
        std::size_t start = 0;
        while (true) {
            std::size_t newline = buffer_.find('\n', start);
            if (newline == std::string::npos)
                break;
            std::size_t end = newline;
            if (end > start && buffer_[end - 1] == '\r')
                --end;
            onLine_(std::string_view(buffer_).substr(start, end - start));
            start = newline + 1;
        }
        buffer_.erase(0, start);
    }

    void finish()
    {
        if (!buffer_.empty()) {
            onLine_(buffer_);
            buffer_.clear();
        }
    }

private:
    std::function<void(std::string_view)> onLine_;
    std::string buffer_;
};

}

BuildJob::BuildJob() = default;

BuildJob::~BuildJob()
{
    cancel();
    wait();
}

Status BuildJob::start(BuildPlan plan, fs::path root, BuildCallbacks callbacks)
{
    if (active_.load())
        return makeError("build.running", "A build is already running");
    wait();
    cancelled_ = false;
    active_ = true;
    result_ = {};
    result_.state = BuildState::Running;
    thread_ = std::thread([this, plan = std::move(plan), root = std::move(root), callbacks = std::move(callbacks)]() mutable {
        run(std::move(plan), std::move(root), std::move(callbacks));
    });
    return {};
}

void BuildJob::cancel()
{
    cancelled_ = true;
    std::lock_guard lock(mutex_);
    if (current_)
        current_->terminate();
}

void BuildJob::wait()
{
    if (thread_.joinable())
        thread_.join();
}

bool BuildJob::running() const
{
    return active_.load();
}

void BuildJob::run(BuildPlan plan, fs::path root, BuildCallbacks callbacks)
{
    auto started = std::chrono::steady_clock::now();
    std::int64_t startedMillis = paths::unixMillis() - 2000;
    DiagnosticParser parser;
    std::mutex diagnosticMutex;

    auto handleLine = [&](std::string_view line, bool isError) {
        if (callbacks.onOutput)
            callbacks.onOutput(line, isError);
        std::lock_guard lock(diagnosticMutex);
        auto diagnostic = parser.parseLine(line);
        if (diagnostic) {
            bool duplicate = std::any_of(result_.diagnostics.begin(), result_.diagnostics.end(), [&](const BuildDiagnostic& item) {
                return item.file == diagnostic->file && item.line == diagnostic->line && item.column == diagnostic->column && item.message == diagnostic->message;
            });
            if (!duplicate) {
                result_.diagnostics.push_back(*diagnostic);
                if (callbacks.onDiagnostic)
                    callbacks.onDiagnostic(*diagnostic);
            }
        }
    };

    BuildState state = BuildState::Succeeded;
    int exitCode = 0;
    for (const auto& step : plan.steps) {
        if (cancelled_.load()) {
            state = BuildState::Cancelled;
            break;
        }
        std::error_code ec;
        if (step.skipIfExists && fs::exists(step.existsPath, ec))
            continue;
        if (callbacks.onStep)
            callbacks.onStep(step.label);
        LineSplitter out([&](std::string_view line) { handleLine(line, false); });
        LineSplitter err([&](std::string_view line) { handleLine(line, true); });
        auto process = Process::start(
            step.process, [&](std::string_view data) { out.feed(data); }, [&](std::string_view data) { err.feed(data); });
        if (!process) {
            handleLine("Could not start " + step.process.program + ": " + process.error().message, true);
            state = BuildState::Failed;
            exitCode = -1;
            result_.failedStep = step.label;
            break;
        }
        {
            std::lock_guard lock(mutex_);
            current_ = std::move(process.value());
            if (cancelled_.load())
                current_->terminate();
        }
        exitCode = current_->wait();
        out.finish();
        err.finish();
        {
            std::lock_guard lock(mutex_);
            current_.reset();
        }
        if (cancelled_.load()) {
            state = BuildState::Cancelled;
            break;
        }
        if (exitCode != 0) {
            state = BuildState::Failed;
            result_.failedStep = step.label;
            break;
        }
    }

    result_.state = state;
    result_.exitCode = exitCode;
    result_.durationMillis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (state == BuildState::Succeeded)
        result_.artifacts = collectArtifacts(root, plan.artifactDirectories, 0);
    (void)startedMillis;
    log::info("build", "finished", Json{{"state", static_cast<int>(state)}, {"exitCode", exitCode}, {"millis", result_.durationMillis}});
    active_ = false;
    if (callbacks.onFinished)
        callbacks.onFinished(result_);
}

BuildResult runBuildBlocking(BuildPlan plan, const fs::path& root, BuildCallbacks callbacks)
{
    BuildJob job;
    auto status = job.start(std::move(plan), root, std::move(callbacks));
    if (!status) {
        BuildResult failed;
        failed.state = BuildState::Failed;
        return failed;
    }
    job.wait();
    return job.result();
}

}
