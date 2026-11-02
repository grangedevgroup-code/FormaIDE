#include "forma/core/process.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "forma/core/log.h"
#include "forma/core/paths.h"
#include "forma/core/text.h"

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace forma {

std::string quoteWindowsArgument(std::string_view argument)
{
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos)
        return std::string(argument);
    std::string output = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            output.append(backslashes * 2 + 1, '\\');
            output.push_back('"');
        } else {
            output.append(backslashes, '\\');
            output.push_back(ch);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, '\\');
    output.push_back('"');
    return output;
}

namespace {

std::string caretEscape(std::string_view input)
{
    std::string output;
    output.reserve(input.size() + 8);
    for (char ch : input) {
        if (std::strchr("()[]%!^\"`<>&|;, *?=", ch) != nullptr && ch != '\0')
            output.push_back('^');
        output.push_back(ch);
    }
    return output;
}

}

std::string buildWindowsBatchCommand(std::string_view program, const std::vector<std::string>& arguments)
{
    std::string line = caretEscape(program);
    for (const auto& argument : arguments) {
        line.push_back(' ');
        line += caretEscape(quoteWindowsArgument(argument));
    }
    return line;
}

#if defined(_WIN32)

namespace {

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), length);
    return output;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty())
        return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

struct HandleCloser {
    void operator()(HANDLE handle) const
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

std::wstring buildEnvironmentBlock(const ProcessSpec& spec)
{
    std::map<std::string, std::string> merged;
    if (spec.inheritEnvironment)
        merged = currentEnvironment();
    for (const auto& [key, value] : spec.environment) {
        auto existing = std::find_if(merged.begin(), merged.end(), [&](const auto& item) {
            return _stricmp(item.first.c_str(), key.c_str()) == 0;
        });
        if (existing != merged.end())
            merged.erase(existing);
        merged[key] = value;
    }
    std::vector<std::pair<std::wstring, std::wstring>> sorted;
    sorted.reserve(merged.size());
    for (const auto& [key, value] : merged)
        sorted.emplace_back(wide(key), wide(value));
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring block;
    for (const auto& [key, value] : sorted) {
        block += key;
        block.push_back(L'=');
        block += value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

}

struct Process::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stderrRead = nullptr;
    int processId = 0;
    std::thread stdoutThread;
    std::thread stderrThread;
    std::thread waiter;
    mutable std::mutex mutex;
    std::condition_variable finished;
    std::optional<int> exit;
    std::atomic<int> openReaders{0};
    ProcessOutputCallback onStdout;
    ProcessOutputCallback onStderr;
    ProcessExitCallback onExit;

    void readLoop(HANDLE pipe, const ProcessOutputCallback& callback)
    {
        char buffer[8192];
        DWORD count = 0;
        while (ReadFile(pipe, buffer, sizeof buffer, &count, nullptr) && count > 0) {
            if (callback)
                callback(std::string_view(buffer, count));
        }
        openReaders.fetch_sub(1);
    }

    void waitLoop()
    {
        WaitForSingleObject(process, INFINITE);
        for (int i = 0; i < 400 && openReaders.load() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (openReaders.load() > 0) {
            if (stdoutThread.joinable())
                CancelSynchronousIo(stdoutThread.native_handle());
            if (stderrThread.joinable())
                CancelSynchronousIo(stderrThread.native_handle());
        }
        if (stdoutThread.joinable())
            stdoutThread.join();
        if (stderrThread.joinable())
            stderrThread.join();
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        {
            std::lock_guard lock(mutex);
            exit = static_cast<int>(code);
        }
        if (onExit)
            onExit(static_cast<int>(code));
        finished.notify_all();
    }

    ~Impl()
    {
        if (waiter.joinable()) {
            bool alive;
            {
                std::lock_guard lock(mutex);
                alive = !exit.has_value();
            }
            if (alive && job != nullptr)
                TerminateJobObject(job, 1);
            waiter.join();
        }
        HandleCloser close;
        close(stdinWrite);
        close(stdoutRead);
        close(stderrRead);
        close(process);
        close(job);
    }
};

Result<std::unique_ptr<Process>> Process::start(const ProcessSpec& spec, ProcessOutputCallback onStdout,
                                                ProcessOutputCallback onStderr, ProcessExitCallback onExit)
{
    auto resolved = findExecutable(spec.program);
    if (!resolved)
        return makeError("process.notfound", "Cannot find program: " + spec.program);

    std::wstring applicationName;
    std::wstring commandLine;
    std::wstring extension = text::toLower(paths::toUtf8(resolved->extension())) == ".bat"
            || text::toLower(paths::toUtf8(resolved->extension())) == ".cmd"
        ? L"batch"
        : L"";
    if (!extension.empty()) {
        for (const auto& argument : spec.arguments) {
            if (argument.find_first_of("\r\n") != std::string::npos)
                return makeError("process.argument", "A batch file argument cannot contain a line break");
        }
        auto comspec = paths::env("ComSpec");
        applicationName = wide(comspec.value_or("C:\\Windows\\System32\\cmd.exe"));
        commandLine = L"cmd.exe /d /s /c \"" + wide(buildWindowsBatchCommand(paths::toUtf8(*resolved), spec.arguments)) + L"\"";
    } else {
        applicationName = resolved->wstring();
        std::string line = quoteWindowsArgument(paths::toUtf8(*resolved));
        for (const auto& argument : spec.arguments) {
            line.push_back(' ');
            line += quoteWindowsArgument(argument);
        }
        commandLine = wide(line);
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof attributes;
    attributes.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    HandleCloser close;

    if (!CreatePipe(&stdinRead, &stdinWrite, &attributes, 0) || !CreatePipe(&stdoutRead, &stdoutWrite, &attributes, 0))
        return makeError("process.pipe", "Cannot create pipes");
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    if (!spec.mergeStderr) {
        if (!CreatePipe(&stderrRead, &stderrWrite, &attributes, 0))
            return makeError("process.pipe", "Cannot create pipes");
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
    }

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeStorage(attributeSize);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize);
    std::vector<HANDLE> inherited = {stdinRead, stdoutWrite};
    if (stderrWrite != nullptr)
        inherited.push_back(stderrWrite);
    UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                              inherited.size() * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdinRead;
    startup.StartupInfo.hStdOutput = stdoutWrite;
    startup.StartupInfo.hStdError = spec.mergeStderr ? stdoutWrite : stderrWrite;
    startup.lpAttributeList = attributeList;

    std::wstring environment = buildEnvironmentBlock(spec);
    std::wstring directory = spec.workingDirectory.empty() ? std::wstring() : spec.workingDirectory.wstring();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (spec.lowerPriority)
        flags |= BELOW_NORMAL_PRIORITY_CLASS;

    PROCESS_INFORMATION info{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    BOOL created = CreateProcessW(applicationName.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, flags,
                                  environment.data(), directory.empty() ? nullptr : directory.c_str(),
                                  &startup.StartupInfo, &info);
    DWORD createError = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributeList);
    close(stdinRead);
    close(stdoutWrite);
    close(stderrWrite);
    if (!created) {
        close(stdinWrite);
        close(stdoutRead);
        close(stderrRead);
        return makeError("process.start",
                         "Cannot start " + spec.program + " (Windows error " + std::to_string(createError) + ")");
    }

    auto impl = std::make_unique<Impl>();
    impl->job = CreateJobObjectW(nullptr, nullptr);
    if (impl->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (spec.memoryLimitBytes > 0) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.JobMemoryLimit = static_cast<SIZE_T>(spec.memoryLimitBytes);
        }
        if (spec.origin == Origin::Plugin)
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(impl->job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    impl->process = info.hProcess;
    impl->processId = static_cast<int>(info.dwProcessId);
    impl->stdoutRead = stdoutRead;
    impl->stderrRead = stderrRead;
    impl->stdinWrite = stdinWrite;
    impl->onStdout = std::move(onStdout);
    impl->onStderr = std::move(onStderr);
    impl->onExit = std::move(onExit);
    if (!spec.keepStdinOpen) {
        close(impl->stdinWrite);
        impl->stdinWrite = nullptr;
    }
    Impl* raw = impl.get();
    impl->openReaders = stderrRead != nullptr ? 2 : 1;
    impl->stdoutThread = std::thread([raw] { raw->readLoop(raw->stdoutRead, raw->onStdout); });
    if (stderrRead != nullptr)
        impl->stderrThread = std::thread([raw] { raw->readLoop(raw->stderrRead, raw->onStderr); });
    impl->waiter = std::thread([raw] { raw->waitLoop(); });
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}

Status Process::writeStdin(std::string_view data)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->stdinWrite == nullptr)
        return makeError("process.stdin", "The process input is closed");
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        if (!WriteFile(impl_->stdinWrite, data.data() + offset, static_cast<DWORD>(data.size() - offset), &written, nullptr))
            return makeError("process.stdin", "Cannot write to the process");
        offset += written;
    }
    return {};
}

