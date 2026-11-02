#include "forma/lsp/language_client.h"

#include <algorithm>

#include "forma/core/paths.h"
#include "forma/core/process.h"
#include "forma/core/text.h"

namespace fs = std::filesystem;

namespace forma::lsp {

namespace {

const char* kHexDigits = "0123456789ABCDEF";

Position parsePosition(const Json& json)
{
    return {jsonInt(json, "line"), jsonInt(json, "character")};
}

Range parseRange(const Json& json)
{
    Range range;
    if (json.contains("start"))
        range.start = parsePosition(json["start"]);
    if (json.contains("end"))
        range.end = parsePosition(json["end"]);
    return range;
}

Location parseLocation(const Json& json)
{
    Location location;
    if (json.contains("targetUri")) {
        location.path = paths::toUtf8(uriToPath(jsonString(json, "targetUri")));
        if (json.contains("targetSelectionRange"))
            location.range = parseRange(json["targetSelectionRange"]);
    } else {
        location.path = paths::toUtf8(uriToPath(jsonString(json, "uri")));
        if (json.contains("range"))
            location.range = parseRange(json["range"]);
    }
    return location;
}

std::vector<Location> parseLocations(const Json& result)
{
    std::vector<Location> locations;
    if (result.is_array()) {
        for (const auto& item : result)
            locations.push_back(parseLocation(item));
    } else if (result.is_object()) {
        locations.push_back(parseLocation(result));
    }
    return locations;
}

std::vector<TextEdit> parseEdits(const Json& list)
{
    std::vector<TextEdit> edits;
    if (!list.is_array())
        return edits;
    for (const auto& item : list) {
        TextEdit edit;
        if (item.contains("range"))
            edit.range = parseRange(item["range"]);
        edit.newText = jsonString(item, "newText");
        edits.push_back(std::move(edit));
    }
    return edits;
}

std::string flattenMarkup(const Json& contents)
{
    if (contents.is_string())
        return contents.get<std::string>();
    if (contents.is_object())
        return jsonString(contents, "value");
    if (contents.is_array()) {
        std::string output;
        for (const auto& item : contents) {
            if (!output.empty())
                output += "\n";
            output += flattenMarkup(item);
        }
        return output;
    }
    return {};
}

std::size_t offsetOf(const std::string& text, const std::vector<std::size_t>& lineStarts, Position position)
{
    if (position.line >= static_cast<int>(lineStarts.size()))
        return text.size();
    std::size_t offset = lineStarts[static_cast<std::size_t>(position.line)];
    int units = 0;
    while (offset < text.size() && text[offset] != '\n' && units < position.character) {
        auto c = static_cast<unsigned char>(text[offset]);
        int length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        units += length == 4 ? 2 : 1;
        offset += static_cast<std::size_t>(length);
    }
    return offset;
}

}

std::string pathToUri(const fs::path& path)
{
    std::string generic = paths::toGeneric(path);
    std::string output = "file://";
    if (!generic.empty() && generic[0] != '/')
        output += "/";
    for (char ch : generic) {
        auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == ':') {
            output.push_back(ch);
        } else {
            output.push_back('%');
            output.push_back(kHexDigits[c >> 4]);
            output.push_back(kHexDigits[c & 15]);
        }
    }
    return output;
}

fs::path uriToPath(std::string_view uri)
{
    std::string_view rest = uri;
    if (text::startsWith(rest, "file://"))
        rest.remove_prefix(7);
    std::string decoded;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            decoded.push_back(static_cast<char>(std::stoi(std::string(rest.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(rest[i]);
        }
    }
#if defined(_WIN32)
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':')
        decoded.erase(0, 1);
#endif
    return paths::fromUtf8(decoded);
}

std::string applyTextEdits(std::string input, std::vector<TextEdit> edits)
{
    std::vector<std::size_t> lineStarts = {0};
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\n')
            lineStarts.push_back(i + 1);
    }
    struct Resolved {
        std::size_t start;
        std::size_t end;
        std::string text;
    };
    std::vector<Resolved> resolved;
    for (auto& edit : edits)
        resolved.push_back({offsetOf(input, lineStarts, edit.range.start), offsetOf(input, lineStarts, edit.range.end), std::move(edit.newText)});
    std::stable_sort(resolved.begin(), resolved.end(), [](const Resolved& a, const Resolved& b) { return a.start > b.start; });
    for (const auto& edit : resolved)
        input.replace(edit.start, edit.end - edit.start, edit.text);
    return input;
}

