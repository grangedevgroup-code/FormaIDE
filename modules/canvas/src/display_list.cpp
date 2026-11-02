#include "forma/canvas/display_list.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "forma/core/text.h"
#include "forma/ir/catalog.h"

namespace forma::canvas {

using namespace forma::ir;
using layout::Rect;
using layout::ResolvedTextStyle;

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

}

Color Color::parse(std::string_view text, Color fallback)
{
    if (text.empty() || text[0] != '#')
        return fallback;
    std::string digits(text.substr(1));
    for (char c : digits) {
        if (hexValue(c) < 0)
            return fallback;
    }
    auto pair = [&](std::size_t index) { return static_cast<std::uint8_t>(hexValue(digits[index]) * 16 + hexValue(digits[index + 1])); };
    auto single = [&](std::size_t index) { return static_cast<std::uint8_t>(hexValue(digits[index]) * 17); };
    switch (digits.size()) {
    case 3:
        return {single(0), single(1), single(2), 255};
    case 4:
        return {single(0), single(1), single(2), single(3)};
    case 6:
        return {pair(0), pair(2), pair(4), 255};
    case 8:
        return {pair(0), pair(2), pair(4), pair(6)};
    default:
        return fallback;
    }
}

namespace {

class Builder {
public:
    Builder(const layout::LayoutResult& layout, const ApplicationModel& model, const BuildOptions& options)
        : layout_(layout), model_(model), options_(options)
    {
    }

    DisplayList run()
    {
        list_.bounds = Rect{0, 0, layout_.size.width, layout_.size.height};
        if (!layout_.nodes.empty())
            drawNode(0);
        return std::move(list_);
    }

private:
    Color color(const std::string& token, std::string_view fallback = {})
    {
        return Color::parse(resolveColor(model_, token, fallback));
    }

    Color optionalColor(const std::optional<std::string>& value, const std::string& fallbackToken, std::string_view fallback = {})
    {
        if (value)
            return color(*value, fallback);
        return color(fallbackToken, fallback);
    }

    DrawOp& push(DrawOp op, const std::string& component)
    {
        op.component = component;
        list_.ops.push_back(std::move(op));
        return list_.ops.back();
    }

    void rect(const std::string& owner, Rect area, Color fill, Color stroke = {0, 0, 0, 0}, double strokeWidth = 0, double radius = 0)
    {
        DrawOp op;
        op.kind = OpKind::Rect;
        op.rect = area;
        op.fill = fill;
        op.stroke = stroke;
        op.strokeWidth = strokeWidth;
        op.radius = radius;
        push(std::move(op), owner);
    }

    void ellipse(const std::string& owner, Rect area, Color fill, Color stroke = {0, 0, 0, 0}, double strokeWidth = 0)
    {
        DrawOp op;
        op.kind = OpKind::Ellipse;
        op.rect = area;
        op.fill = fill;
        op.stroke = stroke;
        op.strokeWidth = strokeWidth;
        push(std::move(op), owner);
    }

    void label(const std::string& owner, Rect area, std::string value, ResolvedTextStyle style, int hAlign, int vAlign, bool wrap = false)
    {
        DrawOp op;
        op.kind = OpKind::Text;
        op.rect = area;
        op.text = std::move(value);
        op.textStyle = std::move(style);
        op.textHAlign = hAlign;
        op.textVAlign = vAlign;
        op.wrap = wrap;
        push(std::move(op), owner);
    }

    void glyph(const std::string& owner, Rect area, Glyph kind, Color tint, double strokeWidth = 1.5)
    {
        DrawOp op;
        op.kind = OpKind::Glyph;
        op.rect = area;
        op.glyph = kind;
        op.stroke = tint;
        op.strokeWidth = strokeWidth;
        push(std::move(op), owner);
    }