void Process::closeStdin()
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->stdinWrite != nullptr) {
        CloseHandle(impl_->stdinWrite);
        impl_->stdinWrite = nullptr;
    }
}

void Process::terminate()
{
    if (impl_->job != nullptr)
        TerminateJobObject(impl_->job, 1);
    else
        TerminateProcess(impl_->process, 1);
}

int Process::id() const
{
    return impl_->processId;
}

std::map<std::string, std::string> currentEnvironment()
{
    std::map<std::string, std::string> variables;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr)
        return variables;
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += wcslen(cursor) + 1) {
        std::wstring_view entry(cursor);
        auto separator = entry.find(L'=', 1);
        if (separator == std::wstring_view::npos)
            continue;
        variables[narrow(entry.substr(0, separator))] = narrow(entry.substr(separator + 1));
    }
    FreeEnvironmentStringsW(block);
    return variables;
}

bool isProcessAlive(int processId)
{
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
    if (handle == nullptr)
        return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
}

int currentProcessId()
{
    return static_cast<int>(GetCurrentProcessId());
}

#elif !defined(__EMSCRIPTEN__)

struct Process::Impl {
    pid_t pid = -1;
    int stdinWrite = -1;
    int stdoutRead = -1;
    int stderrRead = -1;
    std::thread stdoutThread;
    std::thread stderrThread;
    std::thread waiter;
    mutable std::mutex mutex;
    std::condition_variable finished;
    std::optional<int> exit;
    std::atomic<int> openReaders{0};
    std::atomic<bool> cancelReaders{false};
    ProcessOutputCallback onStdout;
    ProcessOutputCallback onStderr;
    ProcessExitCallback onExit;

