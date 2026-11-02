#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"

namespace forma::ai {

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};

class IHttpCall {
public:
    virtual ~IHttpCall() = default;
    virtual void cancel() = 0;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual std::shared_ptr<IHttpCall> send(const HttpRequest& request, std::function<void(std::string_view)> onChunk,
                                            std::function<void(int status, const std::string& error)> onDone) = 0;
};

class SseParser {
public:
    using Handler = std::function<void(const std::string& event, const std::string& data)>;
    void feed(std::string_view data, const Handler& handler);

private:
    std::string buffer_;
    std::string event_;
    std::string data_;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    Json schema;
};

struct ToolCall {
    std::string id;
    std::string name;
    Json input;
    bool inputValid = true;
};

struct ChatMessage {
    std::string role;
    std::string text;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;
    bool isError = false;
    Json raw;
};

struct ChatRequest {
    std::string system;
    std::vector<ChatMessage> messages;
    std::vector<ToolDefinition> tools;
};

struct ChatResult {
    ChatMessage message;
    std::string stopReason;
    int inputTokens = 0;
    int outputTokens = 0;
    std::string servedModel;
};

struct ChatHandlers {
    std::function<void(std::string_view)> onText;
    std::function<void(const ChatResult&)> onDone;
    std::function<void(const Error&)> onError;
};

struct ProviderConfig {
    std::string id;
    std::string label;
    std::string kind = "openai";
    std::string baseUrl;
    std::string model;
    std::string credentialAccount;
    int contextSize = 0;
    int maxOutputTokens = 0;
    bool local = false;
};

class IAIProvider {
public:
    virtual ~IAIProvider() = default;
    virtual const ProviderConfig& config() const = 0;
    virtual std::shared_ptr<IHttpCall> chat(const ChatRequest& request, ChatHandlers handlers) = 0;
    virtual std::shared_ptr<IHttpCall> listModels(std::function<void(Result<std::vector<std::string>>)> reply) = 0;
};

std::unique_ptr<IAIProvider> createProvider(ProviderConfig config, std::string apiKey, std::shared_ptr<IHttpTransport> transport);
std::vector<ProviderConfig> providerPresets();
Json providerConfigToJson(const ProviderConfig& config);
ProviderConfig providerConfigFromJson(const Json& json);

}