    void drawBackground(const Component& component, const Rect& frame)
    {
        const Style& style = component.style;
        Color fill = style.background ? color(*style.background) : Color{0, 0, 0, 0};
        Color stroke = style.borderColor ? color(*style.borderColor) : Color{0, 0, 0, 0};
        bool ellipseShape = component.type == "shape" && propertyString(component, "shape") == "ellipse";
        if (style.shadow) {
            DrawOp op;
            op.kind = ellipseShape ? OpKind::Ellipse : OpKind::Rect;
            op.rect = frame;
            op.fill = fill.visible() ? fill : Color{0, 0, 0, 0};
            op.radius = style.radius;
            op.shadowX = style.shadow->x;
            op.shadowY = style.shadow->y;
            op.shadowBlur = style.shadow->blur;
            op.shadowColor = Color::parse(resolveColor(model_, style.shadow->color, "#00000040"));
            op.stroke = stroke;
            op.strokeWidth = style.borderWidth;
            push(std::move(op), component.id);
            return;
        }
        if (!fill.visible() && (style.borderWidth <= 0 || !stroke.visible()))
            return;
        DrawOp op;
        op.kind = ellipseShape ? OpKind::Ellipse : OpKind::Rect;
        op.rect = frame;
        op.fill = fill;
        op.stroke = stroke;
        op.strokeWidth = style.borderWidth;
        op.radius = style.radius;
        op.dashed = style.borderStyle == "dashed";
        push(std::move(op), component.id);
    }

