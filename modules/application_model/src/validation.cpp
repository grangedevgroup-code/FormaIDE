#include "forma/ir/validation.h"

#include <cctype>
#include <regex>
#include <set>

#include "forma/core/text.h"

namespace forma::ir {

namespace {

bool isValidId(const std::string& id)
{
    if (id.empty())
        return false;
    if (!std::isalpha(static_cast<unsigned char>(id[0])) && id[0] != '_')
        return false;
    for (char ch : id) {
        auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

bool isHexColor(const std::string& value)
{
    if (value.size() != 4 && value.size() != 5 && value.size() != 7 && value.size() != 9)
        return false;
    if (value[0] != '#')
        return false;
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

class Validator {
public:
    Validator(const ApplicationModel& model, const ComponentCatalog& catalog) : model_(model), catalog_(catalog) {}

    std::vector<Diagnostic> run()
    {
        checkApplication();
        std::set<std::string> screenIds;
        for (const auto& screen : model_.screens) {
            if (!screenIds.insert(screen.root.id).second)
                add(Severity::Error, "screen.duplicate", "Two screens use the id " + screen.root.id, screen.root.id);
            checkTree(screen.root, screen.root.id);
        }
        for (const auto& definition : model_.components)
            checkTree(definition.root, definition.root.id);
        checkNavigation();
        checkActions();
        checkThemes();
        return std::move(diagnostics_);
    }

private:
    void add(Severity severity, std::string code, std::string message, std::string screen = {}, std::string component = {}, std::string key = {})
    {
        diagnostics_.push_back({severity, std::move(code), std::move(message), std::move(screen), std::move(component), std::move(key)});
    }

    void checkApplication()
    {
        if (model_.meta.name.empty())
            add(Severity::Error, "app.name", "The application has no name");
        if (!model_.meta.bundleId.empty()) {
            static const std::regex pattern("^[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+$");
            if (!std::regex_match(model_.meta.bundleId, pattern))
                add(Severity::Error, "app.bundleId", "The application identifier must look like com.example.app, got " + model_.meta.bundleId);
        }
        if (model_.screens.empty())
            add(Severity::Warning, "app.noScreens", "The application has no screens");
        if (!model_.activeTheme.empty() && model_.findTheme(model_.activeTheme) == nullptr)
            add(Severity::Error, "theme.active", "The active theme does not exist: " + model_.activeTheme);
    }

    void checkColor(const std::optional<std::string>& value, const std::string& screen, const Component& component, const char* key)
    {
        if (value)
            checkColorValue(*value, screen, component.id, key);
    }

    void checkColorValue(const std::string& value, const std::string& screen, const std::string& component, const char* key)
    {
        if (value.empty())
            return;
        if (value[0] == '$') {
            std::string name = value.substr(1);
            if (text::startsWith(name, "color."))
                name = name.substr(6);
            const Theme* theme = model_.theme();
            if (theme == nullptr || theme->tokens.colors.find(name) == theme->tokens.colors.end())
                add(Severity::Warning, "token.color", "Unknown color token " + value, screen, component, key);
            return;
        }
        if (!isHexColor(value))
            add(Severity::Error, "style.color", "Colors must look like #RRGGBB, got " + value, screen, component, key);
    }

    void checkSize(const SizeSpec& size, const std::string& screen, const std::string& component, const char* key)
    {
        if (size.mode == SizeMode::Fixed && size.value < 0)
            add(Severity::Error, "layout.size", "A size cannot be negative", screen, component, key);
        if (size.mode == SizeMode::Percent && (size.value < 0 || size.value > 1000))
            add(Severity::Error, "layout.percent", "A percentage must be between 0 and 1000", screen, component, key);
        if (size.min && size.max && *size.min > *size.max)
            add(Severity::Error, "layout.bounds", "The minimum size is larger than the maximum", screen, component, key);
    }

    void checkTree(const Component& root, const std::string& screen)
    {
        std::set<std::string> ids;
        visitComponents(root, [&](const Component& component, const Component* parent) {
            if (!isValidId(component.id))
                add(Severity::Error, "component.id", "Invalid component id: \"" + component.id + "\"", screen, component.id);
            if (!ids.insert(component.id).second)
                add(Severity::Error, "component.duplicate", "Two components use the id " + component.id, screen, component.id);

            const ComponentSchema* schema = catalog_.find(component.type);
            if (schema == nullptr && component.type != "screen") {
                add(Severity::Warning, "component.type", "Unknown component type " + component.type + "; generators treat it as custom", screen, component.id);
            }
            if (schema != nullptr) {
                for (const auto& property : schema->properties) {
                    if (property.required && !component.properties.contains(property.key) && component.bindings.count("properties." + property.key) == 0 && component.bindings.count(property.key) == 0)
                        add(Severity::Error, "property.required", component.id + " needs a value for " + property.label, screen, component.id, "properties." + property.key);
                }
                if (!schema->container && !component.children.empty())
                    add(Severity::Error, "component.children", component.type + " cannot contain other components", screen, component.id);
                for (const auto& [event, action] : component.events) {
                    if (std::find(schema->events.begin(), schema->events.end(), event) == schema->events.end())
                        add(Severity::Warning, "event.unknown", component.type + " has no event " + event, screen, component.id, "events." + event);
                }
            }

            for (const auto& [event, action] : component.events) {
                if (!action.empty() && model_.findAction(action) == nullptr)
                    add(Severity::Error, "event.action", "Event " + event + " points to a missing action: " + action, screen, component.id, "events." + event);
            }
            for (const auto& [key, binding] : component.bindings) {
                std::string base = binding.source.substr(0, binding.source.find('.'));
                if (model_.findState(base) == nullptr)
                    add(Severity::Error, "binding.source", "Binding on " + key + " points to missing state: " + binding.source, screen, component.id, "bindings." + key);
            }

            checkColor(component.style.background, screen, component, "style.background");
            checkColor(component.style.borderColor, screen, component, "style.borderColor");
            checkColor(component.typography.color, screen, component, "typography.color");
            checkSize(component.width, screen, component.id, "width");
            checkSize(component.height, screen, component.id, "height");
            if (component.style.opacity < 0 || component.style.opacity > 1)
                add(Severity::Error, "style.opacity", "Opacity must be between 0 and 1", screen, component.id, "style.opacity");
            if (component.style.radius < 0)
                add(Severity::Error, "style.radius", "A corner radius cannot be negative", screen, component.id, "style.radius");
            if (component.layout.spacing < 0)
                add(Severity::Warning, "layout.spacing", "Negative spacing is not supported by every generator", screen, component.id, "layout.spacing");

            if (parent != nullptr) {
                if (parent->layout.kind == LayoutKind::Grid) {
                    int columns = static_cast<int>(parent->layout.columns.size());
                    if (columns > 0 && component.grid.column + component.grid.columnSpan > columns)
                        add(Severity::Error, "layout.grid", component.id + " extends past the grid's columns", screen, component.id, "grid.column");
                    if (component.grid.column < 0 || component.grid.row < 0 || component.grid.columnSpan < 1 || component.grid.rowSpan < 1)
                        add(Severity::Error, "layout.grid", component.id + " has an invalid grid position", screen, component.id, "grid");
                }
                if (parent->layout.kind == LayoutKind::Constraint && component.pins.left && component.pins.right
                    && component.width.mode == SizeMode::Fixed)
                    add(Severity::Warning, "layout.pins", component.id + " is pinned on both sides and has a fixed width; the pins win", screen, component.id, "pins");
                bool parentHugsWidth = parent->width.mode == SizeMode::Hug;
                if (parentHugsWidth && component.width.mode == SizeMode::Fill)
                    add(Severity::Warning, "layout.fillInHug", component.id + " fills a parent that hugs its content, so its width is undefined", screen, component.id, "width");
            }

            if (!component.instanceOf.empty() && model_.findDefinition(component.instanceOf) == nullptr)
                add(Severity::Error, "instance.definition", component.id + " is an instance of a missing component: " + component.instanceOf, screen, component.id, "instanceOf");

            std::string source = propertyString(component, "src");
            if (!source.empty() && component.type == "image" && !text::startsWith(source, "http") && model_.findAsset(source) == nullptr)
                add(Severity::Warning, "asset.missing", component.id + " uses an asset that isn't registered: " + source, screen, component.id, "properties.src");
        });
    }

    void checkNavigation()
    {
        const auto& navigation = model_.navigation;
        if (!navigation.start.empty() && model_.findScreen(navigation.start) == nullptr)
            add(Severity::Error, "navigation.start", "The start screen does not exist: " + navigation.start);
        if (navigation.start.empty() && !model_.screens.empty())
            add(Severity::Warning, "navigation.start", "No start screen is set; the first screen will be used");
        for (const auto& route : navigation.routes) {
            if (!route.from.empty() && model_.findScreen(route.from) == nullptr)
                add(Severity::Error, "navigation.route", "Route " + route.id + " starts at a missing screen: " + route.from);
            if (model_.findScreen(route.to) == nullptr)
                add(Severity::Error, "navigation.route", "Route " + route.id + " goes to a missing screen: " + route.to);
        }
    }

    void checkActions()
    {
        std::set<std::string> ids;
        for (const auto& action : model_.actions) {
            if (!ids.insert(action.id).second)
                add(Severity::Error, "action.duplicate", "Two actions use the id " + action.id);
            if (action.kind == "navigate" && model_.findScreen(action.target) == nullptr)
                add(Severity::Error, "action.target", "Action " + action.id + " navigates to a missing screen: " + action.target);
            if ((action.kind == "setState" || action.kind == "toggleState" || action.kind == "incrementState") && model_.findState(action.target) == nullptr)
                add(Severity::Error, "action.target", "Action " + action.id + " changes missing state: " + action.target);
            if (action.kind == "sequence") {
                for (const auto& step : action.steps) {
                    if (model_.findAction(step) == nullptr)
                        add(Severity::Error, "action.step", "Action " + action.id + " runs a missing action: " + step);
                }
            }
            static const std::set<std::string> kinds = {"navigate", "back", "setState", "toggleState", "incrementState", "openUrl", "handler", "sequence", "showDialog", "dismissDialog"};
            if (kinds.count(action.kind) == 0)
                add(Severity::Warning, "action.kind", "Action " + action.id + " has an unknown kind: " + action.kind);
        }
    }

    void checkThemes()
    {
        for (const auto& theme : model_.themes) {
            for (const auto& [name, value] : theme.tokens.colors) {
                if (!isHexColor(value))
                    add(Severity::Error, "theme.color", "Theme " + theme.id + " has an invalid color " + name + ": " + value);
            }
        }
    }

    const ApplicationModel& model_;
    const ComponentCatalog& catalog_;
    std::vector<Diagnostic> diagnostics_;
};

}

std::vector<Diagnostic> validateModel(const ApplicationModel& model, const ComponentCatalog& catalog)
{
    return Validator(model, catalog).run();
}

bool hasErrors(const std::vector<Diagnostic>& diagnostics)
{
    for (const auto& item : diagnostics) {
        if (item.severity == Severity::Error)
            return true;
    }
    return false;
}

const char* severityName(Severity severity)
{
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "error";
}

}
