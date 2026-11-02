#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/framed_json.h"
#include "forma/editor/language_registry.h"

namespace forma::lsp {

struct Position {
    int line = 0;
    int character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Location {
    std::string path;
    Range range;
};

struct Diagnostic {
    Range range;
    int severity = 1;
    std::string message;
    std::string source;
    std::string code;
};

struct CompletionItem {
    std::string label;
    std::string detail;
    std::string insertText;
    int kind = 0;
};

struct TextEdit {
    Range range;
    std::string newText;
};

struct WorkspaceEdit {
    std::map<std::string, std::vector<TextEdit>> files;
};

std::string pathToUri(const std::filesystem::path& path);
std::filesystem::path uriToPath(std::string_view uri);
std::string applyTextEdits(std::string text, std::vector<TextEdit> edits);

class LanguageClient {
public:
    using DiagnosticsCallback = std::function<void(const std::string& path, const std::vector<Diagnostic>&)>;
    using LogCallback = std::function<void(std::string_view)>;
    template <class T>
    using Reply = std::function<void(Result<T>)>;

    LanguageClient(std::filesystem::path root, std::string languageId);
    ~LanguageClient();

    Status start(const std::vector<std::string>& command, std::function<void()> onReady = {});
    void stop();
    bool running() const;
    bool ready() const { return ready_.load(); }
    const std::string& languageId() const { return languageId_; }
    Json serverCapabilities() const;

    void setDiagnosticsCallback(DiagnosticsCallback callback) { onDiagnostics_ = std::move(callback); }
    void setLogCallback(LogCallback callback) { onLog_ = std::move(callback); }

    void didOpen(const std::filesystem::path& path, std::string_view text);
    void didChange(const std::filesystem::path& path, std::string_view text);
    void didSave(const std::filesystem::path& path, std::string_view text);
    void didClose(const std::filesystem::path& path);

    void completion(const std::filesystem::path& path, Position position, Reply<std::vector<CompletionItem>> reply);
    void hover(const std::filesystem::path& path, Position position, Reply<std::string> reply);
    void signatureHelp(const std::filesystem::path& path, Position position, Reply<std::string> reply);
    void definition(const std::filesystem::path& path, Position position, Reply<std::vector<Location>> reply);
    void references(const std::filesystem::path& path, Position position, Reply<std::vector<Location>> reply);
    void rename(const std::filesystem::path& path, Position position, std::string_view newName, Reply<WorkspaceEdit> reply);
    void formatting(const std::filesystem::path& path, int tabSize, bool insertSpaces, Reply<std::vector<TextEdit>> reply);

private:
    using ResponseHandler = std::function<void(const Json& result, const Json* error)>;

    void request(std::string_view method, Json params, ResponseHandler handler);
    void notify(std::string_view method, Json params);
    void handleMessage(const Json& message);
    Json textDocument(const std::filesystem::path& path) const;
    Json positionParams(const std::filesystem::path& path, Position position) const;

    std::filesystem::path root_;
    std::string languageId_;
    FramedJsonProcess process_;
    std::atomic<bool> ready_{false};
    std::atomic<int> nextId_{1};
    std::mutex mutex_;
    std::map<int, ResponseHandler> pending_;
    std::map<std::string, int> versions_;
    std::vector<std::function<void()>> queued_;
    Json capabilities_;
    DiagnosticsCallback onDiagnostics_;
    LogCallback onLog_;
    std::function<void()> onReady_;
};

struct ServerChoice {
    std::vector<std::string> command;
    std::string install;
    bool found = false;
};

ServerChoice chooseServer(const editor::LanguageDefinition& language);

}