ServerChoice chooseServer(const editor::LanguageDefinition& language)
{
    ServerChoice choice;
    for (const auto& server : language.servers) {
        if (server.command.empty())
            continue;
        std::vector<fs::path> extra;
#if defined(_WIN32)
        extra.push_back(paths::fromUtf8(paths::env("ProgramFiles").value_or("C:\\Program Files")) / "LLVM" / "bin");
        if (auto appData = paths::env("APPDATA"))
            extra.push_back(paths::fromUtf8(*appData) / "npm");
#endif
        extra.push_back(paths::homeDir() / ".cargo" / "bin");
        extra.push_back(paths::homeDir() / "go" / "bin");
        extra.push_back(paths::homeDir() / ".local" / "bin");
        extra.push_back(paths::homeDir() / ".dotnet" / "tools");
        if (auto found = findExecutable(server.command[0], extra)) {
            choice.command = server.command;
            choice.command[0] = paths::toUtf8(*found);
            choice.install = server.install;
            choice.found = true;
            return choice;
        }
        if (choice.install.empty())
            choice.install = server.install;
    }
    return choice;
}

LanguageClient::LanguageClient(fs::path root, std::string languageId) : root_(std::move(root)), languageId_(std::move(languageId)) {}

LanguageClient::~LanguageClient()
{
    stop();
}

Status LanguageClient::start(const std::vector<std::string>& command, std::function<void()> onReady)
{
    if (command.empty())
        return makeError("lsp.command", "No language server command");
    onReady_ = std::move(onReady);
    ProcessSpec spec;
    spec.program = command[0];
    spec.arguments.assign(command.begin() + 1, command.end());
    spec.workingDirectory = root_;
    spec.origin = Origin::System;
    auto status = process_.start(
        spec, [this](const Json& message) { handleMessage(message); }, [this](int) { ready_ = false; },
        [this](std::string_view data) {
            if (onLog_)
                onLog_(data);
        });
    if (!status)
        return status;

    Json params = Json::object();
    params["processId"] = currentProcessId();
    params["rootUri"] = pathToUri(root_);
    params["clientInfo"] = Json{{"name", "FormaIDE"}};
    params["capabilities"] = Json{
        {"textDocument",
         {{"synchronization", {{"didSave", true}}},
          {"completion", {{"completionItem", {{"snippetSupport", false}, {"documentationFormat", {"plaintext"}}}}}},
          {"hover", {{"contentFormat", {"plaintext", "markdown"}}}},
          {"signatureHelp", {{"signatureInformation", {{"documentationFormat", {"plaintext"}}}}}},
          {"definition", Json::object()},
          {"references", Json::object()},
          {"rename", {{"prepareSupport", false}}},
          {"formatting", Json::object()},
          {"publishDiagnostics", {{"relatedInformation", false}}}}},
        {"workspace", {{"workspaceFolders", true}, {"configuration", false}}},
        {"general", {{"positionEncodings", {"utf-16"}}}}};
    params["workspaceFolders"] = Json::array({Json{{"uri", pathToUri(root_)}, {"name", paths::toUtf8(root_.filename())}}});
    request("initialize", params, [this](const Json& result, const Json* error) {
        if (error != nullptr) {
            if (onLog_)
                onLog_("The language server refused to initialize: " + dumpJsonCompact(*error));
            return;
        }
        std::vector<std::function<void()>> queued;
        {
            std::lock_guard lock(mutex_);
            capabilities_ = result.contains("capabilities") ? result["capabilities"] : Json::object();
        }
        notify("initialized", Json::object());
        ready_ = true;
        {
            std::lock_guard lock(mutex_);
            queued.swap(queued_);
        }
        for (auto& item : queued)
            item();
        if (onReady_)
            onReady_();
    });
    return {};
}

void LanguageClient::stop()
{
    if (!process_.running()) {
        process_.stop();
        return;
    }
    if (ready_.load()) {
        request("shutdown", nullptr, [](const Json&, const Json*) {});
        notify("exit", nullptr);
    }
    ready_ = false;
    process_.stop();
    std::map<int, ResponseHandler> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(pending_);
    }
    Json failure = Json{{"message", "The language server stopped"}};
    for (auto& [id, handler] : pending)
        handler(nullptr, &failure);
}

bool LanguageClient::running() const
{
    return process_.running();
}

Json LanguageClient::serverCapabilities() const
{
    std::lock_guard lock(const_cast<std::mutex&>(mutex_));
    return capabilities_;
}