    void drawLeaf(const Component& c, const Rect& f)
    {
        ResolvedTextStyle style = layout::resolveTextStyle(c, &model_);
        const std::string& type = c.type;
        Color border = color("$color.border", "#D5D8DE");
        Color surface = color("$color.surface", "#F4F5F7");
        Color muted = color("$color.textMuted", "#5F6673");
        Color primary = color("$color.primary", "#2B6CD4");
        Color background = color("$color.background", "#FFFFFF");
        Color textColor = Color::parse(style.color, {26, 29, 35, 255});
        double fieldRadius = c.style.radius > 0 ? c.style.radius : 6;
        bool disabled = propertyBool(c, "disabled");

        if (type == "text") {
            int hAlign = style.align == "center" ? 1 : style.align == "right" ? 2 : 0;
            label(c.id, f, propertyString(c, "content"), style, hAlign, 0, true);
        } else if (type == "button") {
            std::string variant = propertyString(c, "variant", "primary");
            ResolvedTextStyle buttonText = style;
            Color fill = primary;
            Color stroke{0, 0, 0, 0};
            if (variant == "secondary") {
                fill = surface;
                stroke = border;
            } else if (variant == "text") {
                fill = Color{0, 0, 0, 0};
                if (!c.typography.color)
                    buttonText.color = resolveColor(model_, "$color.primary", "#2B6CD4");
            } else if (!c.typography.color) {
                buttonText.color = resolveColor(model_, "$color.onPrimary", "#FFFFFF");
            }
            if (c.style.background)
                fill = color(*c.style.background);
            if (c.style.borderColor)
                stroke = color(*c.style.borderColor);
            rect(c.id, f, fill, stroke, c.style.borderWidth > 0 ? c.style.borderWidth : (stroke.visible() ? 1 : 0), fieldRadius);
            label(c.id, f, propertyString(c, "label"), buttonText, 1, 1);
        } else if (type == "textinput") {
            Color fill = c.style.background ? color(*c.style.background) : background;
            Color stroke = c.style.borderColor ? color(*c.style.borderColor) : border;
            rect(c.id, f, fill, stroke, c.style.borderWidth > 0 ? c.style.borderWidth : 1, fieldRadius);
            std::string value = propertyString(c, "value");
            ResolvedTextStyle inputText = style;
            if (value.empty()) {
                value = propertyString(c, "placeholder");
                inputText.color = resolveColor(model_, "$color.textMuted", "#5F6673");
            } else if (propertyBool(c, "password")) {
                value = std::string(value.size(), '*');
            }
            label(c.id, Rect{f.x + 10, f.y, std::max(0.0, f.width - 20), f.height}, value, inputText, 0, 1);
        } else if (type == "checkbox" || type == "radio") {
            bool checked = propertyBool(c, "checked");
            Rect box{f.x, f.centerY() - 9, 18, 18};
            if (type == "checkbox") {
                rect(c.id, box, checked ? primary : background, checked ? primary : border, 1, 4);
                if (checked)
                    glyph(c.id, Rect{box.x + 3, box.y + 3, 12, 12}, Glyph::Check, color("$color.onPrimary", "#FFFFFF"), 2);
            } else {
                ellipse(c.id, box, background, checked ? primary : border, 1);
                if (checked)
                    ellipse(c.id, Rect{box.x + 4, box.y + 4, 10, 10}, primary);
            }
            label(c.id, Rect{f.x + 26, f.y, std::max(0.0, f.width - 26), f.height}, propertyString(c, "label"), style, 0, 1);
        } else if (type == "switch") {
            bool on = propertyBool(c, "checked");
            Rect track{f.x, f.centerY() - 11, 40, 22};
            rect(c.id, track, on ? primary : border, {0, 0, 0, 0}, 0, 11);
            ellipse(c.id, Rect{on ? track.x + 20 : track.x + 2, track.y + 2, 18, 18}, background);
            std::string text = propertyString(c, "label");
            if (!text.empty())
                label(c.id, Rect{f.x + 50, f.y, std::max(0.0, f.width - 50), f.height}, text, style, 0, 1);
        } else if (type == "dropdown") {
            rect(c.id, f, c.style.background ? color(*c.style.background) : background, c.style.borderColor ? color(*c.style.borderColor) : border, 1, fieldRadius);
            auto items = propertyStrings(c, "items");
            int selected = static_cast<int>(propertyNumber(c, "selected", 0));
            std::string value = selected >= 0 && selected < static_cast<int>(items.size()) ? items[static_cast<std::size_t>(selected)] : std::string();
            label(c.id, Rect{f.x + 10, f.y, std::max(0.0, f.width - 34), f.height}, value, style, 0, 1);
            glyph(c.id, Rect{f.right() - 24, f.centerY() - 6, 12, 12}, Glyph::Chevron, muted);
        } else if (type == "slider") {
            double minimum = propertyNumber(c, "minimum", 0);
            double maximum = propertyNumber(c, "maximum", 100);
            double value = propertyNumber(c, "value", 50);
            double ratio = maximum > minimum ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0) : 0;
            Rect track{f.x + 8, f.centerY() - 2, std::max(0.0, f.width - 16), 4};
            rect(c.id, track, border, {0, 0, 0, 0}, 0, 2);
            rect(c.id, Rect{track.x, track.y, track.width * ratio, 4}, primary, {0, 0, 0, 0}, 0, 2);
            ellipse(c.id, Rect{track.x + track.width * ratio - 8, f.centerY() - 8, 16, 16}, background, primary, 2);
        } else if (type == "tabs") {
            auto items = propertyStrings(c, "items");
            int selected = static_cast<int>(propertyNumber(c, "selected", 0));
            double width = items.empty() ? 0 : f.width / static_cast<double>(items.size());
            rect(c.id, Rect{f.x, f.y + 39, f.width, 1}, border);
            for (std::size_t i = 0; i < items.size(); ++i) {
                Rect cell{f.x + width * static_cast<double>(i), f.y, width, 40};
                ResolvedTextStyle tabText = style;
                bool active = static_cast<int>(i) == selected;
                if (active) {
                    tabText.weight = 600;
                    tabText.color = resolveColor(model_, "$color.primary", "#2B6CD4");
                    rect(c.id, Rect{cell.x, f.y + 38, cell.width, 2}, primary);
                }
                label(c.id, cell, items[i], tabText, 1, 1);
            }
        } else if (type == "navbar") {
            ResolvedTextStyle titleText = style;
            titleText.weight = std::max(titleText.weight, 600);
            titleText.size = std::max(titleText.size, 17.0);
            double left = f.x + 16;
            if (propertyBool(c, "back")) {
                glyph(c.id, Rect{f.x + 12, f.centerY() - 8, 16, 16}, Glyph::Back, textColor, 2);
                left = f.x + 36;
            }
            rect(c.id, Rect{f.x, f.bottom() - 1, f.width, 1}, border);
            label(c.id, Rect{left, f.y, std::max(0.0, f.right() - left - 16), f.height}, propertyString(c, "title"), titleText, 0, 1);
        } else if (type == "list") {
            auto items = propertyStrings(c, "items");
            rect(c.id, f, c.style.background ? color(*c.style.background) : background, c.style.borderColor ? color(*c.style.borderColor) : border, 1, c.style.radius);
            int selected = static_cast<int>(propertyNumber(c, "selected", -1));
            double rowHeight = 36;
            for (std::size_t i = 0; i < items.size(); ++i) {
                Rect row{f.x + 1, f.y + 1 + rowHeight * static_cast<double>(i), std::max(0.0, f.width - 2), rowHeight};
                if (row.bottom() > f.bottom())
                    break;
                if (static_cast<int>(i) == selected)
                    rect(c.id, row, surface);
                label(c.id, Rect{row.x + 12, row.y, std::max(0.0, row.width - 24), row.height}, items[i], style, 0, 1);
                if (i + 1 < items.size())
                    rect(c.id, Rect{row.x, row.bottom() - 1, row.width, 1}, border);
            }
        } else if (type == "table" || type == "datatable") {
            auto columns = propertyStrings(c, "columns");
            rect(c.id, f, c.style.background ? color(*c.style.background) : background, c.style.borderColor ? color(*c.style.borderColor) : border, 1, c.style.radius);
            double rowHeight = 32;
            rect(c.id, Rect{f.x + 1, f.y + 1, std::max(0.0, f.width - 2), rowHeight}, surface);
            double width = columns.empty() ? 0 : f.width / static_cast<double>(columns.size());
            ResolvedTextStyle header = style;
            header.weight = 600;
            for (std::size_t i = 0; i < columns.size(); ++i)
                label(c.id, Rect{f.x + width * static_cast<double>(i) + 10, f.y, std::max(0.0, width - 20), rowHeight}, columns[i], header, 0, 1);
            for (double y = f.y + rowHeight; y + rowHeight <= f.bottom(); y += rowHeight)
                rect(c.id, Rect{f.x + 1, y, std::max(0.0, f.width - 2), 1}, border);
            for (std::size_t i = 1; i < columns.size(); ++i)
                rect(c.id, Rect{f.x + width * static_cast<double>(i), f.y + 1, 1, std::max(0.0, f.height - 2)}, border);
        } else if (type == "image") {
            std::string source = propertyString(c, "src");
            if (source.empty()) {
                rect(c.id, f, surface, border, 1, c.style.radius);
                glyph(c.id, Rect{f.centerX() - 12, f.centerY() - 12, 24, 24}, Glyph::Placeholder, muted);
            } else {
                DrawOp op;
                op.kind = OpKind::Image;
                op.rect = f;
                op.imageSource = source;
                if (const Asset* asset = model_.findAsset(source))
                    op.imageSource = asset->path;
                op.imageFit = propertyString(c, "fit", "cover");
                op.radius = c.style.radius;
                push(std::move(op), c.id);
            }
        } else if (type == "icon") {
            glyph(c.id, f, Glyph::Star, Color::parse(resolveColor(model_, propertyString(c, "color", "$color.text"), style.color), textColor));
        } else if (type == "chart") {
            rect(c.id, f, c.style.background ? color(*c.style.background) : background, border, 1, c.style.radius);
            std::string kind = propertyString(c, "chart", "bar");
            glyph(c.id, Rect{f.x + 12, f.y + 12, std::max(0.0, f.width - 24), std::max(0.0, f.height - 24)},
                  kind == "line" ? Glyph::LineChart : kind == "pie" ? Glyph::PieChart : Glyph::BarChart, primary, 2);
        } else if (type == "map") {
            rect(c.id, f, surface, border, 1, c.style.radius);
            glyph(c.id, Rect{f.x + 12, f.y + 12, std::max(0.0, f.width - 24), std::max(0.0, f.height - 24)}, Glyph::Map, muted);
        } else if (type == "media") {
            rect(c.id, f, Color{20, 22, 27, 255}, border, 1, c.style.radius);
            glyph(c.id, Rect{f.centerX() - 16, f.centerY() - 16, 32, 32}, Glyph::Play, Color{236, 238, 242, 255}, 2);
        } else if (type == "native" || type == "custom") {
            rect(c.id, f, surface, border, 1, 4);
            list_.ops.back().dashed = true;
            std::string name = propertyString(c, type == "native" ? "control" : "className", type);
            ResolvedTextStyle hint = style;
            hint.size = 12;
            hint.color = resolveColor(model_, "$color.textMuted", "#5F6673");
            label(c.id, f, name, hint, 1, 1);
        } else if (type == "divider") {
        } else if (type == "shape" || type == "spacer") {
            if (type == "spacer" && options_.showPlaceholders) {
                DrawOp op;
                op.kind = OpKind::Rect;
                op.rect = f;
                op.stroke = Color{border.r, border.g, border.b, 120};
                op.strokeWidth = 1;
                op.dashed = true;
                push(std::move(op), c.id);
            }
            if (type == "shape" && propertyString(c, "shape") == "line") {
                DrawOp op;
                op.kind = OpKind::Line;
                op.rect = f;
                op.stroke = c.style.borderColor ? color(*c.style.borderColor) : border;
                op.strokeWidth = c.style.borderWidth > 0 ? c.style.borderWidth : 1;
                push(std::move(op), c.id);
            }
        } else {
            rect(c.id, f, surface, border, 1, 4);
            list_.ops.back().dashed = true;
            ResolvedTextStyle hint = style;
            hint.size = 12;
            hint.color = resolveColor(model_, "$color.textMuted", "#5F6673");
            label(c.id, f, type, hint, 1, 1);
        }
        (void)disabled;
    }

