#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"
#include "forma/terminal/terminal.h"

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace fs = std::filesystem;

namespace forma::terminal {

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

}

struct Pty::Impl {
    HPCON console = nullptr;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::thread reader;
    std::thread waiter;
    std::mutex mutex;
    std::optional<int> exit;
    std::function<void(std::string_view)> onOutput;
    std::function<void(int)> onExit;

    ~Impl()
    {
        if (process != nullptr)
            TerminateProcess(process, 1);
        if (job != nullptr)
            TerminateJobObject(job, 1);
        if (waiter.joinable())
            waiter.join();
        if (console != nullptr) {
            ClosePseudoConsole(console);
            console = nullptr;
        }
        if (inputWrite != nullptr)
            CloseHandle(inputWrite);
        if (reader.joinable()) {
            CancelSynchronousIo(reader.native_handle());
            reader.join();
        }
        if (outputRead != nullptr)
            CloseHandle(outputRead);
        if (process != nullptr)
            CloseHandle(process);
        if (job != nullptr)
            CloseHandle(job);
    }
};

Result<std::unique_ptr<Pty>> Pty::start(const Options& options, std::function<void(std::string_view)> onOutput, std::function<void(int)> onExit)
{
    auto program = findExecutable(options.program);
    if (!program)
        return makeError("terminal.notfound", "Cannot find " + options.program);

    HANDLE inputRead = nullptr;
    HANDLE inputWrite = nullptr;
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&inputRead, &inputWrite, nullptr, 0) || !CreatePipe(&outputRead, &outputWrite, nullptr, 0))
        return makeError("terminal.pipe", "Cannot create pipes");
    HPCON console = nullptr;
    COORD size{static_cast<SHORT>(options.columns), static_cast<SHORT>(options.rows)};
    HRESULT created = CreatePseudoConsole(size, inputRead, outputWrite, 0, &console);
    CloseHandle(inputRead);
    CloseHandle(outputWrite);
    if (FAILED(created)) {
        CloseHandle(inputWrite);
        CloseHandle(outputRead);
        return makeError("terminal.conpty", "This version of Windows has no pseudo console support");
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> storage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes;

    std::string line = quoteWindowsArgument(paths::toUtf8(*program));
    for (const auto& argument : options.arguments) {
        line.push_back(' ');
        line += quoteWindowsArgument(argument);
    }
    std::wstring command = wide(line);

    auto merged = currentEnvironment();
    merged["TERM"] = "xterm-256color";
    merged["COLORTERM"] = "truecolor";
    for (const auto& [key, value] : options.environment)
        merged[key] = value;
    std::vector<std::pair<std::wstring, std::wstring>> sorted;
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

    std::wstring directory = options.workingDirectory.empty() ? std::wstring() : options.workingDirectory.wstring();
    PROCESS_INFORMATION info{};
    BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                  block.data(), directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
        ClosePseudoConsole(console);
        CloseHandle(inputWrite);
        CloseHandle(outputRead);
        return makeError("terminal.start", "Cannot start " + options.program);
    }
    auto impl = std::make_unique<Impl>();
    impl->job = CreateJobObjectW(nullptr, nullptr);
    if (impl->job != nullptr)
        AssignProcessToJobObject(impl->job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    impl->console = console;
    impl->inputWrite = inputWrite;
    impl->outputRead = outputRead;
    impl->process = info.hProcess;
    impl->onOutput = std::move(onOutput);
    impl->onExit = std::move(onExit);
    Impl* raw = impl.get();
    impl->reader = std::thread([raw] {
        char buffer[8192];
        DWORD count = 0;
        while (ReadFile(raw->outputRead, buffer, sizeof buffer, &count, nullptr) && count > 0) {
            if (raw->onOutput)
                raw->onOutput(std::string_view(buffer, count));
        }
    });
    impl->waiter = std::thread([raw] {
        WaitForSingleObject(raw->process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(raw->process, &code);
        {
            std::lock_guard lock(raw->mutex);
            raw->exit = static_cast<int>(code);
        }
        if (raw->onExit)
            raw->onExit(static_cast<int>(code));
    });
    return std::unique_ptr<Pty>(new Pty(std::move(impl)));
}

Status Pty::write(std::string_view data)
{
    DWORD written = 0;
    if (!WriteFile(impl_->inputWrite, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
        return makeError("terminal.write", "The terminal is closed");
    return {};
}

void Pty::resize(int columns, int rows)
{
    if (impl_->console != nullptr)
        ResizePseudoConsole(impl_->console, COORD{static_cast<SHORT>(columns), static_cast<SHORT>(rows)});
}

void Pty::terminate()
{
    if (impl_->job != nullptr)
        TerminateJobObject(impl_->job, 1);
    else
        TerminateProcess(impl_->process, 1);
}

#elif !defined(__EMSCRIPTEN__)

struct Pty::Impl {
    int master = -1;
    pid_t pid = -1;
    int wake[2] = {-1, -1};
    std::thread reader;
    std::thread waiter;
    std::mutex mutex;
    std::optional<int> exit;
    std::function<void(std::string_view)> onOutput;
    std::function<void(int)> onExit;

    ~Impl()
    {
        if (pid > 0)
            ::kill(-pid, SIGHUP);
        if (wake[1] >= 0) {
            auto ignored = ::write(wake[1], "x", 1);
            (void)ignored;
        }
        if (reader.joinable())
            reader.join();
        if (pid > 0) {
            bool finished;
            {
                std::lock_guard lock(mutex);
                finished = exit.has_value();
            }
            if (!finished)
                ::kill(-pid, SIGKILL);
        }
        if (waiter.joinable())
            waiter.join();
        if (master >= 0)
            ::close(master);
        if (wake[0] >= 0)
            ::close(wake[0]);
        if (wake[1] >= 0)
            ::close(wake[1]);
    }
};

Result<std::unique_ptr<Pty>> Pty::start(const Options& options, std::function<void(std::string_view)> onOutput, std::function<void(int)> onExit)
{
    auto program = findExecutable(options.program);
    if (!program)
        return makeError("terminal.notfound", "Cannot find " + options.program);
    std::string programPath = paths::toUtf8(*program);
    std::vector<std::string> argumentStorage = {programPath};
    argumentStorage.insert(argumentStorage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argumentStorage)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    auto merged = currentEnvironment();
    merged["TERM"] = "xterm-256color";
    merged["COLORTERM"] = "truecolor";
    for (const auto& [key, value] : options.environment)
        merged[key] = value;
    std::vector<std::string> environmentStorage;
    for (const auto& [key, value] : merged)
        environmentStorage.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& entry : environmentStorage)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::string directory = paths::toUtf8(options.workingDirectory);

    winsize size{};
    size.ws_col = static_cast<unsigned short>(options.columns);
    size.ws_row = static_cast<unsigned short>(options.rows);
    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &size);
    if (pid < 0)
        return makeError("terminal.start", "Cannot create a pseudo terminal");
    if (pid == 0) {
        if (!directory.empty() && ::chdir(directory.c_str()) != 0)
            ::_exit(127);
        ::execve(programPath.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    auto impl = std::make_unique<Impl>();
    impl->master = master;
    impl->pid = pid;
    if (::pipe(impl->wake) != 0)
        return makeError("terminal.pipe", "Cannot create pipes");
    impl->onOutput = std::move(onOutput);
    impl->onExit = std::move(onExit);
    Impl* raw = impl.get();
    impl->reader = std::thread([raw] {
        char buffer[8192];
        pollfd items[2] = {{raw->master, POLLIN, 0}, {raw->wake[0], POLLIN, 0}};
        while (true) {
            int ready = ::poll(items, 2, -1);
            if (ready < 0 && errno != EINTR)
                return;
            if (ready <= 0)
                continue;
            if (items[1].revents != 0)
                return;
            auto count = ::read(raw->master, buffer, sizeof buffer);
            if (count <= 0) {
                if (count < 0 && errno == EINTR)
                    continue;
                return;
            }
            if (raw->onOutput)
                raw->onOutput(std::string_view(buffer, static_cast<std::size_t>(count)));
        }
    });
    impl->waiter = std::thread([raw] {
        int status = 0;
        while (::waitpid(raw->pid, &status, 0) < 0 && errno == EINTR) {
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        {
            std::lock_guard lock(raw->mutex);
            raw->exit = code;
        }
        if (raw->onExit)
            raw->onExit(code);
    });
    return std::unique_ptr<Pty>(new Pty(std::move(impl)));
}

Status Pty::write(std::string_view data)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto written = ::write(impl_->master, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return makeError("terminal.write", "The terminal is closed");
        }
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

void Pty::resize(int columns, int rows)
{
    winsize size{};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(impl_->master, TIOCSWINSZ, &size);
}

void Pty::terminate()
{
    ::kill(-impl_->pid, SIGHUP);
}

#else

struct Pty::Impl {
    std::mutex mutex;
    std::optional<int> exit;
};

Result<std::unique_ptr<Pty>> Pty::start(const Options&, std::function<void(std::string_view)>, std::function<void(int)>)
{
    return makeError("terminal.unsupported", "Terminals are not available on this platform");
}

Status Pty::write(std::string_view)
{
    return makeError("terminal.unsupported", "Terminals are not available on this platform");
}

void Pty::resize(int, int) {}
void Pty::terminate() {}

#endif

Pty::Pty(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Pty::~Pty() = default;

bool Pty::running() const
{
    std::lock_guard lock(impl_->mutex);
    return !impl_->exit.has_value();
}

std::optional<int> Pty::exitCode() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->exit;
}

std::vector<ShellInfo> availableShells()
{
    std::vector<ShellInfo> shells;
    auto add = [&](const char* id, const char* name, const char* program, std::vector<std::string> arguments = {}) {
        if (auto found = findExecutable(program))
            shells.push_back({id, name, paths::toUtf8(*found), std::move(arguments)});
    };
#if defined(_WIN32)
    add("pwsh", "PowerShell 7", "pwsh", {"-NoLogo"});
    add("powershell", "Windows PowerShell", "powershell", {"-NoLogo"});
    add("cmd", "Command Prompt", "cmd");
    std::error_code ec;
    fs::path gitBash = paths::fromUtf8(paths::env("ProgramFiles").value_or("C:\\Program Files")) / "Git" / "bin" / "bash.exe";
    if (fs::exists(gitBash, ec))
        shells.push_back({"gitbash", "Git Bash", paths::toUtf8(gitBash), {"--login", "-i"}});
    add("wsl", "WSL", "wsl");
#else
    if (auto shell = paths::env("SHELL")) {
        std::string name = shell->substr(shell->find_last_of('/') + 1);
        shells.push_back({name, name, *shell, {"-l"}});
    }
    for (const char* name : {"bash", "zsh", "fish", "sh"}) {
        if (std::none_of(shells.begin(), shells.end(), [&](const ShellInfo& item) { return item.id == name; }))
            add(name, name, name, {"-l"});
    }
#endif
    return shells;
}

}
