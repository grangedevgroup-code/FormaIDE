#include "forma/ai/provider.h"

#include <algorithm>
#include <mutex>

#include "forma/core/text.h"

namespace forma::ai {

void SseParser::feed(std::string_view chunk, const Handler& handler)
{
    buffer_.append(chunk);
    std::size_t start = 0;
    while (true) {
        auto newline = buffer_.find('\n', start);
        if (newline == std::string::npos)
            break;
        std::string line = buffer_.substr(start, newline - start);
        start = newline + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) {
            if (!data_.empty() || !event_.empty())
                handler(event_, data_);
            event_.clear();
            data_.clear();
        } else if (text::startsWith(line, "event:")) {
            event_ = text::trim(std::string_view(line).substr(6));
        } else if (text::startsWith(line, "data:")) {
            std::string_view value = std::string_view(line).substr(5);
            if (!value.empty() && value.front() == ' ')
                value.remove_prefix(1);
            if (!data_.empty())
                data_.push_back('\n');
            data_.append(value);
        }
    }
    buffer_.erase(0, start);
}

namespace {

std::string errorFromBody(int status, const std::string& body)
{
    auto parsed = parseJson(body);
    if (parsed && parsed->is_object() && parsed->contains("error")) {
        const Json& error = (*parsed)["error"];
        if (error.is_object())
            return "The provider answered " + std::to_string(status) + ": " + jsonString(error, "message", dumpJsonCompact(error));
        if (error.is_string())
            return "The provider answered " + std::to_string(status) + ": " + error.get<std::string>();
    }
    if (status == 401 || status == 403)
        return "The provider refused the API key (status " + std::to_string(status) + "). Check it in Settings, Assistant.";
    return "The provider answered with status " + std::to_string(status) + (body.empty() ? std::string() : ": " + body.substr(0, 400));
}

class Collector : public std::enable_shared_from_this<Collector> {
public:
    std::string body;
};

class AnthropicProvider final : public IAIProvider {
public:
    AnthropicProvider(ProviderConfig config, std::string key, std::shared_ptr<IHttpTransport> transport)
        : config_(std::move(config)), key_(std::move(key)), transport_(std::move(transport))
    {
    }

    const ProviderConfig& config() const override { return config_; }

    std::shared_ptr<IHttpCall> listModels(std::function<void(Result<std::vector<std::string>>)> reply) override
    {
        HttpRequest request;
        request.method = "GET";
        request.url = base() + "/v1/models?limit=100";
        request.headers = headers();
        auto collected = std::make_shared<std::string>();
        return transport_->send(request, [collected](std::string_view data) { collected->append(data); },
                                [collected, reply = std::move(reply)](int status, const std::string& error) {
                                    if (!error.empty()) {
                                        reply(makeError("ai.network", error));
                                        return;
                                    }
                                    if (status != 200) {
                                        reply(makeError("ai.models", errorFromBody(status, *collected)));
                                        return;
                                    }
                                    std::vector<std::string> models;
                                    auto parsed = parseJson(*collected);
                                    if (parsed && parsed->contains("data")) {
                                        for (const auto& item : (*parsed)["data"])
                                            models.push_back(jsonString(item, "id"));
                                    }
                                    reply(std::move(models));
                                });
    }

