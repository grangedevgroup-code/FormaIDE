#include "forma/debugger/debug_client.h"

#include "forma/core/paths.h"
#include "forma/core/process.h"

namespace fs = std::filesystem;

namespace forma::debugger {

std::vector<AdapterInfo> knownAdapters()
{
    std::vector<AdapterInfo> adapters = {
        {"lldb", "LLDB", {"lldb-dap"}, "launch", "lldb-dap", "winget install LLVM.LLVM, or brew install llvm, or your package manager's lldb"},
        {"gdb", "GDB", {"gdb", "--interpreter=dap", "--quiet"}, "launch", "gdb", "Install GDB 14 or newer"},
        {"debugpy", "debugpy", {"python", "-m", "debugpy.adapter"}, "launch", "python", "pip install debugpy"},
        {"netcoredbg", "netcoredbg", {"netcoredbg", "--interpreter=vscode"}, "launch", "coreclr", "Install netcoredbg from github.com/Samsung/netcoredbg"},
        {"delve", "Delve", {"dlv", "dap"}, "launch", "go", "go install github.com/go-delve/delve/cmd/dlv@latest"},
    };
    for (auto& adapter : adapters) {
        std::vector<fs::path> extra;
#if defined(_WIN32)
        extra.push_back(paths::fromUtf8(paths::env("ProgramFiles").value_or("C:\\Program Files")) / "LLVM" / "bin");
#endif
        extra.push_back(paths::homeDir() / "go" / "bin");
        auto found = findExecutable(adapter.command[0], extra);
        if (!found && adapter.id == "lldb")
            found = findExecutable("lldb-vscode", extra);
        if (found) {
            adapter.command[0] = paths::toUtf8(*found);
            adapter.found = true;
        }
        if (adapter.id == "debugpy" && adapter.found) {
            ProcessSpec check;
            check.program = adapter.command[0];
            check.arguments = {"-c", "import debugpy"};
            check.origin = Origin::System;
            auto result = runProcess(check, std::chrono::seconds(10));
            adapter.found = result && result->exitCode == 0;
        }
    }
    return adapters;
}

std::optional<AdapterInfo> findAdapter(std::string_view id)
{
    for (auto& adapter : knownAdapters()) {
        if (adapter.id == id)
            return adapter;
    }
    return std::nullopt;
}

DebugSession::DebugSession() = default;

DebugSession::~DebugSession()
{
    stop();
}

void DebugSession::setState(SessionState state)
{
    state_ = state;
    if (callbacks_.onState)
        callbacks_.onState(state);
}

Json DebugSession::capabilities() const
{
    std::lock_guard lock(const_cast<std::mutex&>(mutex_));
    return capabilities_;
}

Status DebugSession::start(const AdapterInfo& adapter, const LaunchOptions& options, Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    launch_ = options;
    requestKind_ = adapter.request;
    initialized_ = false;
    ProcessSpec spec;
    spec.program = adapter.command[0];
    spec.arguments.assign(adapter.command.begin() + 1, adapter.command.end());
    spec.workingDirectory = options.workingDirectory;
    spec.origin = Origin::Build;
    setState(SessionState::Starting);
    auto status = process_.start(
        spec, [this](const Json& message) { handleMessage(message); },
        [this](int) {
            if (state_.load() != SessionState::Terminated)
                setState(SessionState::Terminated);
        },
        [this](std::string_view data) {
            if (callbacks_.onOutput)
                callbacks_.onOutput("adapter", data);
        });
    if (!status) {
        setState(SessionState::Terminated);
        return status;
    }
    Json arguments = Json{{"clientID", "formaide"}, {"clientName", "FormaIDE"}, {"adapterID", adapter.type}, {"pathFormat", "path"},
                          {"linesStartAt1", true}, {"columnsStartAt1", true}, {"supportsVariableType", true}, {"supportsRunInTerminalRequest", false},
                          {"locale", "en-US"}};
    request("initialize", arguments, [this](bool success, const Json& body, const std::string& message) {
        if (!success) {
            if (callbacks_.onOutput)
                callbacks_.onOutput("error", "The debug adapter refused to start: " + message + "\n");
            setState(SessionState::Terminated);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            capabilities_ = body;
        }
        Json launch = launch_.extra;
        if (!launch.is_object())
            launch = Json::object();
        launch["program"] = launch_.program;
        if (!launch_.arguments.empty())
            launch["args"] = launch_.arguments;
        launch["cwd"] = paths::toUtf8(launch_.workingDirectory);
        launch["stopOnEntry"] = launch_.stopOnEntry;
        if (!launch_.environment.empty())
            launch["env"] = launch_.environment;
        request(requestKind_, launch, [this](bool ok, const Json&, const std::string& reason) {
            if (!ok) {
                if (callbacks_.onOutput)
                    callbacks_.onOutput("error", "The program could not start under the debugger: " + reason + "\n");
                setState(SessionState::Terminated);
            }
        });
    });
    return {};
}

void DebugSession::stop()
{
    if (process_.running()) {
        request("disconnect", Json{{"terminateDebuggee", true}}, [](bool, const Json&, const std::string&) {});
    }
    process_.stop();
    if (state_.load() != SessionState::Idle && state_.load() != SessionState::Terminated)
        setState(SessionState::Terminated);
    std::map<int, ResponseHandler> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& [id, handler] : pending)
        handler(false, Json(), "The debug session ended");
}

