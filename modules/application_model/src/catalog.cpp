#include "forma/ir/catalog.h"

#include <algorithm>

#include "forma/core/text.h"
#include "forma/ir/serialization.h"

namespace forma::ir {

void ComponentCatalog::add(ComponentSchema schema)
{
    auto existing = std::find_if(schemas_.begin(), schemas_.end(), [&](const auto& item) { return item.type == schema.type; });
    if (existing != schemas_.end())
        *existing = std::move(schema);
    else
        schemas_.push_back(std::move(schema));
}

const ComponentSchema* ComponentCatalog::find(std::string_view type) const
{
    auto it = std::find_if(schemas_.begin(), schemas_.end(), [&](const auto& item) { return item.type == type; });
    return it == schemas_.end() ? nullptr : &*it;
}

std::vector<std::string> ComponentCatalog::categories() const
{
    std::vector<std::string> names;
    for (const auto& schema : schemas_) {
        if (std::find(names.begin(), names.end(), schema.category) == names.end())
            names.push_back(schema.category);
    }
    return names;
}

Component ComponentCatalog::instantiate(std::string_view type, std::string id) const
{
    Component component = makeComponent(std::move(id), std::string(type));
    const ComponentSchema* schema = find(type);
    if (schema == nullptr)
        return component;
    component.name = schema->label;
    auto status = applyFieldOverrides(component, schema->defaults);
    (void)status;
    for (const auto& property : schema->properties) {
        if (!property.defaultValue.is_null())
            component.properties[property.key] = property.defaultValue;
    }
    return component;
}

namespace {

PropertySchema text(std::string key, std::string label, std::string fallback = {}, bool required = false)
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = "Content";
    schema.type = PropertyType::Text;
    if (!fallback.empty())
        schema.defaultValue = std::move(fallback);
    schema.required = required;
    return schema;
}

PropertySchema multiline(std::string key, std::string label, std::string fallback = {}, bool required = false)
{
    PropertySchema schema = text(std::move(key), std::move(label), std::move(fallback), required);
    schema.type = PropertyType::MultilineText;
    return schema;
}

PropertySchema number(std::string key, std::string label, double fallback)
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = "Content";
    schema.type = PropertyType::Number;
    schema.defaultValue = fallback;
    return schema;
}

PropertySchema boolean(std::string key, std::string label, bool fallback, std::string group = "Behavior")
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = std::move(group);
    schema.type = PropertyType::Boolean;
    schema.defaultValue = fallback;
    return schema;
}

PropertySchema choice(std::string key, std::string label, std::vector<std::string> options, std::string fallback)
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = "Content";
    schema.type = PropertyType::Enum;
    schema.options = std::move(options);
    schema.defaultValue = std::move(fallback);
    return schema;
}

PropertySchema list(std::string key, std::string label, std::vector<std::string> fallback)
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = "Content";
    schema.type = PropertyType::StringList;
    schema.defaultValue = std::move(fallback);
    return schema;
}

PropertySchema asset(std::string key, std::string label)
{
    PropertySchema schema;
    schema.key = std::move(key);
    schema.label = std::move(label);
    schema.group = "Content";
    schema.type = PropertyType::Asset;
    return schema;
}