    std::shared_ptr<IHttpCall> chat(const ChatRequest& chatRequest, ChatHandlers handlers) override
    {
        Json body = Json::object();
        body["model"] = config_.model;
        body["max_tokens"] = config_.maxOutputTokens > 0 ? config_.maxOutputTokens : 64000;
        body["stream"] = true;
        if (!chatRequest.system.empty())
            body["system"] = chatRequest.system;
        const std::string& model = config_.model;
        bool opus55 = model == "claude-opus-5-5";
        bool explicitAdaptive = text::startsWith(model, "claude-opus-4-8") || text::startsWith(model, "claude-opus-4-7") || text::startsWith(model, "claude-opus-4-6") || text::startsWith(model, "claude-sonnet-4-6");
        if (explicitAdaptive)
            body["thinking"] = Json{{"type", "adaptive"}};
        if (opus55)
            body["output_config"] = Json{{"effort", "high"}};
        Json messages = Json::array();
        for (const auto& message : chatRequest.messages) {
            if (message.role == "tool") {
                Json block = Json{{"type", "tool_result"}, {"tool_use_id", message.toolCallId}, {"content", message.text}};
                if (message.isError)
                    block["is_error"] = true;
                if (!messages.empty() && messages.back()["role"] == "user" && messages.back()["content"].is_array()
                    && !messages.back()["content"].empty() && messages.back()["content"].back().value("type", "") == "tool_result") {
                    messages.back()["content"].push_back(block);
                } else {
                    messages.push_back(Json{{"role", "user"}, {"content", Json::array({block})}});
                }
            } else if (message.role == "assistant" && !message.raw.is_null()) {
                messages.push_back(Json{{"role", "assistant"}, {"content", message.raw}});
            } else {
                messages.push_back(Json{{"role", message.role}, {"content", message.text}});
            }
        }
        body["messages"] = messages;
        if (!chatRequest.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : chatRequest.tools)
                tools.push_back(Json{{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.schema}, {"eager_input_streaming", true}});
            body["tools"] = tools;
        }
        HttpRequest request;
        request.url = base() + "/v1/messages";
        request.headers = headers();
        request.headers["accept"] = "text/event-stream";
        static const std::vector<std::string> fallbackModels = {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"};
        if (std::find(fallbackModels.begin(), fallbackModels.end(), model) != fallbackModels.end() && text::contains(base(), "api.anthropic.com")) {
            request.headers["anthropic-beta"] = "server-side-fallback-2026-07-01";
            body["fallbacks"] = "default";
        }
        request.body = dumpJsonCompact(body);

        struct State {
            SseParser parser;
            std::map<int, Json> blocks;
            std::map<int, std::string> partialInput;
            ChatResult result;
            std::string errorBody;
            std::string streamError;
            std::string refusal;
        };
        auto state = std::make_shared<State>();
        auto shared = std::make_shared<ChatHandlers>(std::move(handlers));
        return transport_->send(
            request,
            [state, shared](std::string_view chunk) {
                state->errorBody.append(chunk.substr(0, std::min<std::size_t>(chunk.size(), 8192)));
                state->parser.feed(chunk, [&](const std::string&, const std::string& data) {
                    auto parsed = parseJson(data);
                    if (!parsed || !parsed->is_object())
                        return;
                    const Json& event = *parsed;
                    std::string type = jsonString(event, "type");
                    if (type == "message_start" && event.contains("message")) {
                        state->result.servedModel = jsonString(event["message"], "model");
                        if (event["message"].contains("usage"))
                            state->result.inputTokens = jsonInt(event["message"]["usage"], "input_tokens");
                    } else if (type == "content_block_start" && event.contains("content_block")) {
                        state->blocks[jsonInt(event, "index")] = event["content_block"];
                    } else if (type == "content_block_delta" && event.contains("delta")) {
                        int index = jsonInt(event, "index");
                        const Json& delta = event["delta"];
                        std::string deltaType = jsonString(delta, "type");
                        Json& block = state->blocks[index];
                        if (deltaType == "text_delta") {
                            std::string value = jsonString(delta, "text");
                            block["text"] = block.value("text", std::string()) + value;
                            if (shared->onText)
                                shared->onText(value);
                        } else if (deltaType == "thinking_delta") {
                            block["thinking"] = block.value("thinking", std::string()) + jsonString(delta, "thinking");
                        } else if (deltaType == "signature_delta") {
                            block["signature"] = block.value("signature", std::string()) + jsonString(delta, "signature");
                        } else if (deltaType == "input_json_delta") {
                            state->partialInput[index] += jsonString(delta, "partial_json");
                        }
                    } else if (type == "message_delta") {
                        if (event.contains("delta"))
                            state->result.stopReason = jsonString(event["delta"], "stop_reason");
                        if (event.contains("usage"))
                            state->result.outputTokens = jsonInt(event["usage"], "output_tokens");
                        if (event.contains("delta") && event["delta"].contains("stop_details") && event["delta"]["stop_details"].is_object())
                            state->refusal = jsonString(event["delta"]["stop_details"], "explanation");
                    } else if (type == "error" && event.contains("error")) {
                        state->streamError = jsonString(event["error"], "message", "The stream failed");
                    }
                });
            },
            [state, shared](int status, const std::string& error) {
                if (!error.empty()) {
                    if (shared->onError)
                        shared->onError(makeError("ai.network", error));
                    return;
                }
                if (status != 200) {
                    if (shared->onError)
                        shared->onError(makeError("ai.status", errorFromBody(status, state->errorBody)));
                    return;
                }
                if (!state->streamError.empty()) {
                    if (shared->onError)
                        shared->onError(makeError("ai.stream", state->streamError));
                    return;
                }
                if (state->result.stopReason == "refusal") {
                    if (shared->onError)
                        shared->onError(makeError("ai.refusal", state->refusal.empty() ? std::string("The model declined this request.") : "The model declined this request: " + state->refusal));
                    return;
                }
                ChatMessage& message = state->result.message;
                message.role = "assistant";
                Json raw = Json::array();
                for (auto& [index, block] : state->blocks) {
                    std::string type = block.value("type", std::string());
                    if (type == "text") {
                        message.text += block.value("text", std::string());
                    } else if (type == "tool_use") {
                        ToolCall call;
                        call.id = jsonString(block, "id");
                        call.name = jsonString(block, "name");
                        std::string partial = state->partialInput[index];
                        auto input = parseJson(partial.empty() ? std::string("{}") : partial);
                        if (input && input->is_object()) {
                            call.input = *input;
                            block["input"] = *input;
                        } else {
                            call.inputValid = false;
                            call.input = Json::object();
                            block["input"] = Json::object();
                        }
                        message.toolCalls.push_back(std::move(call));
                    }
                    raw.push_back(block);
                }
                message.raw = raw;
                if (state->result.stopReason == "max_tokens")
                    message.toolCalls.clear();
                if (shared->onDone)
                    shared->onDone(state->result);
            });
    }

private:
    std::string base() const
    {
        std::string url = config_.baseUrl.empty() ? std::string("https://api.anthropic.com") : config_.baseUrl;
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    std::map<std::string, std::string> headers() const
    {
        std::map<std::string, std::string> output;
        output["content-type"] = "application/json";
        output["anthropic-version"] = "2023-06-01";
        if (!key_.empty())
            output["x-api-key"] = key_;
        return output;
    }

    ProviderConfig config_;
    std::string key_;
    std::shared_ptr<IHttpTransport> transport_;
};

class OpenAiCompatibleProvider final : public IAIProvider {
public:
    OpenAiCompatibleProvider(ProviderConfig config, std::string key, std::shared_ptr<IHttpTransport> transport)
        : config_(std::move(config)), key_(std::move(key)), transport_(std::move(transport))
    {
    }

