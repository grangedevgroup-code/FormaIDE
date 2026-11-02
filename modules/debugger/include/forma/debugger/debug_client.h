#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/framed_json.h"

namespace forma::debugger {

struct AdapterInfo {
    std::string id;
    std::string name;
    std::vector<std::string> command;
    std::string request = "launch";
    std::string type;
    std::string install;
    bool found = false;
};

std::vector<AdapterInfo> knownAdapters();
std::optional<AdapterInfo> findAdapter(std::string_view id);

struct Breakpoint {
    int line = 0;
    std::string condition;
    bool verified = false;
    int id = 0;
};

struct StackFrame {
    int id = 0;
    std::string name;
    std::string path;
    int line = 0;
    int column = 0;
};

struct Scope {
    std::string name;
    int variablesReference = 0;
    bool expensive = false;
};

struct Variable {
    std::string name;
    std::string value;
    std::string type;
    int variablesReference = 0;
};

struct ThreadInfo {
    int id = 0;
    std::string name;
};

struct StopEvent {
    std::string reason;
    int threadId = 0;
    std::string description;
    bool allThreadsStopped = true;
};

enum class SessionState { Idle, Starting, Running, Stopped, Terminated };

struct LaunchOptions {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path workingDirectory;
    std::map<std::string, std::string> environment;
    bool stopOnEntry = false;
    Json extra = Json::object();
};

class DebugSession {
public:
    struct Callbacks {
        std::function<void(SessionState)> onState;
        std::function<void(const StopEvent&)> onStopped;
        std::function<void(std::string_view category, std::string_view text)> onOutput;
        std::function<void(int exitCode)> onExited;
        std::function<void(const std::string& path, const std::vector<Breakpoint>&)> onBreakpoints;
    };

    template <class T>
    using Reply = std::function<void(Result<T>)>;

    DebugSession();
    ~DebugSession();

    Status start(const AdapterInfo& adapter, const LaunchOptions& options, Callbacks callbacks);
    void stop();
    SessionState state() const { return state_.load(); }
    Json capabilities() const;

    void setBreakpoints(const std::filesystem::path& path, const std::vector<Breakpoint>& breakpoints);

    void continueExecution(int threadId);
    void pause(int threadId);
    void next(int threadId);
    void stepIn(int threadId);
    void stepOut(int threadId);

    void threads(Reply<std::vector<ThreadInfo>> reply);
    void stackTrace(int threadId, Reply<std::vector<StackFrame>> reply);
    void scopes(int frameId, Reply<std::vector<Scope>> reply);
    void variables(int reference, Reply<std::vector<Variable>> reply);
    void evaluate(std::string_view expression, int frameId, Reply<std::string> reply);
    void readMemory(std::string_view reference, int count, Reply<std::string> reply);
    void setExceptionBreakpoints(const std::vector<std::string>& filters);

private:
    using ResponseHandler = std::function<void(bool success, const Json& body, const std::string& message)>;

    void request(std::string_view command, Json arguments, ResponseHandler handler);
    void handleMessage(const Json& message);
    void handleEvent(const std::string& event, const Json& body);
    void flushBreakpoints();
    void setState(SessionState state);

    FramedJsonProcess process_;
    std::atomic<int> seq_{1};
    std::atomic<SessionState> state_{SessionState::Idle};
    std::mutex mutex_;
    std::map<int, ResponseHandler> pending_;
    std::map<std::string, std::vector<Breakpoint>> breakpoints_;
    Json capabilities_;
    Callbacks callbacks_;
    LaunchOptions launch_;
    std::string requestKind_;
    bool initialized_ = false;
};

}
