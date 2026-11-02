#include "forma/core/framed_json.h"

#include "forma/core/text.h"

namespace forma {

std::vector<std::string> ContentLengthParser::feed(std::string_view data)
{
    buffer_.append(data);
    std::vector<std::string> messages;
    while (true) {
        auto headerEnd = buffer_.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
            break;
        std::size_t length = 0;
        bool found = false;
        for (const auto& line : text::splitLines(std::string_view(buffer_).substr(0, headerEnd))) {
            if (text::startsWith(text::toLower(line), "content-length:")) {
                try {
                    length = static_cast<std::size_t>(std::stoull(text::trim(line.substr(15))));
                    found = true;
                } catch (const std::exception&) {
                    found = false;
                }
            }
        }
        if (!found) {
            buffer_.erase(0, headerEnd + 4);
            continue;
        }
        if (buffer_.size() < headerEnd + 4 + length)
            break;
        messages.push_back(buffer_.substr(headerEnd + 4, length));
        buffer_.erase(0, headerEnd + 4 + length);
    }
    return messages;
}

std::string frameContentLength(std::string_view body)
{
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

FramedJsonProcess::~FramedJsonProcess()
{
    stop();
}

Status FramedJsonProcess::start(const ProcessSpec& spec, MessageCallback onMessage, ExitCallback onExit, LogCallback onLog)
{
    onMessage_ = std::move(onMessage);
    onExit_ = std::move(onExit);
    onLog_ = std::move(onLog);
    ProcessSpec actual = spec;
    actual.keepStdinOpen = true;
    auto started = Process::start(
        actual,
        [this](std::string_view data) {
            for (const auto& body : parser_.feed(data)) {
                auto parsed = parseJson(body);
                if (parsed && onMessage_)
                    onMessage_(*parsed);
            }
        },
        [this](std::string_view data) {
            if (onLog_)
                onLog_(data);
        },
        [this](int code) {
            if (onExit_)
                onExit_(code);
        });
    if (!started)
        return started.error();
    process_ = std::move(started.value());
    return {};
}

Status FramedJsonProcess::send(const Json& message)
{
    std::lock_guard lock(writeMutex_);
    if (!process_)
        return makeError("rpc.closed", "The process is not running");
    return process_->writeStdin(frameContentLength(dumpJsonCompact(message)));
}

void FramedJsonProcess::stop()
{
    if (!process_)
        return;
    process_->closeStdin();
    if (!process_->waitFor(std::chrono::milliseconds(500)))
        process_->terminate();
    process_->wait();
    process_.reset();
}

bool FramedJsonProcess::running() const
{
    return process_ && process_->running();
}

}