    const ProviderConfig& config() const override { return config_; }

    std::shared_ptr<IHttpCall> listModels(std::function<void(Result<std::vector<std::string>>)> reply) override
    {
        HttpRequest request;
        request.method = "GET";
        request.url = base() + "/models";
        request.headers = headers();
        auto collected = std::make_shared<std::string>();
        return transport_->send(request, [collected](std::string_view data) { collected->append(data); },
                                [collected, reply = std::move(reply)](int status, const std::string& error) {
                                    if (!error.empty()) {
                                        reply(makeError("ai.network", error));
                                        return;
                                    }
                                    if (status != 200) {
                                        reply(makeError("ai.models", errorFromBody(status, *collected)));
                                        return;
                                    }
                                    std::vector<std::string> models;
                                    auto parsed = parseJson(*collected);
                                    if (parsed && parsed->contains("data")) {
                                        for (const auto& item : (*parsed)["data"])
                                            models.push_back(jsonString(item, "id"));
                                    }
                                    reply(std::move(models));
                                });
    }

    std::shared_ptr<IHttpCall> chat(const ChatRequest& chatRequest, ChatHandlers handlers) override
    {
        Json body = Json::object();
        body["model"] = config_.model;
        body["stream"] = true;
        if (config_.maxOutputTokens > 0)
            body["max_tokens"] = config_.maxOutputTokens;
        Json messages = Json::array();
        if (!chatRequest.system.empty())
            messages.push_back(Json{{"role", "system"}, {"content", chatRequest.system}});
        for (const auto& message : chatRequest.messages) {
            if (message.role == "tool") {
                messages.push_back(Json{{"role", "tool"}, {"tool_call_id", message.toolCallId}, {"content", message.text}});
            } else if (message.role == "assistant" && !message.toolCalls.empty()) {
                Json calls = Json::array();
                for (const auto& call : message.toolCalls)
                    calls.push_back(Json{{"id", call.id}, {"type", "function"}, {"function", {{"name", call.name}, {"arguments", dumpJsonCompact(call.input)}}}});
                Json entry = Json{{"role", "assistant"}, {"tool_calls", calls}};
                entry["content"] = message.text.empty() ? Json(nullptr) : Json(message.text);
                messages.push_back(entry);
            } else {
                messages.push_back(Json{{"role", message.role}, {"content", message.text}});
            }
        }
        body["messages"] = messages;
        if (!chatRequest.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : chatRequest.tools)
                tools.push_back(Json{{"type", "function"}, {"function", {{"name", tool.name}, {"description", tool.description}, {"parameters", tool.schema}}}});
            body["tools"] = tools;
        }
        HttpRequest request;
        request.url = base() + "/chat/completions";
        request.headers = headers();
        request.body = dumpJsonCompact(body);

