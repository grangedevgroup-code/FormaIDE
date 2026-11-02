#pragma once

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"
#include "forma/ir/model.h"

namespace forma::ir {

enum class ChangeKind { ComponentChanged, ComponentInserted, ComponentRemoved, ComponentMoved, ScreenAdded, ScreenRemoved, ApplicationChanged, Reset };

struct ChangeEvent {
    ChangeKind kind = ChangeKind::Reset;
    std::string screen;
    std::string component;
    std::string key;
};

struct EditOperation {
    enum class Kind { SetField, Insert, Remove, Move, AddScreen, RemoveScreen, SetSection, AddDefinition, RemoveDefinition };
    Kind kind = Kind::SetField;
    std::string screen;
    std::string component;
    std::string key;
    forma::Json before;
    forma::Json after;
    std::string parent;
    int index = 0;
    std::string newParent;
    int newIndex = 0;
};

class IApplicationModel {
public:
    virtual ~IApplicationModel() = default;
    virtual const ApplicationModel& model() const = 0;
    virtual Status setField(std::string_view screen, std::string_view component, std::string_view key, const forma::Json& value) = 0;
    virtual Result<std::string> insertComponent(std::string_view screen, std::string_view parent, int index, Component component) = 0;
    virtual Status removeComponent(std::string_view screen, std::string_view component) = 0;
    virtual Status moveComponent(std::string_view screen, std::string_view component, std::string_view newParent, int newIndex) = 0;
    virtual Result<std::string> addScreen(Screen screen) = 0;
    virtual Status removeScreen(std::string_view screen) = 0;
    virtual Status setSection(std::string_view section, const forma::Json& value) = 0;
};

class ModelEditor final : public IApplicationModel {
public:
    using Listener = std::function<void(const ChangeEvent&)>;

    ModelEditor();
    explicit ModelEditor(ApplicationModel model);

    const ApplicationModel& model() const override { return model_; }
    void reset(ApplicationModel model);

    Status setField(std::string_view screen, std::string_view component, std::string_view key, const forma::Json& value) override;
    Result<std::string> insertComponent(std::string_view screen, std::string_view parent, int index, Component component) override;
    Status removeComponent(std::string_view screen, std::string_view component) override;
    Status moveComponent(std::string_view screen, std::string_view component, std::string_view newParent, int newIndex) override;
    Result<std::string> addScreen(Screen screen) override;
    Status removeScreen(std::string_view screen) override;
    Status setSection(std::string_view section, const forma::Json& value) override;

    Result<std::string> addDefinition(ComponentDefinition definition);
    Status removeDefinition(std::string_view id);
    Status setScreenField(std::string_view screen, std::string_view key, const forma::Json& value);

    void beginTransaction(std::string label);
    void commitTransaction();
    void rollbackTransaction();
    bool inTransaction() const { return transactionDepth_ > 0; }

    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    std::string undoLabel() const;
    std::string redoLabel() const;
    Status undo();
    Status redo();
    void clearHistory();

    int addListener(Listener listener);
    void removeListener(int id);

    std::uint64_t revision() const { return revision_; }
    bool isDirty() const { return revision_ != cleanRevision_; }
    void markClean() { cleanRevision_ = revision_; }

    std::string uniqueComponentId(std::string_view screen, std::string_view base) const;
    std::string uniqueScreenId(std::string_view base) const;

    void setHistoryRecorder(std::function<void(const std::string& label, const std::vector<EditOperation>&)> recorder);

private:
    struct Group {
        std::string label;
        std::vector<EditOperation> operations;
    };

    Status apply(const EditOperation& operation, bool forward);
    void record(EditOperation operation);
    void notifyChange(const ChangeEvent& event);
    Component* rootFor(std::string_view screen);
    const Component* rootFor(std::string_view screen) const;

    ApplicationModel model_;
    std::deque<Group> undoStack_;
    std::vector<Group> redoStack_;
    Group transaction_;
    int transactionDepth_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t cleanRevision_ = 0;
    std::vector<std::pair<int, Listener>> listeners_;
    int nextListener_ = 1;
    std::function<void(const std::string&, const std::vector<EditOperation>&)> recorder_;
};

class TransactionScope {
public:
    TransactionScope(ModelEditor& editor, std::string label) : editor_(editor) { editor_.beginTransaction(std::move(label)); }
    ~TransactionScope() { editor_.commitTransaction(); }
    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;

private:
    ModelEditor& editor_;
};

}