    void drawContainerChrome(const Component& c, const Rect& f)
    {
        ResolvedTextStyle style = layout::resolveTextStyle(c, &model_);
        if (c.type == "dialog") {
            std::string title = propertyString(c, "title");
            if (!title.empty()) {
                style.weight = 600;
                label(c.id, Rect{f.x + c.layout.padding.left, f.y + c.layout.padding.top, std::max(0.0, f.width - c.layout.padding.left - c.layout.padding.right), 24}, title, style, 0, 1);
            }
        } else if (c.type == "tabs") {
            auto items = propertyStrings(c, "items");
            int selected = static_cast<int>(propertyNumber(c, "selected", 0));
            double width = items.empty() ? 0 : f.width / static_cast<double>(items.size());
            Color border = color("$color.border", "#D5D8DE");
            Color primary = color("$color.primary", "#2B6CD4");
            rect(c.id, Rect{f.x, f.y + 39, f.width, 1}, border);
            for (std::size_t i = 0; i < items.size(); ++i) {
                Rect cell{f.x + width * static_cast<double>(i), f.y, width, 40};
                ResolvedTextStyle tabText = style;
                if (static_cast<int>(i) == selected) {
                    tabText.weight = 600;
                    tabText.color = resolveColor(model_, "$color.primary", "#2B6CD4");
                    rect(c.id, Rect{cell.x, f.y + 38, cell.width, 2}, primary);
                }
                label(c.id, cell, items[i], tabText, 1, 1);
            }
        }
    }