        struct PartialCall {
            std::string id;
            std::string name;
            std::string arguments;
        };
        struct State {
            SseParser parser;
            std::map<int, PartialCall> calls;
            ChatResult result;
            std::string errorBody;
        };
        auto state = std::make_shared<State>();
        auto shared = std::make_shared<ChatHandlers>(std::move(handlers));
        return transport_->send(
            request,
            [state, shared](std::string_view chunk) {
                state->errorBody.append(chunk.substr(0, std::min<std::size_t>(chunk.size(), 8192)));
                state->parser.feed(chunk, [&](const std::string&, const std::string& data) {
                    if (data == "[DONE]")
                        return;
                    auto parsed = parseJson(data);
                    if (!parsed || !parsed->is_object())
                        return;
                    if (parsed->contains("model"))
                        state->result.servedModel = jsonString(*parsed, "model");
                    if (parsed->contains("usage") && (*parsed)["usage"].is_object()) {
                        state->result.inputTokens = jsonInt((*parsed)["usage"], "prompt_tokens");
                        state->result.outputTokens = jsonInt((*parsed)["usage"], "completion_tokens");
                    }
                    auto choices = parsed->find("choices");
                    if (choices == parsed->end() || !choices->is_array() || choices->empty())
                        return;
                    const Json& choice = (*choices)[0];
                    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
                        state->result.stopReason = choice["finish_reason"].get<std::string>();
                    if (!choice.contains("delta"))
                        return;
                    const Json& delta = choice["delta"];
                    std::string content = jsonString(delta, "content");
                    if (!content.empty()) {
                        state->result.message.text += content;
                        if (shared->onText)
                            shared->onText(content);
                    }
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& call : delta["tool_calls"]) {
                            PartialCall& partial = state->calls[jsonInt(call, "index")];
                            std::string id = jsonString(call, "id");
                            if (!id.empty())
                                partial.id = id;
                            if (call.contains("function")) {
                                std::string name = jsonString(call["function"], "name");
                                if (!name.empty())
                                    partial.name = name;
                                partial.arguments += jsonString(call["function"], "arguments");
                            }
                        }
                    }
                });
            },
            [state, shared](int status, const std::string& error) {
                if (!error.empty()) {
                    if (shared->onError)
                        shared->onError(makeError("ai.network", error));
                    return;
                }
                if (status != 200) {
                    if (shared->onError)
                        shared->onError(makeError("ai.status", errorFromBody(status, state->errorBody)));
                    return;
                }
                state->result.message.role = "assistant";
                if (state->result.stopReason != "length") {
                    int counter = 0;
                    for (const auto& [index, partial] : state->calls) {
                        ToolCall call;
                        call.id = partial.id.empty() ? "call_" + std::to_string(counter) : partial.id;
                        ++counter;
                        call.name = partial.name;
                        auto input = parseJson(partial.arguments.empty() ? std::string("{}") : partial.arguments);
                        if (input && input->is_object()) {
                            call.input = *input;
                        } else {
                            call.inputValid = false;
                            call.input = Json::object();
                        }
                        state->result.message.toolCalls.push_back(std::move(call));
                    }
                }
                if (shared->onDone)
                    shared->onDone(state->result);
            });
    }

