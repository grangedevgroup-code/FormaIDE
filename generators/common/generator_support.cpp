#include "generator_support.h"

#include <algorithm>

#include "forma/core/text.h"

namespace forma::generators {

using namespace forma::ir;

ColorRef colorRef(const ApplicationModel& model, std::string_view value)
{
    ColorRef reference;
    if (!value.empty() && value[0] == '$') {
        std::string name(value.substr(1));
        if (text::startsWith(name, "color."))
            name = name.substr(6);
        reference.isToken = true;
        reference.token = name;
        reference.literal = resolveColor(model, value, "#000000");
        return reference;
    }
    reference.literal = std::string(value);
    return reference;
}

std::string NameTable::name(const std::string& id)
{
    auto found = names_.find(id);
    if (found != names_.end())
        return found->second;
    std::string base;
    switch (style_) {
    case Style::Camel:
        base = text::toCamelCase(id);
        break;
    case Style::Pascal:
        base = text::toPascalCase(id);
        break;
    case Style::Snake:
        base = text::toSnakeCase(id);
        break;
    }
    std::string candidate = base;
    if (reserved_.count(candidate) != 0)
        candidate += "Item";
    int counter = 2;
    while (used_.count(candidate) != 0)
        candidate = base + std::to_string(counter++);
    used_.insert(candidate);
    names_[id] = candidate;
    return candidate;
}

std::vector<ScreenNames> screenNames(const ApplicationModel& model, std::string_view suffix)
{
    std::vector<ScreenNames> names;
    std::set<std::string> used;
    for (const auto& screen : model.screens) {
        std::string stem = text::toPascalCase(screen.root.name.empty() ? screen.root.id : screen.root.name);
        std::string base = text::endsWith(stem, suffix) ? stem : stem + std::string(suffix);
        std::string candidate = base;
        int counter = 2;
        while (used.count(candidate) != 0)
            candidate = stem + std::to_string(counter++) + std::string(suffix);
        used.insert(candidate);
        names.push_back({&screen, candidate});
    }
    return names;
}

std::string startScreenId(const ApplicationModel& model)
{
    if (!model.navigation.start.empty() && model.findScreen(model.navigation.start) != nullptr)
        return model.navigation.start;
    return model.screens.empty() ? std::string() : model.screens.front().root.id;
}

std::string definitionClassName(const ComponentDefinition& definition)
{
    return text::toPascalCase(definition.root.name.empty() ? definition.root.id : definition.root.name);
}

bool isContainerType(const Component& component)
{
    if (component.type == "screen" || !component.children.empty())
        return true;
    const ComponentSchema* schema = builtinCatalog().find(component.type);
    return schema != nullptr && schema->container;
}

namespace {

bool anyComponent(const ApplicationModel& model, const std::function<bool(const Component&)>& predicate)
{
    bool found = false;
    auto check = [&](const Component& root) {
        visitComponents(root, [&](const Component& component, const Component*) {
            if (predicate(component))
                found = true;
        });
    };
    for (const auto& screen : model.screens)
        check(screen.root);
    for (const auto& definition : model.components)
        check(definition.root);
    return found;
}

}

bool usesType(const ApplicationModel& model, std::string_view type)
{
    return anyComponent(model, [&](const Component& component) { return component.type == type; });
}

bool usesLayout(const ApplicationModel& model, LayoutKind kind)
{
    return anyComponent(model, [&](const Component& component) { return component.layout.kind == kind && !component.children.empty(); });
}

bool usesWrap(const ApplicationModel& model)
{
    return anyComponent(model, [&](const Component& component) { return component.layout.wrap && !component.children.empty(); });
}

std::vector<std::string> collectEventActions(const Component& component)
{
    std::vector<std::string> actions;
    for (const auto& [event, action] : component.events)
        actions.push_back(action);
    return actions;
}

std::string stateDefaultString(const StateVariable& variable)
{
    if (variable.initial.is_string())
        return variable.initial.get<std::string>();
    return {};
}

double stateDefaultNumber(const StateVariable& variable)
{
    if (variable.initial.is_number())
        return variable.initial.get<double>();
    return 0;
}

bool stateDefaultBool(const StateVariable& variable)
{
    if (variable.initial.is_boolean())
        return variable.initial.get<bool>();
    return false;
}

std::vector<std::string> stateDefaultList(const StateVariable& variable)
{
    std::vector<std::string> values;
    if (variable.initial.is_array()) {
        for (const auto& item : variable.initial) {
            if (item.is_string())
                values.push_back(item.get<std::string>());
        }
    }
    return values;
}

const std::vector<std::string>& standardEvents()
{
    static const std::vector<std::string> events = {"onClick", "onChange", "onSubmit", "onSelect", "onBack", "onDismiss", "onMove", "onEnded"};
    return events;
}

std::vector<std::string> allComponentTypes()
{
    std::vector<std::string> types;
    for (const auto& schema : builtinCatalog().all())
        types.push_back(schema.type);
    return types;
}

std::vector<std::string> allLayoutKinds()
{
    return {"absolute", "vertical", "horizontal", "grid", "flex", "scroll", "constraint"};
}

std::string projectSlug(std::string_view name)
{
    return text::toKebabCase(name);
}

std::string projectClass(std::string_view name)
{
    return text::toPascalCase(name);
}

std::string packageName(std::string_view bundleId)
{
    std::string output;
    for (const auto& part : text::split(bundleId, '.')) {
        if (!output.empty())
            output.push_back('.');
        output += text::toLower(text::toIdentifier(part));
    }
    return output.empty() ? std::string("com.example.app") : output;
}

std::string bindingKeyFor(const Component& component)
{
    if (component.type == "text")
        return "properties.content";
    if (component.type == "button" || component.type == "checkbox" || component.type == "radio" || component.type == "switch")
        return "properties.label";
    if (component.type == "textinput")
        return "properties.value";
    if (component.type == "navbar")
        return "properties.title";
    return {};
}

std::optional<std::string> boundState(const Component& component, const std::string& key)
{
    auto it = component.bindings.find(key);
    if (it != component.bindings.end())
        return it->second.source;
    if (text::startsWith(key, "properties.")) {
        auto shortKey = component.bindings.find(key.substr(11));
        if (shortKey != component.bindings.end())
            return shortKey->second.source;
    }
    return std::nullopt;
}

std::optional<PrimaryText> primaryText(const Component& component)
{
    std::string key = bindingKeyFor(component);
    if (key.empty())
        return std::nullopt;
    return PrimaryText{key, propertyString(component, key.substr(11))};
}

}
