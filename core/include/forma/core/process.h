#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/filesystem_service.h"
#include "forma/core/result.h"

namespace forma {

struct ProcessSpec {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::map<std::string, std::string> environment;
    bool inheritEnvironment = true;
    bool mergeStderr = false;
    bool keepStdinOpen = false;
    bool lowerPriority = false;
    std::uint64_t memoryLimitBytes = 0;
    Origin origin = Origin::User;
    std::string label;
};

using ProcessOutputCallback = std::function<void(std::string_view)>;
using ProcessExitCallback = std::function<void(int exitCode)>;

class Process {
public:
    static Result<std::unique_ptr<Process>> start(const ProcessSpec& spec, ProcessOutputCallback onStdout,
                                                  ProcessOutputCallback onStderr = {},
                                                  ProcessExitCallback onExit = {});
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Status writeStdin(std::string_view data);
    void closeStdin();
    void terminate();
    int wait();
    bool waitFor(std::chrono::milliseconds timeout);
    bool running() const;
    std::optional<int> exitCode() const;
    int id() const;

    struct Impl;

private:
    explicit Process(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct RunResult {
    int exitCode = -1;
    std::string out;
    std::string err;
    bool timedOut = false;
};

Result<RunResult> runProcess(const ProcessSpec& spec, std::chrono::milliseconds timeout = std::chrono::minutes(5));

std::optional<std::filesystem::path> findExecutable(std::string_view name,
                                                    const std::vector<std::filesystem::path>& extraDirectories = {});
std::map<std::string, std::string> currentEnvironment();
bool isProcessAlive(int processId);
int currentProcessId();

std::string quoteWindowsArgument(std::string_view argument);
std::string buildWindowsBatchCommand(std::string_view program, const std::vector<std::string>& arguments);

}