private:
    std::string base() const
    {
        std::string url = config_.baseUrl;
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    std::map<std::string, std::string> headers() const
    {
        std::map<std::string, std::string> output;
        output["content-type"] = "application/json";
        if (!key_.empty())
            output["authorization"] = "Bearer " + key_;
        return output;
    }

    ProviderConfig config_;
    std::string key_;
    std::shared_ptr<IHttpTransport> transport_;
};

}

std::unique_ptr<IAIProvider> createProvider(ProviderConfig config, std::string apiKey, std::shared_ptr<IHttpTransport> transport)
{
    if (config.kind == "anthropic")
        return std::make_unique<AnthropicProvider>(std::move(config), std::move(apiKey), std::move(transport));
    return std::make_unique<OpenAiCompatibleProvider>(std::move(config), std::move(apiKey), std::move(transport));
}

std::vector<ProviderConfig> providerPresets()
{
    std::vector<ProviderConfig> presets;
    auto add = [&](const char* id, const char* label, const char* kind, const char* url, const char* model, bool local) {
        ProviderConfig config;
        config.id = id;
        config.label = label;
        config.kind = kind;
        config.baseUrl = url;
        config.model = model;
        config.credentialAccount = local ? std::string() : std::string("ai.") + id;
        config.local = local;
        presets.push_back(std::move(config));
    };
    add("ollama", "Ollama (on this computer)", "openai", "http://localhost:11434/v1", "", true);
    add("lmstudio", "LM Studio (on this computer)", "openai", "http://localhost:1234/v1", "", true);
    add("llamacpp", "llama.cpp server (on this computer)", "openai", "http://localhost:8080/v1", "", true);
    add("anthropic", "Anthropic", "anthropic", "https://api.anthropic.com", "claude-opus-5-5", false);
    add("openai", "OpenAI", "openai", "https://api.openai.com/v1", "", false);
    add("custom", "Other endpoint (OpenAI format)", "openai", "", "", false);
    return presets;
}

Json providerConfigToJson(const ProviderConfig& config)
{
    return Json{{"id", config.id}, {"label", config.label}, {"kind", config.kind}, {"baseUrl", config.baseUrl}, {"model", config.model},
                {"credentialAccount", config.credentialAccount}, {"contextSize", config.contextSize}, {"maxOutputTokens", config.maxOutputTokens}, {"local", config.local}};
}

ProviderConfig providerConfigFromJson(const Json& json)
{
    ProviderConfig config;
    config.id = jsonString(json, "id");
    config.label = jsonString(json, "label", config.id);
    config.kind = jsonString(json, "kind", "openai");
    config.baseUrl = jsonString(json, "baseUrl");
    config.model = jsonString(json, "model");
    config.credentialAccount = jsonString(json, "credentialAccount");
    config.contextSize = jsonInt(json, "contextSize");
    config.maxOutputTokens = jsonInt(json, "maxOutputTokens");
    config.local = jsonBool(json, "local");
    return config;
}

}