    void drawNode(int index)
    {
        const auto& node = layout_.nodes[static_cast<std::size_t>(index)];
        if (!node.visible || node.component == nullptr)
            return;
        const Component& component = *node.component;
        bool hasOpacity = component.style.opacity < 1;
        if (hasOpacity) {
            DrawOp op;
            op.kind = OpKind::PushOpacity;
            op.opacity = component.style.opacity;
            push(std::move(op), component.id);
        }
        const ComponentSchema* schema = builtinCatalog().find(component.type);
        bool isContainer = component.type == "screen" || (schema != nullptr && schema->container) || !component.children.empty();
        bool drawsOwnBackground = component.type == "button" || component.type == "textinput" || component.type == "dropdown" || component.type == "list"
            || component.type == "table" || component.type == "datatable" || component.type == "chart" || component.type == "map"
            || component.type == "media";
        if (!drawsOwnBackground)
            drawBackground(component, node.frame);
        if (isContainer) {
            drawContainerChrome(component, node.frame);
            bool clip = component.style.clip || component.layout.kind == LayoutKind::Scroll;
            if (clip) {
                DrawOp op;
                op.kind = OpKind::PushClip;
                op.rect = node.frame;
                op.radius = component.style.radius;
                push(std::move(op), component.id);
            }
            for (std::size_t i = static_cast<std::size_t>(index) + 1; i < layout_.nodes.size(); ++i) {
                const auto& candidate = layout_.nodes[i];
                if (candidate.depth <= node.depth)
                    break;
                if (candidate.parentIndex == index)
                    drawNode(static_cast<int>(i));
            }
            if (clip) {
                DrawOp op;
                op.kind = OpKind::PopClip;
                push(std::move(op), component.id);
            }
        } else {
            drawLeaf(component, node.frame);
        }
        if (hasOpacity) {
            DrawOp op;
            op.kind = OpKind::PopOpacity;
            push(std::move(op), component.id);
        }
    }

