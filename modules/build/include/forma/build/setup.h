#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "forma/build/toolchain.h"
#include "forma/core/process.h"
#include "forma/core/result.h"

namespace forma::build {

struct SetupStep {
    std::string label;
    ProcessSpec process;
    std::string input;
    std::vector<int> successCodes = {0};
    std::function<Status()> action;
};

struct InstallPlan {
    std::string id;
    std::string name;
    std::vector<SetupStep> steps;
    std::vector<std::string> dependencies;
    bool mayAskForAdmin = false;
    std::string note;
    std::string manualUrl;

    bool automatic() const { return !steps.empty(); }
    std::string preview() const;
};

struct ToolStack {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> required;
    std::vector<std::string> optional;
    std::vector<std::string> platforms;
};

const std::vector<ToolStack>& toolStacks();
std::vector<ToolStack> toolStacksForHost();
const ToolStack* findToolStack(std::string_view id);

InstallPlan planToolInstall(std::string_view toolId, ToolchainDetector& tools);
InstallPlan planCommandInstall(std::string_view name, std::string_view command);
InstallPlan planServerInstall(std::string_view languageName, std::string_view serverCommand, std::string_view installHint);
std::vector<std::string> installOrder(const std::vector<std::string>& toolIds, ToolchainDetector& tools);

std::filesystem::path managedToolsDir();
void refreshEnvironmentFromSystem();

struct SetupCallbacks {
    std::function<void(std::string_view label)> onStep;
    std::function<void(std::string_view text, bool isError)> onOutput;
    std::function<void(bool success, std::string_view message)> onFinished;
};

class SetupJob {
public:
    SetupJob();
    ~SetupJob();
    SetupJob(const SetupJob&) = delete;
    SetupJob& operator=(const SetupJob&) = delete;

    Status start(InstallPlan plan, SetupCallbacks callbacks);
    void cancel();
    void wait();
    bool running() const { return running_.load(); }

private:
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    Process* current_ = nullptr;
};

struct AndroidDevice {
    std::string serial;
    std::string label;
    bool ready = false;
};

struct RunDevice {
    std::string id;
    std::string label;
    std::string kind;
    bool ready = true;
};

std::optional<std::filesystem::path> adbPath(ToolchainDetector& tools);
std::optional<std::filesystem::path> emulatorPath(ToolchainDetector& tools);
std::vector<AndroidDevice> androidDevices(ToolchainDetector& tools);
std::vector<std::string> androidVirtualDevices(ToolchainDetector& tools);
std::string androidAcceleration(ToolchainDetector& tools);
std::string androidPackage(const std::filesystem::path& projectDirectory);
std::vector<RunDevice> androidRunDevices(ToolchainDetector& tools);
int freeEmulatorPort(ToolchainDetector& tools);

}
