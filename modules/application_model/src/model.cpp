#include "forma/ir/model.h"

#include <algorithm>
#include <functional>

#include "forma/core/text.h"

namespace forma::ir {

const Screen* ApplicationModel::findScreen(std::string_view id) const
{
    auto it = std::find_if(screens.begin(), screens.end(), [&](const Screen& screen) { return screen.root.id == id; });
    return it == screens.end() ? nullptr : &*it;
}

Screen* ApplicationModel::findScreen(std::string_view id)
{
    auto it = std::find_if(screens.begin(), screens.end(), [&](const Screen& screen) { return screen.root.id == id; });
    return it == screens.end() ? nullptr : &*it;
}

const ComponentDefinition* ApplicationModel::findDefinition(std::string_view id) const
{
    auto it = std::find_if(components.begin(), components.end(), [&](const auto& item) { return item.root.id == id; });
    return it == components.end() ? nullptr : &*it;
}

const Action* ApplicationModel::findAction(std::string_view id) const
{
    auto it = std::find_if(actions.begin(), actions.end(), [&](const auto& item) { return item.id == id; });
    return it == actions.end() ? nullptr : &*it;
}

const StateVariable* ApplicationModel::findState(std::string_view name) const
{
    auto it = std::find_if(state.begin(), state.end(), [&](const auto& item) { return item.name == name; });
    return it == state.end() ? nullptr : &*it;
}

const Asset* ApplicationModel::findAsset(std::string_view idOrPath) const
{
    auto it = std::find_if(assets.begin(), assets.end(),
                           [&](const auto& item) { return item.id == idOrPath || item.path == idOrPath; });
    return it == assets.end() ? nullptr : &*it;
}

const Theme* ApplicationModel::findTheme(std::string_view id) const
{
    auto it = std::find_if(themes.begin(), themes.end(), [&](const auto& item) { return item.id == id; });
    return it == themes.end() ? nullptr : &*it;
}

const Theme* ApplicationModel::theme() const
{
    if (const Theme* active = findTheme(activeTheme))
        return active;
    return themes.empty() ? nullptr : &themes.front();
}

const Component* findComponent(const Component& root, std::string_view id)
{
    if (root.id == id)
        return &root;
    for (const auto& child : root.children) {
        if (const Component* found = findComponent(child, id))
            return found;
    }
    return nullptr;
}

Component* findComponent(Component& root, std::string_view id)
{
    return const_cast<Component*>(findComponent(const_cast<const Component&>(root), id));
}

const Component* findParent(const Component& root, std::string_view id)
{
    for (const auto& child : root.children) {
        if (child.id == id)
            return &root;
        if (const Component* found = findParent(child, id))
            return found;
    }
    return nullptr;
}

Component* findParent(Component& root, std::string_view id)
{
    return const_cast<Component*>(findParent(const_cast<const Component&>(root), id));
}

int indexInParent(const Component& parent, std::string_view id)
{
    for (std::size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

void visitComponents(const Component& root, const std::function<void(const Component&, const Component*)>& visitor)
{
    std::function<void(const Component&, const Component*)> walk = [&](const Component& node, const Component* parent) {
        visitor(node, parent);
        for (const auto& child : node.children)
            walk(child, &node);
    };
    walk(root, nullptr);
}

std::string layoutKindName(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Absolute:
        return "absolute";
    case LayoutKind::Vertical:
        return "vertical";
    case LayoutKind::Horizontal:
        return "horizontal";
    case LayoutKind::Grid:
        return "grid";
    case LayoutKind::Flex:
        return "flex";
    case LayoutKind::Scroll:
        return "scroll";
    case LayoutKind::Constraint:
        return "constraint";
    }
    return "absolute";
}

std::string alignName(Align align)
{
    switch (align) {
    case Align::Start:
        return "start";
    case Align::Center:
        return "center";
    case Align::End:
        return "end";
    case Align::Stretch:
        return "stretch";
    }
    return "start";
}

std::string justifyName(Justify justify)
{
    switch (justify) {
    case Justify::Start:
        return "start";
    case Justify::Center:
        return "center";
    case Justify::End:
        return "end";
    case Justify::SpaceBetween:
        return "spaceBetween";
    case Justify::SpaceAround:
        return "spaceAround";
    case Justify::SpaceEvenly:
        return "spaceEvenly";
    }
    return "start";
}

std::string sizeModeName(SizeMode mode)
{
    switch (mode) {
    case SizeMode::Fixed:
        return "fixed";
    case SizeMode::Fill:
        return "fill";
    case SizeMode::Hug:
        return "hug";
    case SizeMode::Percent:
        return "percent";
    }
    return "hug";
}

std::string propertyString(const Component& component, std::string_view key, std::string_view fallback)
{
    return jsonString(component.properties, key, fallback);
}

double propertyNumber(const Component& component, std::string_view key, double fallback)
{
    return jsonNumber(component.properties, key, fallback);
}

bool propertyBool(const Component& component, std::string_view key, bool fallback)
{
    return jsonBool(component.properties, key, fallback);
}

std::vector<std::string> propertyStrings(const Component& component, std::string_view key)
{
    return jsonStrings(component.properties, key);
}

std::string resolveColor(const ApplicationModel& model, std::string_view value, std::string_view fallback)
{
    if (value.empty())
        return std::string(fallback);
    if (value[0] != '$')
        return std::string(value);
    std::string name(value.substr(1));
    if (text::startsWith(name, "color."))
        name = name.substr(6);
    if (const Theme* theme = model.theme()) {
        auto it = theme->tokens.colors.find(name);
        if (it != theme->tokens.colors.end())
            return it->second;
    }
    return std::string(fallback);
}

Component makeComponent(std::string id, std::string type, std::string name)
{
    Component component;
    component.id = std::move(id);
    component.type = std::move(type);
    component.name = name.empty() ? text::toPascalCase(component.type) : std::move(name);
    return component;
}

Screen makeScreen(std::string id, std::string name, double width, double height)
{
    Screen screen;
    screen.root = makeComponent(std::move(id), "screen", std::move(name));
    screen.root.width = SizeSpec{SizeMode::Fixed, width};
    screen.root.height = SizeSpec{SizeMode::Fixed, height};
    screen.root.layout.kind = LayoutKind::Vertical;
    screen.root.layout.spacing = 16;
    screen.root.layout.padding = EdgeInsets{24, 24, 24, 24};
    screen.root.layout.align = Align::Stretch;
    screen.root.style.background = "$color.background";
    return screen;
}

Theme makeDefaultTheme(std::string id, std::string name, std::string mode)
{
    Theme theme;
    theme.id = std::move(id);
    theme.name = std::move(name);
    theme.mode = std::move(mode);
    bool dark = theme.mode == "dark";
    theme.tokens.colors = {
        {"background", dark ? "#16181D" : "#FFFFFF"},
        {"surface", dark ? "#20232A" : "#F4F5F7"},
        {"border", dark ? "#3A3F4B" : "#D5D8DE"},
        {"text", dark ? "#ECEEF2" : "#1A1D23"},
        {"textMuted", dark ? "#A0A6B4" : "#5F6673"},
        {"primary", dark ? "#5B9BFF" : "#2B6CD4"},
        {"onPrimary", dark ? "#0E1420" : "#FFFFFF"},
        {"danger", dark ? "#F07178" : "#C9352E"},
    };
    TextStyle body;
    body.size = 15;
    body.weight = 400;
    TextStyle heading;
    heading.size = 28;
    heading.weight = 700;
    TextStyle subheading;
    subheading.size = 20;
    subheading.weight = 600;
    TextStyle caption;
    caption.size = 12;
    caption.weight = 400;
    theme.tokens.typography = {{"body", body}, {"heading", heading}, {"subheading", subheading}, {"caption", caption}};
    theme.tokens.spacing = {{"xs", 4}, {"sm", 8}, {"md", 16}, {"lg", 24}, {"xl", 40}};
    theme.tokens.radius = {{"sm", 4}, {"md", 8}, {"lg", 16}};
    theme.tokens.borders = {{"thin", 1}, {"thick", 2}};
    return theme;
}

ApplicationModel makeEmptyApplication(std::string name, std::string bundleId)
{
    ApplicationModel model;
    model.meta.id = text::toSnakeCase(name);
    model.meta.name = std::move(name);
    model.meta.bundleId = std::move(bundleId);
    model.themes.push_back(makeDefaultTheme("light", "Light", "light"));
    model.themes.push_back(makeDefaultTheme("dark", "Dark", "dark"));
    model.activeTheme = "light";
    model.breakpoints = {{"compact", "Compact", 0, 599}, {"medium", "Medium", 600, 1023}, {"wide", "Wide", 1024, std::nullopt}};
    BuildConfiguration debug;
    debug.id = "debug";
    debug.name = "Debug";
    debug.kind = "debug";
    BuildConfiguration release;
    release.id = "release";
    release.name = "Release";
    release.kind = "release";
    model.buildConfigurations = {debug, release};
    return model;
}

}