    void readLoop(int pipe, const ProcessOutputCallback& callback)
    {
        char buffer[8192];
        while (true) {
            pollfd item{pipe, POLLIN, 0};
            int ready = ::poll(&item, 1, 100);
            if (ready < 0 && errno != EINTR)
                break;
            if (ready <= 0) {
                if (cancelReaders.load())
                    break;
                continue;
            }
            auto count = ::read(pipe, buffer, sizeof buffer);
            if (count > 0) {
                if (callback)
                    callback(std::string_view(buffer, static_cast<std::size_t>(count)));
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        openReaders.fetch_sub(1);
    }

    void waitLoop()
    {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        for (int i = 0; i < 400 && openReaders.load() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cancelReaders = true;
        if (stdoutThread.joinable())
            stdoutThread.join();
        if (stderrThread.joinable())
            stderrThread.join();
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        {
            std::lock_guard lock(mutex);
            exit = code;
        }
        if (onExit)
            onExit(code);
        finished.notify_all();
    }

    ~Impl()
    {
        if (waiter.joinable()) {
            bool alive;
            {
                std::lock_guard lock(mutex);
                alive = !exit.has_value();
            }
            if (alive)
                ::kill(-pid, SIGKILL);
            waiter.join();
        }
        if (stdinWrite >= 0)
            ::close(stdinWrite);
        if (stdoutRead >= 0)
            ::close(stdoutRead);
        if (stderrRead >= 0)
            ::close(stderrRead);
    }
};

namespace {

bool makePipe(int (&fds)[2])
{
    if (::pipe(fds) != 0)
        return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

}

Result<std::unique_ptr<Process>> Process::start(const ProcessSpec& spec, ProcessOutputCallback onStdout,
                                                ProcessOutputCallback onStderr, ProcessExitCallback onExit)
{
    auto resolved = findExecutable(spec.program);
    if (!resolved)
        return makeError("process.notfound", "Cannot find program: " + spec.program);

    int stdinPipe[2];
    int stdoutPipe[2];
    int stderrPipe[2] = {-1, -1};
    int errorPipe[2];
    if (!makePipe(stdinPipe) || !makePipe(stdoutPipe) || !makePipe(errorPipe)
        || (!spec.mergeStderr && !makePipe(stderrPipe)))
        return makeError("process.pipe", "Cannot create pipes");

    std::string programPath = paths::toUtf8(*resolved);
    std::vector<std::string> argumentStorage;
    argumentStorage.push_back(programPath);
    argumentStorage.insert(argumentStorage.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argumentStorage)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    std::map<std::string, std::string> merged;
    if (spec.inheritEnvironment)
        merged = currentEnvironment();
    for (const auto& [key, value] : spec.environment)
        merged[key] = value;
    std::vector<std::string> environmentStorage;
    for (const auto& [key, value] : merged)
        environmentStorage.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& entry : environmentStorage)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::string directory = paths::toUtf8(spec.workingDirectory);
    bool lowerPriority = spec.lowerPriority;
    bool mergeStderr = spec.mergeStderr;
    std::uint64_t memoryLimit = spec.memoryLimitBytes;

    pid_t pid = ::fork();
    if (pid < 0)
        return makeError("process.start", "Cannot start " + spec.program);
    if (pid == 0) {
        ::setsid();
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(mergeStderr ? stdoutPipe[1] : stderrPipe[1], STDERR_FILENO);
        if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
            int code = errno;
            auto ignored = ::write(errorPipe[1], &code, sizeof code);
            (void)ignored;
            ::_exit(127);
        }
        if (lowerPriority) {
            int ignored = ::nice(5);
            (void)ignored;
        }
#if defined(RLIMIT_AS)
        if (memoryLimit > 0) {
            rlimit limit{static_cast<rlim_t>(memoryLimit), static_cast<rlim_t>(memoryLimit)};
            ::setrlimit(RLIMIT_AS, &limit);
        }
#endif
        ::signal(SIGPIPE, SIG_DFL);
        ::execve(programPath.c_str(), argv.data(), envp.data());
        int code = errno;
        auto ignored = ::write(errorPipe[1], &code, sizeof code);
        (void)ignored;
        ::_exit(127);
    }

    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);
    ::close(errorPipe[1]);
    if (stderrPipe[1] >= 0)
        ::close(stderrPipe[1]);

    int execError = 0;
    auto bytes = ::read(errorPipe[0], &execError, sizeof execError);
    ::close(errorPipe[0]);
    if (bytes > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        if (stderrPipe[0] >= 0)
            ::close(stderrPipe[0]);
        return makeError("process.start", "Cannot start " + spec.program + ": " + std::strerror(execError));
    }

    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->stdinWrite = stdinPipe[1];
    impl->stdoutRead = stdoutPipe[0];
    impl->stderrRead = stderrPipe[0];
    impl->onStdout = std::move(onStdout);
    impl->onStderr = std::move(onStderr);
    impl->onExit = std::move(onExit);
    if (!spec.keepStdinOpen) {
        ::close(impl->stdinWrite);
        impl->stdinWrite = -1;
    }
    Impl* raw = impl.get();
    impl->openReaders = raw->stderrRead >= 0 ? 2 : 1;
    impl->stdoutThread = std::thread([raw] { raw->readLoop(raw->stdoutRead, raw->onStdout); });
    if (raw->stderrRead >= 0)
        impl->stderrThread = std::thread([raw] { raw->readLoop(raw->stderrRead, raw->onStderr); });
    impl->waiter = std::thread([raw] { raw->waitLoop(); });
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}

Status Process::writeStdin(std::string_view data)
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->stdinWrite < 0)
        return makeError("process.stdin", "The process input is closed");
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto written = ::write(impl_->stdinWrite, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return makeError("process.stdin", "Cannot write to the process");
        }
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