ComponentCatalog buildCatalog()
{
    ComponentCatalog catalog;
    auto add = [&](std::string type, std::string label, std::string category, std::string icon, bool container, forma::Json defaults,
                   std::vector<PropertySchema> properties = {}, std::vector<std::string> events = {}) {
        ComponentSchema schema;
        schema.type = std::move(type);
        schema.label = std::move(label);
        schema.category = std::move(category);
        schema.icon = std::move(icon);
        schema.container = container;
        schema.defaults = std::move(defaults);
        schema.properties = std::move(properties);
        schema.events = std::move(events);
        catalog.add(std::move(schema));
    };
    using J = forma::Json;

    add("container", "Container", "Basic", "square", true,
        J{{"width", 240}, {"height", 120}, {"layout.type", "vertical"}, {"layout.spacing", 8}, {"layout.padding", 8}}, {}, {"onClick"});
    add("frame", "Frame", "Basic", "frame", true,
        J{{"width", 320}, {"height", 240}, {"layout.type", "absolute"}, {"style.background", "$color.surface"}, {"style.clip", true}}, {}, {"onClick"});
    add("group", "Group", "Basic", "group", true, J{{"width", "hug"}, {"height", "hug"}, {"layout.type", "absolute"}});
    add("text", "Text", "Basic", "text", false, J{{"width", "hug"}, {"height", "hug"}},
        {multiline("content", "Text", "Text", true)}, {"onClick"});
    add("image", "Image", "Basic", "image", false, J{{"width", 160}, {"height", 120}},
        {asset("src", "Source"), text("alt", "Description"), choice("fit", "Fit", {"contain", "cover", "fill", "none"}, "cover")}, {"onClick"});
    add("icon", "Icon", "Basic", "star", false, J{{"width", 24}, {"height", 24}},
        {text("name", "Icon name", "star", true), text("color", "Color")}, {"onClick"});
    add("shape", "Shape", "Basic", "shape", false, J{{"width", 80}, {"height", 80}, {"style.background", "$color.surface"}},
        {choice("shape", "Shape", {"rectangle", "ellipse", "line"}, "rectangle")}, {"onClick"});
    add("divider", "Divider", "Basic", "divider", false, J{{"width", "fill"}, {"height", 1}, {"style.background", "$color.border"}},
        {choice("orientation", "Direction", {"horizontal", "vertical"}, "horizontal")});
    add("spacer", "Spacer", "Basic", "spacer", false, J{{"width", 16}, {"height", 16}, {"grow", 0}});

    add("vstack", "Vertical stack", "Layout", "stack-vertical", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "vertical"}, {"layout.spacing", 8}});
    add("hstack", "Horizontal stack", "Layout", "stack-horizontal", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "horizontal"}, {"layout.spacing", 8}, {"layout.align", "center"}});
    add("grid", "Grid", "Layout", "grid", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "grid"}, {"layout.spacing", 8}, {"layout.columns", J::array({"1fr", "1fr"})}});
    add("scroll", "Scroll container", "Layout", "scroll", true,
        J{{"width", "fill"}, {"height", 240}, {"layout.type", "scroll"}, {"layout.spacing", 8}, {"style.clip", true}});
    add("flex", "Flex layout", "Layout", "flex", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "flex"}, {"layout.spacing", 8}, {"layout.wrap", true}});
    add("absolute", "Absolute layout", "Layout", "absolute", true,
        J{{"width", 320}, {"height", 240}, {"layout.type", "absolute"}});
    add("constraint", "Constraint layout", "Layout", "constraint", true,
        J{{"width", "fill"}, {"height", 240}, {"layout.type", "constraint"}});

    add("button", "Button", "Interface", "button", false, J{{"width", "hug"}, {"height", "hug"}},
        {text("label", "Label", "Button", true), choice("variant", "Style", {"primary", "secondary", "text"}, "primary"),
         boolean("disabled", "Disabled", false), boolean("loading", "Loading", false)},
        {"onClick"});
    add("textinput", "Text input", "Interface", "input", false, J{{"width", "fill"}, {"height", "hug"}},
        {text("placeholder", "Placeholder", "Enter text"), text("value", "Value"), boolean("password", "Password", false),
         boolean("disabled", "Disabled", false)},
        {"onChange", "onSubmit"});
    add("checkbox", "Checkbox", "Interface", "checkbox", false, J{{"width", "hug"}, {"height", "hug"}},
        {text("label", "Label", "Checkbox"), boolean("checked", "Checked", false, "Content"), boolean("disabled", "Disabled", false)},
        {"onChange"});
    add("radio", "Radio button", "Interface", "radio", false, J{{"width", "hug"}, {"height", "hug"}},
        {text("label", "Label", "Option"), text("group", "Group", "group"), boolean("checked", "Checked", false, "Content"),
         boolean("disabled", "Disabled", false)},
        {"onChange"});
    add("dropdown", "Dropdown", "Interface", "dropdown", false, J{{"width", "fill"}, {"height", "hug"}},
        {list("items", "Items", {"First", "Second", "Third"}), number("selected", "Selected", 0), boolean("disabled", "Disabled", false)},
        {"onChange"});
    add("slider", "Slider", "Interface", "slider", false, J{{"width", "fill"}, {"height", "hug"}},
        {number("minimum", "Minimum", 0), number("maximum", "Maximum", 100), number("value", "Value", 50), boolean("disabled", "Disabled", false)},
        {"onChange"});
    add("switch", "Switch", "Interface", "switch", false, J{{"width", "hug"}, {"height", "hug"}},
        {text("label", "Label", "Switch"), boolean("checked", "On", false, "Content"), boolean("disabled", "Disabled", false)},
        {"onChange"});
    add("tabs", "Tabs", "Interface", "tabs", true, J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "vertical"}},
        {list("items", "Tabs", {"First", "Second", "Third"}), number("selected", "Selected", 0)}, {"onChange"});
    add("navbar", "Navigation bar", "Interface", "navbar", false, J{{"width", "fill"}, {"height", "hug"}, {"style.background", "$color.surface"}},
        {text("title", "Title", "Title", true), boolean("back", "Back button", false, "Content")}, {"onBack"});
    add("toolbar", "Toolbar", "Interface", "toolbar", true,
        J{{"width", "fill"}, {"height", 44}, {"layout.type", "horizontal"}, {"layout.spacing", 8}, {"layout.padding", J::array({6, 8})},
          {"layout.align", "center"}, {"style.background", "$color.surface"}});
    add("dialog", "Dialog", "Interface", "dialog", true,
        J{{"width", 320}, {"height", "hug"}, {"layout.type", "vertical"}, {"layout.spacing", 12}, {"layout.padding", 16},
          {"style.background", "$color.background"}, {"style.borderColor", "$color.border"}, {"style.borderWidth", 1}, {"style.radius", 8}},
        {text("title", "Title", "Dialog title")}, {"onDismiss"});
    add("modal", "Modal", "Interface", "modal", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "vertical"}, {"layout.spacing", 12}, {"layout.padding", 16},
          {"style.background", "$color.background"}},
        {boolean("dismissible", "Can be dismissed", true, "Content")}, {"onDismiss"});
    add("card", "Card", "Interface", "card", true,
        J{{"width", "fill"}, {"height", "hug"}, {"layout.type", "vertical"}, {"layout.spacing", 8}, {"layout.padding", 16},
          {"style.background", "$color.surface"}, {"style.borderColor", "$color.border"}, {"style.borderWidth", 1}, {"style.radius", 8}},
        {}, {"onClick"});
    add("list", "List", "Interface", "list", false, J{{"width", "fill"}, {"height", 200}},
        {list("items", "Items", {"First item", "Second item", "Third item"}), number("selected", "Selected", -1)}, {"onSelect"});
    add("table", "Table", "Interface", "table", false, J{{"width", "fill"}, {"height", 200}},
        {list("columns", "Columns", {"Name", "Value"})}, {"onSelect"});

    {
        ComponentSchema* unused = nullptr;
        (void)unused;
    }

    add("chart", "Chart", "Advanced", "chart", false, J{{"width", "fill"}, {"height", 200}},
        {choice("chart", "Chart type", {"bar", "line", "pie"}, "bar"), list("labels", "Labels", {"A", "B", "C", "D"})}, {"onSelect"});
    add("map", "Map", "Advanced", "map", false, J{{"width", "fill"}, {"height", 240}},
        {number("latitude", "Latitude", 47.0379), number("longitude", "Longitude", -122.9007), number("zoom", "Zoom", 12)}, {"onMove"});
    add("media", "Media player", "Advanced", "media", false, J{{"width", "fill"}, {"height", 200}},
        {asset("src", "Source"), boolean("autoplay", "Autoplay", false), boolean("controls", "Controls", true)}, {"onEnded"});
    add("datatable", "Data table", "Advanced", "table", false, J{{"width", "fill"}, {"height", 240}},
        {list("columns", "Columns", {"Name", "Value"}), text("model", "Data model"), boolean("sortable", "Sortable", true)}, {"onSelect"});
    add("custom", "Custom widget", "Advanced", "custom", true, J{{"width", 200}, {"height", 120}, {"layout.type", "absolute"}},
        {text("className", "Class name", "CustomWidget", true), text("include", "Header or import")});
    add("native", "Native control", "Advanced", "native", false, J{{"width", 200}, {"height", 44}},
        {text("control", "Control name", "Control", true)});

    return catalog;
}

}

const ComponentCatalog& builtinCatalog()
{
    static const ComponentCatalog catalog = buildCatalog();
    return catalog;
}

}
