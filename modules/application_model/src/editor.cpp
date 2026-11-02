#include "forma/ir/editor.h"

#include <algorithm>
#include <set>

#include "forma/core/text.h"
#include "forma/ir/serialization.h"

namespace forma::ir {

namespace {

constexpr std::size_t kMaxUndoGroups = 500;

Json sectionValue(const ApplicationModel& model, std::string_view section)
{
    Json full = applicationToJson(model);
    if (section == "meta")
        return full["meta"];
    if (section == "themes") {
        Json themes = Json::array();
        for (const auto& theme : model.themes)
            themes.push_back(themeToJson(theme));
        return themes;
    }
    auto it = full.find(std::string(section));
    return it == full.end() ? Json() : *it;
}

Status assignSection(ApplicationModel& model, std::string_view section, const Json& value)
{
    if (section == "themes") {
        std::vector<Theme> themes;
        if (!value.is_array())
            return makeError("ir.section", "Themes must be a list");
        for (const auto& item : value) {
            auto theme = themeFromJson(item);
            if (!theme)
                return theme.error();
            themes.push_back(std::move(theme.value()));
        }
        model.themes = std::move(themes);
        return {};
    }
    Json full = applicationToJson(model);
    if (value.is_null())
        full.erase(std::string(section));
    else
        full[std::string(section)] = value;
    auto parsed = applicationFromJson(full);
    if (!parsed)
        return parsed.error();
    parsed->screens = std::move(model.screens);
    parsed->components = std::move(model.components);
    parsed->themes = std::move(model.themes);
    model = std::move(parsed.value());
    return {};
}

}

ModelEditor::ModelEditor() = default;

ModelEditor::ModelEditor(ApplicationModel model) : model_(std::move(model)) {}

void ModelEditor::reset(ApplicationModel model)
{
    model_ = std::move(model);
    undoStack_.clear();
    redoStack_.clear();
    transaction_ = {};
    transactionDepth_ = 0;
    ++revision_;
    cleanRevision_ = revision_;
    notifyChange({ChangeKind::Reset, {}, {}, {}});
}

Component* ModelEditor::rootFor(std::string_view screen)
{
    if (Screen* found = model_.findScreen(screen))
        return &found->root;
    for (auto& definition : model_.components) {
        if (definition.root.id == screen)
            return &definition.root;
    }
    return nullptr;
}

const Component* ModelEditor::rootFor(std::string_view screen) const
{
    return const_cast<ModelEditor*>(this)->rootFor(screen);
}

void ModelEditor::record(EditOperation operation)
{
    if (transactionDepth_ > 0) {
        transaction_.operations.push_back(std::move(operation));
        return;
    }
    Group group;
    group.operations.push_back(std::move(operation));
    switch (group.operations.front().kind) {
    case EditOperation::Kind::SetField:
        group.label = "Change " + group.operations.front().key;
        break;
    case EditOperation::Kind::Insert:
        group.label = "Add component";
        break;
    case EditOperation::Kind::Remove:
        group.label = "Delete component";
        break;
    case EditOperation::Kind::Move:
        group.label = "Move component";
        break;
    case EditOperation::Kind::AddScreen:
        group.label = "Add screen";
        break;
    case EditOperation::Kind::RemoveScreen:
        group.label = "Delete screen";
        break;
    case EditOperation::Kind::SetSection:
        group.label = "Change " + group.operations.front().key;
        break;
    case EditOperation::Kind::AddDefinition:
        group.label = "Add reusable component";
        break;
    case EditOperation::Kind::RemoveDefinition:
        group.label = "Delete reusable component";
        break;
    }
    if (recorder_)
        recorder_(group.label, group.operations);
    undoStack_.push_back(std::move(group));
    while (undoStack_.size() > kMaxUndoGroups)
        undoStack_.pop_front();
    redoStack_.clear();
}

void ModelEditor::notifyChange(const ChangeEvent& event)
{
    auto listeners = listeners_;
    for (const auto& [id, listener] : listeners)
        listener(event);
}

Status ModelEditor::apply(const EditOperation& operation, bool forward)
{
    using Kind = EditOperation::Kind;
    switch (operation.kind) {
    case Kind::SetField: {
        const Json& value = forward ? operation.after : operation.before;
        if (operation.component.empty() || operation.component == "@screen") {
            Screen* screen = model_.findScreen(operation.screen);
            if (screen == nullptr)
                return makeError("ir.missing", "No such screen: " + operation.screen);
            Json json = screenToJson(*screen);
            json.erase("children");
            if (value.is_null())
                jsonErasePath(json, operation.key);
            else
                jsonSetPath(json, operation.key, value);
            auto parsed = screenFromJson(json);
            if (!parsed)
                return parsed.error();
            parsed->root.children = std::move(screen->root.children);
            *screen = std::move(parsed.value());
            notifyChange({ChangeKind::ComponentChanged, operation.screen, screen->root.id, operation.key});
            return {};
        }
        Component* root = rootFor(operation.screen);
        if (root == nullptr)
            return makeError("ir.missing", "No such screen: " + operation.screen);
        Component* component = findComponent(*root, operation.component);
        if (component == nullptr)
            return makeError("ir.missing", "No such component: " + operation.component);
        if (auto status = setComponentField(*component, operation.key, value); !status)
            return status;
        notifyChange({ChangeKind::ComponentChanged, operation.screen, operation.component, operation.key});
        return {};
    }
    case Kind::Insert:
    case Kind::Remove: {
        bool inserting = (operation.kind == Kind::Insert) == forward;
        Component* root = rootFor(operation.screen);
        if (root == nullptr)
            return makeError("ir.missing", "No such screen: " + operation.screen);
        Component* parent = findComponent(*root, operation.parent);
        if (parent == nullptr)
            return makeError("ir.missing", "No such parent: " + operation.parent);
        if (inserting) {
            auto parsed = componentFromJson(operation.after.is_null() ? operation.before : operation.after);
            if (!parsed)
                return parsed.error();
            int index = std::clamp(operation.index, 0, static_cast<int>(parent->children.size()));
            std::string id = parsed->id;
            parent->children.insert(parent->children.begin() + index, std::move(parsed.value()));
            notifyChange({ChangeKind::ComponentInserted, operation.screen, id, {}});
        } else {
            int index = indexInParent(*parent, operation.component);
            if (index < 0)
                return makeError("ir.missing", "No such component: " + operation.component);
            parent->children.erase(parent->children.begin() + index);
            notifyChange({ChangeKind::ComponentRemoved, operation.screen, operation.component, {}});
        }
        return {};
    }
    case Kind::Move: {
        Component* root = rootFor(operation.screen);
        if (root == nullptr)
            return makeError("ir.missing", "No such screen: " + operation.screen);
        const std::string& fromParentId = forward ? operation.parent : operation.newParent;
        const std::string& toParentId = forward ? operation.newParent : operation.parent;
        int toIndex = forward ? operation.newIndex : operation.index;
        Component* fromParent = findComponent(*root, fromParentId);
        Component* toParent = findComponent(*root, toParentId);
        if (fromParent == nullptr || toParent == nullptr)
            return makeError("ir.missing", "No such parent");
        int fromIndex = indexInParent(*fromParent, operation.component);
        if (fromIndex < 0)
            return makeError("ir.missing", "No such component: " + operation.component);
        Component moved = std::move(fromParent->children[static_cast<std::size_t>(fromIndex)]);
        fromParent->children.erase(fromParent->children.begin() + fromIndex);
        toParent = findComponent(*root, toParentId);
        int index = std::clamp(toIndex, 0, static_cast<int>(toParent->children.size()));
        toParent->children.insert(toParent->children.begin() + index, std::move(moved));
        notifyChange({ChangeKind::ComponentMoved, operation.screen, operation.component, {}});
        return {};
    }
    case Kind::AddScreen:
    case Kind::RemoveScreen: {
        bool adding = (operation.kind == Kind::AddScreen) == forward;
        if (adding) {
            auto parsed = screenFromJson(operation.after.is_null() ? operation.before : operation.after);
            if (!parsed)
                return parsed.error();
            int index = std::clamp(operation.index, 0, static_cast<int>(model_.screens.size()));
            std::string id = parsed->root.id;
            model_.screens.insert(model_.screens.begin() + index, std::move(parsed.value()));
            notifyChange({ChangeKind::ScreenAdded, id, {}, {}});
        } else {
            auto it = std::find_if(model_.screens.begin(), model_.screens.end(), [&](const Screen& screen) { return screen.root.id == operation.screen; });
            if (it == model_.screens.end())
                return makeError("ir.missing", "No such screen: " + operation.screen);
            model_.screens.erase(it);
            notifyChange({ChangeKind::ScreenRemoved, operation.screen, {}, {}});
        }
        return {};
    }
    case Kind::AddDefinition:
    case Kind::RemoveDefinition: {
        bool adding = (operation.kind == Kind::AddDefinition) == forward;
        if (adding) {
            auto parsed = definitionFromJson(operation.after.is_null() ? operation.before : operation.after);
            if (!parsed)
                return parsed.error();
            model_.components.push_back(std::move(parsed.value()));
        } else {
            auto it = std::find_if(model_.components.begin(), model_.components.end(), [&](const auto& item) { return item.root.id == operation.screen; });
            if (it == model_.components.end())
                return makeError("ir.missing", "No such component definition: " + operation.screen);
            model_.components.erase(it);
        }
        notifyChange({ChangeKind::ApplicationChanged, {}, {}, "components"});
        return {};
    }
    case Kind::SetSection: {
        const Json& value = forward ? operation.after : operation.before;
        if (auto status = assignSection(model_, operation.key, value); !status)
            return status;
        notifyChange({ChangeKind::ApplicationChanged, {}, {}, operation.key});
        return {};
    }
    }
    return {};
}

Status ModelEditor::setField(std::string_view screen, std::string_view component, std::string_view key, const Json& value)
{
    if (key == "id")
        return makeError("ir.field", "Use the rename command to change an id");
    Component* root = rootFor(screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    Component* target = findComponent(*root, component);
    if (target == nullptr)
        return makeError("ir.missing", "No such component: " + std::string(component));
    EditOperation operation;
    operation.kind = EditOperation::Kind::SetField;
    operation.screen = std::string(screen);
    operation.component = std::string(component);
    operation.key = std::string(key);
    operation.before = getComponentField(*target, key);
    operation.after = value;
    if (operation.before == operation.after)
        return {};
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Status ModelEditor::setScreenField(std::string_view screen, std::string_view key, const Json& value)
{
    Screen* target = model_.findScreen(screen);
    if (target == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    Json json = screenToJson(*target);
    json.erase("children");
    const Json* existing = jsonFindPath(json, key);
    EditOperation operation;
    operation.kind = EditOperation::Kind::SetField;
    operation.screen = std::string(screen);
    operation.component = "@screen";
    operation.key = std::string(key);
    operation.before = existing != nullptr ? *existing : Json();
    operation.after = value;
    if (operation.before == operation.after)
        return {};
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Result<std::string> ModelEditor::insertComponent(std::string_view screen, std::string_view parent, int index, Component component)
{
    Component* root = rootFor(screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    Component* target = findComponent(*root, parent);
    if (target == nullptr)
        return makeError("ir.missing", "No such parent: " + std::string(parent));

    std::function<void(Component&)> ensureUnique = [&](Component& node) {
        if (node.id.empty() || findComponent(*root, node.id) != nullptr)
            node.id = uniqueComponentId(screen, node.id.empty() ? node.type : node.id);
        std::set<std::string> seen;
        visitComponents(*root, [&](const Component& existing, const Component*) { seen.insert(existing.id); });
        for (auto& child : node.children) {
            if (seen.count(child.id) != 0 || child.id == node.id)
                child.id = uniqueComponentId(screen, child.type);
            ensureUnique(child);
        }
    };
    ensureUnique(component);

    EditOperation operation;
    operation.kind = EditOperation::Kind::Insert;
    operation.screen = std::string(screen);
    operation.parent = std::string(parent);
    operation.component = component.id;
    operation.index = index < 0 ? static_cast<int>(target->children.size()) : index;
    operation.after = componentToJson(component);
    if (auto status = apply(operation, true); !status)
        return status.error();
    ++revision_;
    std::string id = operation.component;
    record(std::move(operation));
    return id;
}

Status ModelEditor::removeComponent(std::string_view screen, std::string_view component)
{
    Component* root = rootFor(screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    if (root->id == component)
        return makeError("ir.root", "The root of a screen cannot be deleted");
    Component* parent = findParent(*root, component);
    if (parent == nullptr)
        return makeError("ir.missing", "No such component: " + std::string(component));
    int index = indexInParent(*parent, component);
    EditOperation operation;
    operation.kind = EditOperation::Kind::Remove;
    operation.screen = std::string(screen);
    operation.parent = parent->id;
    operation.component = std::string(component);
    operation.index = index;
    operation.before = componentToJson(parent->children[static_cast<std::size_t>(index)]);
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Status ModelEditor::moveComponent(std::string_view screen, std::string_view component, std::string_view newParent, int newIndex)
{
    Component* root = rootFor(screen);
    if (root == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    Component* parent = findParent(*root, component);
    if (parent == nullptr)
        return makeError("ir.missing", "No such component: " + std::string(component));
    Component* moving = findComponent(*root, component);
    if (findComponent(*moving, newParent) != nullptr)
        return makeError("ir.cycle", "A component cannot be moved into itself");
    Component* target = findComponent(*root, newParent);
    if (target == nullptr)
        return makeError("ir.missing", "No such parent: " + std::string(newParent));
    int index = indexInParent(*parent, component);
    if (parent == target && index == newIndex)
        return {};
    EditOperation operation;
    operation.kind = EditOperation::Kind::Move;
    operation.screen = std::string(screen);
    operation.component = std::string(component);
    operation.parent = parent->id;
    operation.index = index;
    operation.newParent = std::string(newParent);
    operation.newIndex = newIndex;
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Result<std::string> ModelEditor::addScreen(Screen screen)
{
    if (screen.root.id.empty() || model_.findScreen(screen.root.id) != nullptr)
        screen.root.id = uniqueScreenId(screen.root.id.empty() ? "screen" : screen.root.id);
    EditOperation operation;
    operation.kind = EditOperation::Kind::AddScreen;
    operation.screen = screen.root.id;
    operation.index = static_cast<int>(model_.screens.size());
    operation.after = screenToJson(screen);
    if (auto status = apply(operation, true); !status)
        return status.error();
    ++revision_;
    std::string id = operation.screen;
    record(std::move(operation));
    return id;
}

Status ModelEditor::removeScreen(std::string_view screen)
{
    const Screen* target = model_.findScreen(screen);
    if (target == nullptr)
        return makeError("ir.missing", "No such screen: " + std::string(screen));
    EditOperation operation;
    operation.kind = EditOperation::Kind::RemoveScreen;
    operation.screen = std::string(screen);
    operation.index = static_cast<int>(target - model_.screens.data());
    operation.before = screenToJson(*target);
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Result<std::string> ModelEditor::addDefinition(ComponentDefinition definition)
{
    if (definition.root.id.empty() || model_.findDefinition(definition.root.id) != nullptr)
        return makeError("ir.duplicate", "A reusable component already uses the id " + definition.root.id);
    EditOperation operation;
    operation.kind = EditOperation::Kind::AddDefinition;
    operation.screen = definition.root.id;
    operation.after = definitionToJson(definition);
    if (auto status = apply(operation, true); !status)
        return status.error();
    ++revision_;
    std::string id = operation.screen;
    record(std::move(operation));
    return id;
}

Status ModelEditor::removeDefinition(std::string_view id)
{
    const ComponentDefinition* definition = model_.findDefinition(id);
    if (definition == nullptr)
        return makeError("ir.missing", "No such reusable component: " + std::string(id));
    EditOperation operation;
    operation.kind = EditOperation::Kind::RemoveDefinition;
    operation.screen = std::string(id);
    operation.before = definitionToJson(*definition);
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

Status ModelEditor::setSection(std::string_view section, const Json& value)
{
    EditOperation operation;
    operation.kind = EditOperation::Kind::SetSection;
    operation.key = std::string(section);
    operation.before = sectionValue(model_, section);
    operation.after = value;
    if (operation.before == operation.after)
        return {};
    if (auto status = apply(operation, true); !status)
        return status;
    ++revision_;
    record(std::move(operation));
    return {};
}

void ModelEditor::beginTransaction(std::string label)
{
    if (transactionDepth_ == 0) {
        transaction_ = {};
        transaction_.label = std::move(label);
    }
    ++transactionDepth_;
}

void ModelEditor::commitTransaction()
{
    if (transactionDepth_ == 0)
        return;
    --transactionDepth_;
    if (transactionDepth_ > 0)
        return;
    if (transaction_.operations.empty())
        return;
    if (recorder_)
        recorder_(transaction_.label, transaction_.operations);
    undoStack_.push_back(std::move(transaction_));
    transaction_ = {};
    while (undoStack_.size() > kMaxUndoGroups)
        undoStack_.pop_front();
    redoStack_.clear();
}

void ModelEditor::rollbackTransaction()
{
    if (transactionDepth_ == 0)
        return;
    transactionDepth_ = 0;
    for (auto it = transaction_.operations.rbegin(); it != transaction_.operations.rend(); ++it) {
        auto status = apply(*it, false);
        (void)status;
    }
    transaction_ = {};
    ++revision_;
}

std::string ModelEditor::undoLabel() const
{
    return undoStack_.empty() ? std::string() : undoStack_.back().label;
}

std::string ModelEditor::redoLabel() const
{
    return redoStack_.empty() ? std::string() : redoStack_.back().label;
}

Status ModelEditor::undo()
{
    if (undoStack_.empty())
        return makeError("ir.undo", "Nothing to undo");
    Group group = std::move(undoStack_.back());
    undoStack_.pop_back();
    for (auto it = group.operations.rbegin(); it != group.operations.rend(); ++it) {
        if (auto status = apply(*it, false); !status)
            return status;
    }
    ++revision_;
    if (recorder_)
        recorder_("Undo " + group.label, group.operations);
    redoStack_.push_back(std::move(group));
    return {};
}

Status ModelEditor::redo()
{
    if (redoStack_.empty())
        return makeError("ir.redo", "Nothing to redo");
    Group group = std::move(redoStack_.back());
    redoStack_.pop_back();
    for (const auto& operation : group.operations) {
        if (auto status = apply(operation, true); !status)
            return status;
    }
    ++revision_;
    if (recorder_)
        recorder_("Redo " + group.label, group.operations);
    undoStack_.push_back(std::move(group));
    return {};
}

void ModelEditor::clearHistory()
{
    undoStack_.clear();
    redoStack_.clear();
}

int ModelEditor::addListener(Listener listener)
{
    int id = nextListener_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void ModelEditor::removeListener(int id)
{
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](const auto& item) { return item.first == id; }), listeners_.end());
}

std::string ModelEditor::uniqueComponentId(std::string_view screen, std::string_view base) const
{
    const Component* root = rootFor(screen);
    std::string stem = text::toSnakeCase(base);
    std::size_t underscore = stem.find_last_of('_');
    if (underscore != std::string::npos && underscore + 1 < stem.size()
        && std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(underscore) + 1, stem.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        stem = stem.substr(0, underscore);
    for (int counter = 1;; ++counter) {
        std::string candidate = stem + "_" + std::to_string(counter);
        if (root == nullptr || findComponent(*root, candidate) == nullptr)
            return candidate;
    }
}

std::string ModelEditor::uniqueScreenId(std::string_view base) const
{
    std::string stem = text::toSnakeCase(base);
    if (model_.findScreen(stem) == nullptr)
        return stem;
    for (int counter = 2;; ++counter) {
        std::string candidate = stem + "_" + std::to_string(counter);
        if (model_.findScreen(candidate) == nullptr)
            return candidate;
    }
}

void ModelEditor::setHistoryRecorder(std::function<void(const std::string&, const std::vector<EditOperation>&)> recorder)
{
    recorder_ = std::move(recorder);
}

}