void DebugSession::request(std::string_view command, Json arguments, ResponseHandler handler)
{
    int seq = seq_.fetch_add(1);
    {
        std::lock_guard lock(mutex_);
        pending_[seq] = std::move(handler);
    }
    Json message = Json::object();
    message["seq"] = seq;
    message["type"] = "request";
    message["command"] = std::string(command);
    message["arguments"] = std::move(arguments);
    auto status = process_.send(message);
    if (!status) {
        ResponseHandler failed;
        {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(seq);
            if (it != pending_.end()) {
                failed = std::move(it->second);
                pending_.erase(it);
            }
        }
        if (failed)
            failed(false, Json(), status.error().message);
    }
}

void DebugSession::handleMessage(const Json& message)
{
    std::string type = jsonString(message, "type");
    if (type == "response") {
        int seq = jsonInt(message, "request_seq");
        ResponseHandler handler;
        {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(seq);
            if (it == pending_.end())
                return;
            handler = std::move(it->second);
            pending_.erase(it);
        }
        handler(jsonBool(message, "success"), message.contains("body") ? message["body"] : Json(), jsonString(message, "message"));
    } else if (type == "event") {
        handleEvent(jsonString(message, "event"), message.contains("body") ? message["body"] : Json::object());
    } else if (type == "request") {
        Json response = Json::object();
        response["seq"] = seq_.fetch_add(1);
        response["type"] = "response";
        response["request_seq"] = jsonInt(message, "seq");
        response["command"] = jsonString(message, "command");
        response["success"] = false;
        response["message"] = "Not supported";
        auto status = process_.send(response);
        (void)status;
    }
}

void DebugSession::handleEvent(const std::string& event, const Json& body)
{
    if (event == "initialized") {
        initialized_ = true;
        flushBreakpoints();
        request("setExceptionBreakpoints", Json{{"filters", Json::array()}}, [](bool, const Json&, const std::string&) {});
        request("configurationDone", Json::object(), [](bool, const Json&, const std::string&) {});
        setState(SessionState::Running);
    } else if (event == "stopped") {
        StopEvent stop;
        stop.reason = jsonString(body, "reason");
        stop.threadId = jsonInt(body, "threadId");
        stop.description = jsonString(body, "description", jsonString(body, "text"));
        stop.allThreadsStopped = jsonBool(body, "allThreadsStopped", true);
        setState(SessionState::Stopped);
        if (callbacks_.onStopped)
            callbacks_.onStopped(stop);
    } else if (event == "continued") {
        setState(SessionState::Running);
    } else if (event == "output") {
        if (callbacks_.onOutput)
            callbacks_.onOutput(jsonString(body, "category", "console"), jsonString(body, "output"));
    } else if (event == "exited") {
        if (callbacks_.onExited)
            callbacks_.onExited(jsonInt(body, "exitCode"));
    } else if (event == "terminated") {
        setState(SessionState::Terminated);
    }
}

void DebugSession::flushBreakpoints()
{
    std::map<std::string, std::vector<Breakpoint>> all;
    {
        std::lock_guard lock(mutex_);
        all = breakpoints_;
    }
    for (const auto& [path, list] : all)
        setBreakpoints(paths::fromUtf8(path), list);
}