    const layout::LayoutResult& layout_;
    const ApplicationModel& model_;
    const BuildOptions& options_;
    DisplayList list_;
};

bool nodeHittable(const layout::LayoutResult& layout, const layout::LayoutNode& node, double x, double y)
{
    if (!node.visible)
        return false;
    if (!node.frame.contains(x, y))
        return false;
    int parent = node.parentIndex;
    while (parent >= 0) {
        const auto& ancestor = layout.nodes[static_cast<std::size_t>(parent)];
        if (!ancestor.visible)
            return false;
        if (ancestor.component != nullptr
            && (ancestor.component->style.clip || ancestor.component->layout.kind == LayoutKind::Scroll) && !ancestor.frame.contains(x, y))
            return false;
        parent = ancestor.parentIndex;
    }
    return true;
}

}

DisplayList buildDisplayList(const layout::LayoutResult& layout, const ApplicationModel& model, const BuildOptions& options)
{
    return Builder(layout, model, options).run();
}

std::string hitTest(const layout::LayoutResult& layout, double x, double y, bool includeLocked)
{
    for (auto it = layout.nodes.rbegin(); it != layout.nodes.rend(); ++it) {
        if (!nodeHittable(layout, *it, x, y))
            continue;
        if (!includeLocked && it->component != nullptr && it->component->locked)
            continue;
        if (it->parentIndex < 0)
            return it->id;
        return it->id;
    }
    return {};
}

std::vector<std::string> marqueeTest(const layout::LayoutResult& layout, const Rect& area, const std::string& rootId)
{
    std::vector<std::string> hits;
    for (const auto& node : layout.nodes) {
        if (!node.visible || node.id == rootId || node.component == nullptr || node.component->locked)
            continue;
        if (node.frame.x >= area.x && node.frame.y >= area.y && node.frame.right() <= area.right() && node.frame.bottom() <= area.bottom()) {
            bool parentHit = false;
            int parent = node.parentIndex;
            while (parent >= 0) {
                if (std::find(hits.begin(), hits.end(), layout.nodes[static_cast<std::size_t>(parent)].id) != hits.end()) {
                    parentHit = true;
                    break;
                }
                parent = layout.nodes[static_cast<std::size_t>(parent)].parentIndex;
            }
            if (!parentHit)
                hits.push_back(node.id);
        }
    }
    return hits;
}

std::vector<std::string> ancestorsOf(const layout::LayoutResult& layout, const std::string& id)
{
    std::vector<std::string> chain;
    const layout::LayoutNode* node = layout.find(id);
    if (node == nullptr)
        return chain;
    int parent = node->parentIndex;
    while (parent >= 0) {
        chain.push_back(layout.nodes[static_cast<std::size_t>(parent)].id);
        parent = layout.nodes[static_cast<std::size_t>(parent)].parentIndex;
    }
    return chain;
}

}