void LanguageClient::request(std::string_view method, Json params, ResponseHandler handler)
{
    int id = nextId_.fetch_add(1);
    {
        std::lock_guard lock(mutex_);
        pending_[id] = std::move(handler);
    }
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["id"] = id;
    message["method"] = std::string(method);
    message["params"] = std::move(params);
    auto status = process_.send(message);
    if (!status) {
        ResponseHandler failed;
        {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                failed = std::move(it->second);
                pending_.erase(it);
            }
        }
        if (failed) {
            Json failure = Json{{"message", status.error().message}};
            failed(nullptr, &failure);
        }
    }
}

void LanguageClient::notify(std::string_view method, Json params)
{
    Json message = Json::object();
    message["jsonrpc"] = "2.0";
    message["method"] = std::string(method);
    message["params"] = std::move(params);
    auto status = process_.send(message);
    (void)status;
}

void LanguageClient::handleMessage(const Json& message)
{
    if (message.contains("method")) {
        std::string method = jsonString(message, "method");
        if (message.contains("id")) {
            Json response = Json::object();
            response["jsonrpc"] = "2.0";
            response["id"] = message["id"];
            if (method == "workspace/configuration") {
                Json items = Json::array();
                if (message.contains("params") && message["params"].contains("items")) {
                    for (std::size_t i = 0; i < message["params"]["items"].size(); ++i)
                        items.push_back(nullptr);
                }
                response["result"] = items;
            } else if (method == "client/registerCapability" || method == "window/workDoneProgress/create") {
                response["result"] = nullptr;
            } else if (method == "workspace/workspaceFolders") {
                response["result"] = Json::array({Json{{"uri", pathToUri(root_)}, {"name", paths::toUtf8(root_.filename())}}});
            } else {
                response["error"] = Json{{"code", -32601}, {"message", "Not supported"}};
            }
            auto status = process_.send(response);
            (void)status;
            return;
        }
        if (method == "textDocument/publishDiagnostics" && onDiagnostics_ && message.contains("params")) {
            const Json& params = message["params"];
            std::vector<Diagnostic> diagnostics;
            if (params.contains("diagnostics")) {
                for (const auto& item : params["diagnostics"]) {
                    Diagnostic diagnostic;
                    if (item.contains("range"))
                        diagnostic.range = parseRange(item["range"]);
                    diagnostic.severity = jsonInt(item, "severity", 1);
                    diagnostic.message = jsonString(item, "message");
                    diagnostic.source = jsonString(item, "source");
                    if (item.contains("code"))
                        diagnostic.code = item["code"].is_string() ? item["code"].get<std::string>() : dumpJsonCompact(item["code"]);
                    diagnostics.push_back(std::move(diagnostic));
                }
            }
            onDiagnostics_(paths::toUtf8(uriToPath(jsonString(params, "uri"))), diagnostics);
        } else if (method == "window/logMessage" && onLog_ && message.contains("params")) {
            onLog_(jsonString(message["params"], "message"));
        }
        return;
    }
    if (!message.contains("id") || !message["id"].is_number())
        return;
    int id = message["id"].get<int>();
    ResponseHandler handler;
    {
        std::lock_guard lock(mutex_);
        auto it = pending_.find(id);
        if (it == pending_.end())
            return;
        handler = std::move(it->second);
        pending_.erase(it);
    }
    if (message.contains("error"))
        handler(nullptr, &message["error"]);
    else
        handler(message.contains("result") ? message["result"] : Json(), nullptr);
}

Json LanguageClient::textDocument(const fs::path& path) const
{
    return Json{{"uri", pathToUri(path)}};
}

Json LanguageClient::positionParams(const fs::path& path, Position position) const
{
    return Json{{"textDocument", textDocument(path)}, {"position", {{"line", position.line}, {"character", position.character}}}};
}

void LanguageClient::didOpen(const fs::path& path, std::string_view contents)
{
    std::string uri = pathToUri(path);
    std::string body(contents);
    auto send = [this, uri, body, path] {
        int version;
        {
            std::lock_guard lock(mutex_);
            version = versions_[uri] = 1;
        }
        notify("textDocument/didOpen", Json{{"textDocument", {{"uri", uri}, {"languageId", languageId_}, {"version", version}, {"text", body}}}});
    };
    if (ready_.load()) {
        send();
    } else {
        std::lock_guard lock(mutex_);
        queued_.push_back(send);
    }
}

void LanguageClient::didChange(const fs::path& path, std::string_view contents)
{
    std::string uri = pathToUri(path);
    std::string body(contents);
    auto send = [this, uri, body] {
        int version;
        {
            std::lock_guard lock(mutex_);
            version = ++versions_[uri];
        }
        Json change = Json::array({Json{{"text", body}}});
        notify("textDocument/didChange", Json{{"textDocument", {{"uri", uri}, {"version", version}}}, {"contentChanges", change}});
    };
    if (ready_.load()) {
        send();
    } else {
        std::lock_guard lock(mutex_);
        queued_.push_back(send);
    }
}