void Process::closeStdin()
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->stdinWrite >= 0) {
        ::close(impl_->stdinWrite);
        impl_->stdinWrite = -1;
    }
}

void Process::terminate()
{
    ::kill(-impl_->pid, SIGTERM);
    std::thread([pid = impl_->pid] {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        ::kill(-pid, SIGKILL);
    }).detach();
}

int Process::id() const
{
    return static_cast<int>(impl_->pid);
}

std::map<std::string, std::string> currentEnvironment()
{
    std::map<std::string, std::string> variables;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        std::string_view item(*entry);
        auto separator = item.find('=');
        if (separator == std::string_view::npos)
            continue;
        variables[std::string(item.substr(0, separator))] = std::string(item.substr(separator + 1));
    }
    return variables;
}

bool isProcessAlive(int processId)
{
    return ::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
}

int currentProcessId()
{
    return static_cast<int>(::getpid());
}

#else

struct Process::Impl {
    mutable std::mutex mutex;
    std::condition_variable finished;
    std::optional<int> exit;
};

Result<std::unique_ptr<Process>> Process::start(const ProcessSpec& spec, ProcessOutputCallback, ProcessOutputCallback,
                                                ProcessExitCallback)
{
    return makeError("process.unsupported", "This platform cannot start " + spec.program);
}