void DebugSession::setBreakpoints(const fs::path& path, const std::vector<Breakpoint>& breakpoints)
{
    std::string key = paths::toUtf8(path);
    {
        std::lock_guard lock(mutex_);
        breakpoints_[key] = breakpoints;
    }
    if (!initialized_)
        return;
    Json lines = Json::array();
    for (const auto& breakpoint : breakpoints) {
        Json item = Json{{"line", breakpoint.line}};
        if (!breakpoint.condition.empty())
            item["condition"] = breakpoint.condition;
        lines.push_back(item);
    }
    Json arguments = Json{{"source", {{"path", key}, {"name", paths::toUtf8(path.filename())}}}, {"breakpoints", lines}};
    request("setBreakpoints", arguments, [this, key, breakpoints](bool success, const Json& body, const std::string&) {
        if (!success || !callbacks_.onBreakpoints)
            return;
        std::vector<Breakpoint> result = breakpoints;
        if (body.contains("breakpoints") && body["breakpoints"].is_array()) {
            const Json& list = body["breakpoints"];
            for (std::size_t i = 0; i < list.size() && i < result.size(); ++i) {
                result[i].verified = jsonBool(list[i], "verified");
                result[i].id = jsonInt(list[i], "id");
                if (list[i].contains("line"))
                    result[i].line = jsonInt(list[i], "line");
            }
        }
        callbacks_.onBreakpoints(key, result);
    });
}

void DebugSession::setExceptionBreakpoints(const std::vector<std::string>& filters)
{
    request("setExceptionBreakpoints", Json{{"filters", filters}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::continueExecution(int threadId)
{
    setState(SessionState::Running);
    request("continue", Json{{"threadId", threadId}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::pause(int threadId)
{
    request("pause", Json{{"threadId", threadId}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::next(int threadId)
{
    setState(SessionState::Running);
    request("next", Json{{"threadId", threadId}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::stepIn(int threadId)
{
    setState(SessionState::Running);
    request("stepIn", Json{{"threadId", threadId}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::stepOut(int threadId)
{
    setState(SessionState::Running);
    request("stepOut", Json{{"threadId", threadId}}, [](bool, const Json&, const std::string&) {});
}

void DebugSession::threads(Reply<std::vector<ThreadInfo>> reply)
{
    request("threads", Json::object(), [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.threads", message));
            return;
        }
        std::vector<ThreadInfo> list;
        if (body.contains("threads")) {
            for (const auto& item : body["threads"])
                list.push_back({jsonInt(item, "id"), jsonString(item, "name")});
        }
        reply(std::move(list));
    });
}

void DebugSession::stackTrace(int threadId, Reply<std::vector<StackFrame>> reply)
{
    request("stackTrace", Json{{"threadId", threadId}, {"startFrame", 0}, {"levels", 100}}, [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.stack", message));
            return;
        }
        std::vector<StackFrame> frames;
        if (body.contains("stackFrames")) {
            for (const auto& item : body["stackFrames"]) {
                StackFrame frame;
                frame.id = jsonInt(item, "id");
                frame.name = jsonString(item, "name");
                frame.line = jsonInt(item, "line");
                frame.column = jsonInt(item, "column");
                if (item.contains("source"))
                    frame.path = jsonString(item["source"], "path");
                frames.push_back(std::move(frame));
            }
        }
        reply(std::move(frames));
    });
}

void DebugSession::scopes(int frameId, Reply<std::vector<Scope>> reply)
{
    request("scopes", Json{{"frameId", frameId}}, [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.scopes", message));
            return;
        }
        std::vector<Scope> list;
        if (body.contains("scopes")) {
            for (const auto& item : body["scopes"])
                list.push_back({jsonString(item, "name"), jsonInt(item, "variablesReference"), jsonBool(item, "expensive")});
        }
        reply(std::move(list));
    });
}

void DebugSession::variables(int reference, Reply<std::vector<Variable>> reply)
{
    request("variables", Json{{"variablesReference", reference}}, [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.variables", message));
            return;
        }
        std::vector<Variable> list;
        if (body.contains("variables")) {
            for (const auto& item : body["variables"])
                list.push_back({jsonString(item, "name"), jsonString(item, "value"), jsonString(item, "type"), jsonInt(item, "variablesReference")});
        }
        reply(std::move(list));
    });
}

void DebugSession::evaluate(std::string_view expression, int frameId, Reply<std::string> reply)
{
    Json arguments = Json{{"expression", std::string(expression)}, {"context", "watch"}};
    if (frameId != 0)
        arguments["frameId"] = frameId;
    request("evaluate", arguments, [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.evaluate", message));
            return;
        }
        reply(jsonString(body, "result"));
    });
}

void DebugSession::readMemory(std::string_view reference, int count, Reply<std::string> reply)
{
    request("readMemory", Json{{"memoryReference", std::string(reference)}, {"count", count}}, [reply = std::move(reply)](bool success, const Json& body, const std::string& message) {
        if (!success) {
            reply(makeError("dap.memory", message));
            return;
        }
        reply(jsonString(body, "data"));
    });
}

}
