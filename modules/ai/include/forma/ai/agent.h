#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "forma/ai/provider.h"
#include "forma/project/project.h"

namespace forma::ai {

enum class AutonomyLevel { Suggestions = 1, ProposedChanges = 2, ApprovedWorkspace = 3, ControlledAutomation = 4 };

struct StagedFile {
    std::string path;
    enum class Kind { Create, Modify, Delete } kind = Kind::Modify;
    std::string before;
    std::string beforeHash;
    std::string after;
    bool approved = true;
    std::string note;
};

struct StagedDesignEdit {
    std::string description;
    Json operation;
    bool approved = true;
};

struct ChangeSet {
    std::vector<StagedFile> files;
    std::vector<StagedDesignEdit> design;
    std::vector<std::string> dependenciesAdded;
    std::vector<std::string> configurationChanges;
    std::vector<std::string> warnings;
    std::vector<ir::Diagnostic> validation;
    bool empty() const { return files.empty() && design.empty(); }
    std::string summary() const;
};

struct ApplyResult {
    std::string checkpoint;
    std::vector<std::string> written;
    std::vector<std::string> skipped;
    std::vector<std::string> failures;
};

Result<ApplyResult> applyChangeSet(project::ProjectSession& session, ChangeSet& changes, const codegen::GeneratorRegistry* generators = nullptr);
Status rollbackTo(project::ProjectSession& session, std::string_view checkpoint);

bool isSensitivePath(std::string_view path);
Status checkAiPath(std::string_view path);

struct CommandCheck {
    bool allowed = false;
    bool dangerous = false;
    std::string reason;
};

CommandCheck classifyCommand(std::string_view command);

Status applyDesignOperation(ir::ModelEditor& editor, const Json& operation);

struct ContextItem {
    enum class Kind { File, Selection, Design, Diagnostics, FileTree, Note } kind = Kind::File;
    std::string label;
    std::string content;
    bool included = true;
};

class ContextManager {
public:
    void add(ContextItem item);
    void clear();
    std::vector<ContextItem>& items() { return items_; }
    const std::vector<ContextItem>& items() const { return items_; }
    std::string render(std::size_t budgetCharacters = 400000) const;
    std::size_t includedSize() const;

private:
    std::vector<ContextItem> items_;
};

struct AgentOptions {
    AutonomyLevel autonomy = AutonomyLevel::ProposedChanges;
    int maxTurns = 30;
    bool allowFileReads = true;
    bool allowDesignRead = true;
    std::set<std::string> autoApprovedCommands;
};

struct AgentCallbacks {
    std::function<void(std::string_view)> onText;
    std::function<void(const std::string& tool, const Json& input)> onTool;
    std::function<void(const ChangeSet&)> onChangesStaged;
    std::function<void(const std::string&)> onFinished;
    std::function<void(const Error&)> onError;
    std::function<void(const std::string& command, const CommandCheck& check, std::function<void(bool)> decide)> onCommandApproval;
    std::function<void(const std::string& command, std::string_view output)> onCommandOutput;
};

class AgentSession : public std::enable_shared_from_this<AgentSession> {
public:
    AgentSession(project::ProjectSession& project, const codegen::GeneratorRegistry& generators, std::shared_ptr<IAIProvider> provider);

    void setOptions(AgentOptions options) { options_ = std::move(options); }
    void setCallbacks(AgentCallbacks callbacks) { callbacks_ = std::move(callbacks); }
    void setBuildErrorsProvider(std::function<std::string()> provider) { buildErrors_ = std::move(provider); }
    ContextManager& context() { return context_; }
    ChangeSet& staged() { return staged_; }

    void send(std::string text);
    void cancel();
    bool busy() const { return busy_; }
    const std::vector<ChatMessage>& messages() const { return messages_; }

    std::vector<ToolDefinition> tools() const;
    std::string systemPrompt() const;
    std::string executeTool(const ToolCall& call, bool& isError);

private:
    void step();
    void handleResult(const ChatResult& result);
    void continueToolCalls(std::vector<ToolCall> calls, std::size_t index);
    void finish();
    std::string stageWrite(const Json& input, bool& isError);
    std::string stageEdit(const Json& input, bool& isError);
    std::string stageDelete(const Json& input, bool& isError);
    std::string stageDesign(const Json& input, bool& isError);

    project::ProjectSession& project_;
    const codegen::GeneratorRegistry& generators_;
    std::shared_ptr<IAIProvider> provider_;
    AgentOptions options_;
    AgentCallbacks callbacks_;
    ContextManager context_;
    ChangeSet staged_;
    std::vector<ChatMessage> messages_;
    std::shared_ptr<IHttpCall> call_;
    std::function<std::string()> buildErrors_;
    int turns_ = 0;
    bool busy_ = false;
    bool cancelled_ = false;
    bool contextSent_ = false;
};

}
