#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"
#include "forma/core/process.h"

namespace forma {

class ContentLengthParser {
public:
    std::vector<std::string> feed(std::string_view data);

private:
    std::string buffer_;
};

std::string frameContentLength(std::string_view body);

class FramedJsonProcess {
public:
    using MessageCallback = std::function<void(const Json&)>;
    using ExitCallback = std::function<void(int)>;
    using LogCallback = std::function<void(std::string_view)>;

    FramedJsonProcess() = default;
    ~FramedJsonProcess();

    Status start(const ProcessSpec& spec, MessageCallback onMessage, ExitCallback onExit = {}, LogCallback onLog = {});
    Status send(const Json& message);
    void stop();
    bool running() const;

private:
    std::unique_ptr<Process> process_;
    ContentLengthParser parser_;
    std::mutex writeMutex_;
    MessageCallback onMessage_;
    ExitCallback onExit_;
    LogCallback onLog_;
};

}