Status Process::writeStdin(std::string_view)
{
    return makeError("process.unsupported", "Processes are not available on this platform");
}

void Process::closeStdin() {}
void Process::terminate() {}
int Process::id() const { return -1; }

std::map<std::string, std::string> currentEnvironment()
{
    return {};
}

bool isProcessAlive(int)
{
    return false;
}

int currentProcessId()
{
    return 0;
}

#endif

Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Process::~Process() = default;

int Process::wait()
{
    std::unique_lock lock(impl_->mutex);
    impl_->finished.wait(lock, [this] { return impl_->exit.has_value(); });
    return *impl_->exit;
}

bool Process::waitFor(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->mutex);
    return impl_->finished.wait_for(lock, timeout, [this] { return impl_->exit.has_value(); });
}

bool Process::running() const
{
    std::lock_guard lock(impl_->mutex);
    return !impl_->exit.has_value();
}

std::optional<int> Process::exitCode() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->exit;
}

Result<RunResult> runProcess(const ProcessSpec& spec, std::chrono::milliseconds timeout)
{
    RunResult result;
    std::mutex outputMutex;
    auto started = Process::start(
        spec,
        [&](std::string_view data) {
            std::lock_guard lock(outputMutex);
            result.out.append(data);
        },
        [&](std::string_view data) {
            std::lock_guard lock(outputMutex);
            result.err.append(data);
        });
    if (!started)
        return started.error();
    auto& process = *started.value();
    if (timeout.count() > 0) {
        if (!process.waitFor(timeout)) {
            result.timedOut = true;
            process.terminate();
        }
    }
    result.exitCode = process.wait();
    return result;
}

std::optional<fs::path> findExecutable(std::string_view name, const std::vector<fs::path>& extraDirectories)
{
    std::error_code ec;
    fs::path candidate = paths::fromUtf8(name);

    std::vector<std::string> extensions = {""};
#if defined(_WIN32)
    extensions.clear();
    if (candidate.has_extension())
        extensions.emplace_back("");
    for (const auto& item : text::split(paths::env("PATHEXT").value_or(".COM;.EXE;.BAT;.CMD"), ';'))
        extensions.push_back(item);
#endif

    auto usable = [&](const fs::path& path) {
        if (!fs::is_regular_file(path, ec))
            return false;
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        return ::access(path.c_str(), X_OK) == 0;
#else
        return true;
#endif
    };

    if (candidate.is_absolute() || candidate.has_parent_path()) {
        for (const auto& extension : extensions) {
            fs::path path = candidate;
            path += extension;
            if (usable(path))
                return path;
        }
        return std::nullopt;
    }

    std::vector<fs::path> directories = extraDirectories;
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    for (const auto& item : text::split(paths::env("PATH").value_or(""), separator)) {
        if (!item.empty())
            directories.push_back(paths::fromUtf8(item));
    }
    for (const auto& directory : directories) {
        for (const auto& extension : extensions) {
            fs::path path = directory / candidate;
            path += extension;
            if (usable(path))
                return path;
        }
    }
    return std::nullopt;
}

}