void LanguageClient::didSave(const fs::path& path, std::string_view contents)
{
    if (ready_.load())
        notify("textDocument/didSave", Json{{"textDocument", textDocument(path)}, {"text", std::string(contents)}});
}

void LanguageClient::didClose(const fs::path& path)
{
    if (ready_.load())
        notify("textDocument/didClose", Json{{"textDocument", textDocument(path)}});
    std::lock_guard lock(mutex_);
    versions_.erase(pathToUri(path));
}

void LanguageClient::completion(const fs::path& path, Position position, Reply<std::vector<CompletionItem>> reply)
{
    request("textDocument/completion", positionParams(path, position), [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.completion", jsonString(*error, "message")));
            return;
        }
        std::vector<CompletionItem> items;
        const Json* list = &result;
        if (result.is_object() && result.contains("items"))
            list = &result["items"];
        if (list->is_array()) {
            for (const auto& item : *list) {
                CompletionItem entry;
                entry.label = jsonString(item, "label");
                entry.detail = jsonString(item, "detail");
                entry.insertText = jsonString(item, "insertText", entry.label);
                if (item.contains("textEdit") && item["textEdit"].is_object())
                    entry.insertText = jsonString(item["textEdit"], "newText", entry.insertText);
                entry.kind = jsonInt(item, "kind");
                items.push_back(std::move(entry));
            }
        }
        reply(std::move(items));
    });
}

void LanguageClient::hover(const fs::path& path, Position position, Reply<std::string> reply)
{
    request("textDocument/hover", positionParams(path, position), [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.hover", jsonString(*error, "message")));
            return;
        }
        reply(result.is_object() && result.contains("contents") ? flattenMarkup(result["contents"]) : std::string());
    });
}

void LanguageClient::signatureHelp(const fs::path& path, Position position, Reply<std::string> reply)
{
    request("textDocument/signatureHelp", positionParams(path, position), [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.signature", jsonString(*error, "message")));
            return;
        }
        std::string label;
        if (result.is_object() && result.contains("signatures") && result["signatures"].is_array() && !result["signatures"].empty()) {
            std::size_t active = static_cast<std::size_t>(jsonInt(result, "activeSignature"));
            if (active >= result["signatures"].size())
                active = 0;
            label = jsonString(result["signatures"][active], "label");
        }
        reply(std::move(label));
    });
}

void LanguageClient::definition(const fs::path& path, Position position, Reply<std::vector<Location>> reply)
{
    request("textDocument/definition", positionParams(path, position), [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.definition", jsonString(*error, "message")));
            return;
        }
        reply(parseLocations(result));
    });
}

void LanguageClient::references(const fs::path& path, Position position, Reply<std::vector<Location>> reply)
{
    Json params = positionParams(path, position);
    params["context"] = Json{{"includeDeclaration", true}};
    request("textDocument/references", params, [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.references", jsonString(*error, "message")));
            return;
        }
        reply(parseLocations(result));
    });
}

void LanguageClient::rename(const fs::path& path, Position position, std::string_view newName, Reply<WorkspaceEdit> reply)
{
    Json params = positionParams(path, position);
    params["newName"] = std::string(newName);
    request("textDocument/rename", params, [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.rename", jsonString(*error, "message")));
            return;
        }
        WorkspaceEdit edit;
        if (result.is_object() && result.contains("changes") && result["changes"].is_object()) {
            for (auto item = result["changes"].begin(); item != result["changes"].end(); ++item)
                edit.files[paths::toUtf8(uriToPath(item.key()))] = parseEdits(*item);
        }
        if (result.is_object() && result.contains("documentChanges") && result["documentChanges"].is_array()) {
            for (const auto& change : result["documentChanges"]) {
                if (change.contains("textDocument") && change.contains("edits"))
                    edit.files[paths::toUtf8(uriToPath(jsonString(change["textDocument"], "uri")))] = parseEdits(change["edits"]);
            }
        }
        reply(std::move(edit));
    });
}

void LanguageClient::formatting(const fs::path& path, int tabSize, bool insertSpaces, Reply<std::vector<TextEdit>> reply)
{
    Json params = Json{{"textDocument", textDocument(path)}, {"options", {{"tabSize", tabSize}, {"insertSpaces", insertSpaces}}}};
    request("textDocument/formatting", params, [reply = std::move(reply)](const Json& result, const Json* error) {
        if (error != nullptr) {
            reply(makeError("lsp.formatting", jsonString(*error, "message")));
            return;
        }
        reply(parseEdits(result));
    });
}

}
