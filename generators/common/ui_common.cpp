#include "ui_common.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "forma/core/text.h"
#include "forma/layout/layout_engine.h"

namespace forma::generators {

using namespace ir;

BoxStyle resolveBox(const ApplicationModel& model, const Component& c)
{
    BoxStyle box;
    std::string variant = propertyString(c, "variant", "primary");
    if (c.type == "button") {
        box.radius = 6;
        box.paddingX = 16;
        box.paddingY = 8;
        if (variant == "primary") {
            box.background = resolveColor(model, "$color.primary");
            box.color = resolveColor(model, "$color.onPrimary");
        } else if (variant == "secondary") {
            box.background = resolveColor(model, "$color.surface");
            box.color = resolveColor(model, "$color.text");
            box.borderColor = resolveColor(model, "$color.border");
            box.borderWidth = 1;
        } else {
            box.color = resolveColor(model, "$color.primary");
        }
    } else if (c.type == "textinput" || c.type == "dropdown") {
        box.radius = 6;
        box.paddingX = 10;
        box.paddingY = 6;
        box.background = resolveColor(model, "$color.background");
        box.color = resolveColor(model, "$color.text");
        box.borderColor = resolveColor(model, "$color.border");
        box.borderWidth = 1;
    } else {
        box.color = resolveColor(model, "$color.text");
    }
    if (c.style.background)
        box.background = resolveColor(model, *c.style.background);
    if (c.style.borderColor)
        box.borderColor = resolveColor(model, *c.style.borderColor);
    if (c.style.borderWidth > 0)
        box.borderWidth = c.style.borderWidth;
    if (c.style.radius > 0)
        box.radius = c.style.radius;
    box.opacity = c.style.opacity;
    if (c.typography.color)
        box.color = resolveColor(model, *c.typography.color);
    layout::ResolvedTextStyle text = layout::resolveTextStyle(c, &model);
    box.fontSize = text.size;
    box.fontWeight = text.weight;
    box.italic = c.typography.italic.value_or(false);
    box.underline = c.typography.underline.value_or(false);
    box.fontFamily = c.typography.font;
    box.textAlign = c.typography.align.value_or("");
    return box;
}

std::string rootBackground(const ApplicationModel& model, const Component& root)
{
    if (root.style.background)
        return resolveColor(model, *root.style.background);
    return resolveColor(model, "$color.background", "#FFFFFF");
}

std::string numberText(double value)
{
    if (std::floor(value) == value && std::abs(value) < 1e9)
        return std::to_string(static_cast<long long>(value));
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    std::string out(buffer);
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

const std::vector<std::string>& coreComponentTypes()
{
    static const std::vector<std::string> types = {"screen", "container", "frame", "group", "stack", "vstack", "hstack", "grid", "flex", "scroll", "absolute", "constraint", "card", "text", "button", "image", "icon", "textinput", "checkbox", "radio", "switch", "dropdown", "slider", "divider", "spacer", "shape", "list"};
    return types;
}

std::vector<std::string> coreLayoutKinds()
{
    return allLayoutKinds();
}

std::vector<ResolvedAction> resolveAction(const ApplicationModel& model, const std::string& actionId)
{
    std::vector<ResolvedAction> out;
    const Action* action = model.findAction(actionId);
    if (action == nullptr)
        return out;
    if (action->kind == "sequence") {
        for (const auto& step : action->steps) {
            auto nested = resolveAction(model, step);
            out.insert(out.end(), nested.begin(), nested.end());
        }
        return out;
    }
    ResolvedAction item;
    item.kind = action->kind;
    item.target = action->target;
    item.value = action->value;
    if (action->kind == "handler")
        item.handler = "handle" + text::toPascalCase(action->target.empty() ? action->id : action->target);
    if (action->kind == "openUrl" && item.target.empty() && action->value.is_string())
        item.target = action->value.get<std::string>();
    out.push_back(std::move(item));
    return out;
}

std::set<std::string> collectHandlers(const ApplicationModel& model)
{
    std::set<std::string> handlers;
    for (const auto& action : model.actions) {
        if (action.kind == "handler")
            handlers.insert("handle" + text::toPascalCase(action.target.empty() ? action.id : action.target));
    }
    return handlers;
}

std::set<std::string> screenIds(const ApplicationModel& model)
{
    std::set<std::string> ids;
    for (const auto& screen : model.screens)
        ids.insert(screen.root.id);
    return ids;
}

std::string colorRgbHex(const std::string& color)
{
    if (color.size() == 9 && color[0] == '#')
        return color.substr(0, 7);
    return color;
}

int colorAlpha(const std::string& color)
{
    if (color.size() == 9 && color[0] == '#')
        return std::stoi(color.substr(7, 2), nullptr, 16);
    return 255;
}

codegen::GeneratedFile plainFile(std::string path, std::string content, codegen::FileCategory category)
{
    codegen::GeneratedFile file;
    file.path = std::move(path);
    file.content = std::move(content);
    file.category = category;
    return file;
}

codegen::GeneratedFile standardReadme(const ApplicationModel& model, const codegen::ProjectConfiguration& config, const std::string& framework, const std::string& howToRun, const std::string& userFiles)
{
    std::string name = config.name.empty() ? model.meta.name : config.name;
    std::string body = "# " + name + "\n\nA " + framework + " app made with FormaIDE.\n\n" + howToRun + "\n\nYour own code goes in " + userFiles
        + ". FormaIDE only rewrites the generated files, and it merges or asks before touching anything you changed.\n";
    return plainFile("README.md", body, codegen::FileCategory::User);
}

}
