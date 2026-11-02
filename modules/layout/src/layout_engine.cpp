#include "forma/layout/layout_engine.h"

#include <algorithm>
#include <cmath>

#include "forma/core/text.h"
#include "forma/ir/catalog.h"
#include "forma/ir/serialization.h"

namespace forma::layout {

using namespace forma::ir;

const LayoutNode* LayoutResult::find(std::string_view id) const
{
    auto it = index.find(std::string(id));
    return it == index.end() ? nullptr : &nodes[static_cast<std::size_t>(it->second)];
}

Size ApproximateTextMeasurer::measure(std::string_view input, const ResolvedTextStyle& style, double maxWidth) const
{
    double charWidth = style.size * (style.weight >= 600 ? 0.58 : 0.54) + style.letterSpacing;
    double lineHeight = style.lineHeight > 0 ? style.lineHeight : std::ceil(style.size * 1.3);
    double widest = 0;
    int lines = 0;
    for (const auto& line : text::split(input, '\n')) {
        double width = static_cast<double>(line.size()) * charWidth;
        if (maxWidth > 0 && width > maxWidth) {
            lines += static_cast<int>(std::ceil(width / maxWidth));
            widest = maxWidth;
        } else {
            lines += 1;
            widest = std::max(widest, width);
        }
    }
    return {std::ceil(widest), lineHeight * std::max(1, lines)};
}

std::string breakpointForWidth(const ApplicationModel& model, double width)
{
    for (const auto& breakpoint : model.breakpoints) {
        if (width >= breakpoint.minWidth && (!breakpoint.maxWidth || width <= *breakpoint.maxWidth))
            return breakpoint.id;
    }
    return {};
}

ResolvedTextStyle resolveTextStyle(const Component& component, const ApplicationModel* model)
{
    ResolvedTextStyle style;
    style.color = "#1A1D23";
    auto merge = [&](const TextStyle& source) {
        if (!source.font.empty())
            style.font = source.font;
        if (source.size)
            style.size = *source.size;
        if (source.weight)
            style.weight = *source.weight;
        if (source.italic)
            style.italic = *source.italic;
        if (source.underline)
            style.underline = *source.underline;
        if (source.color)
            style.color = *source.color;
        if (source.align)
            style.align = *source.align;
        if (source.lineHeight)
            style.lineHeight = *source.lineHeight;
        if (source.letterSpacing)
            style.letterSpacing = *source.letterSpacing;
    };
    if (model != nullptr) {
        style.color = resolveColor(*model, "$color.text", "#1A1D23");
        if (const Theme* theme = model->theme()) {
            auto body = theme->tokens.typography.find("body");
            if (body != theme->tokens.typography.end())
                merge(body->second);
            if (!component.typography.ref.empty()) {
                auto named = theme->tokens.typography.find(component.typography.ref);
                if (named != theme->tokens.typography.end())
                    merge(named->second);
            }
        }
    }
    merge(component.typography);
    if (model != nullptr)
        style.color = resolveColor(*model, style.color, style.color);
    return style;
}

Component resolveComponent(const Component& component, const LayoutOptions& options)
{
    bool needsWork = !component.tokens.empty() || !component.instanceOf.empty()
        || (!options.platform.empty() && component.platform.count(options.platform) != 0)
        || (!options.breakpoint.empty() && component.variants.count(options.breakpoint) != 0)
        || (!options.state.empty() && component.states.count(options.state) != 0);
    if (!needsWork)
        return component;

    Component resolved = component;
    if (!component.instanceOf.empty() && options.model != nullptr) {
        if (const ComponentDefinition* definition = options.model->findDefinition(component.instanceOf)) {
            Component expanded = definition->root;
            expanded.id = component.id;
            expanded.name = component.name;
            expanded.x = component.x;
            expanded.y = component.y;
            expanded.pins = component.pins;
            expanded.grid = component.grid;
            expanded.margin = component.margin;
            expanded.grow = component.grow;
            expanded.shrink = component.shrink;
            expanded.alignSelf = component.alignSelf;
            if (!(component.width == SizeSpec{}))
                expanded.width = component.width;
            if (!(component.height == SizeSpec{}))
                expanded.height = component.height;
            expanded.visible = component.visible;
            expanded.instanceOf = component.instanceOf;
            expanded.overrides = component.overrides;
            std::function<void(Component&)> prefix = [&](Component& node) {
                for (auto& child : node.children) {
                    child.id = component.id + "__" + child.id;
                    prefix(child);
                }
            };
            for (auto item = component.overrides.begin(); item != component.overrides.end(); ++item) {
                Component* target = item.key().empty() || item.key() == definition->root.id ? &expanded : findComponent(expanded, item.key());
                if (target != nullptr) {
                    auto status = applyFieldOverrides(*target, *item);
                    (void)status;
                }
            }
            prefix(expanded);
            resolved = std::move(expanded);
        }
    }
    if (options.model != nullptr) {
        if (const Theme* theme = options.model->theme()) {
            for (const auto& [key, token] : component.tokens) {
                auto dot = token.find('.');
                if (dot == std::string::npos)
                    continue;
                std::string group = token.substr(0, dot);
                std::string name = token.substr(dot + 1);
                const std::map<std::string, double>* table = group == "spacing" ? &theme->tokens.spacing
                    : group == "radius"                                         ? &theme->tokens.radius
                    : group == "borders"                                        ? &theme->tokens.borders
                                                                                : nullptr;
                if (table != nullptr) {
                    auto it = table->find(name);
                    if (it != table->end()) {
                        auto status = setComponentField(resolved, key, it->second);
                        (void)status;
                    }
                } else if (group == "color") {
                    auto status = setComponentField(resolved, key, "$" + token);
                    (void)status;
                }
            }
        }
    }
    auto applyOverride = [&](const std::map<std::string, Json>& table, const std::string& key) {
        if (key.empty())
            return;
        auto it = table.find(key);
        if (it != table.end()) {
            auto status = applyFieldOverrides(resolved, it->second);
            (void)status;
        }
    };
    applyOverride(component.variants, options.breakpoint);
    applyOverride(component.platform, options.platform);
    applyOverride(component.states, options.state);
    return resolved;
}

namespace {

class Engine {
public:
    Engine(const LayoutOptions& options) : options_(options), store_(std::make_shared<std::deque<Component>>())
    {
        static const ApproximateTextMeasurer fallback;
        measurer_ = options.measurer != nullptr ? options.measurer : &fallback;
    }

    LayoutResult run(const Component& root, Size viewport)
    {
        LayoutResult result;
        result.storage = store_;
        const Component& resolved = hold(root);
        double width = resolved.width.mode == SizeMode::Fixed ? resolved.width.value : viewport.width;
        double height = resolved.height.mode == SizeMode::Fixed ? resolved.height.value : viewport.height;
        if (viewport.width > 0 && resolved.type == "screen")
            width = viewport.width;
        if (viewport.height > 0 && resolved.type == "screen")
            height = viewport.height;
        Size size{clampSize(width, resolved.width), clampSize(height, resolved.height)};
        result.size = size;
        place(resolved, Rect{0, 0, size.width, size.height}, -1, 0, result);
        return result;
    }

private:
    const Component& hold(const Component& component)
    {
        Component resolved = resolveComponent(component, options_);
        if (resolved == component)
            return component;
        store_->push_back(std::move(resolved));
        return store_->back();
    }

    static double clampSize(double value, const SizeSpec& spec)
    {
        if (spec.min)
            value = std::max(value, *spec.min);
        if (spec.max)
            value = std::min(value, *spec.max);
        return std::max(0.0, value);
    }

    Size leafIntrinsic(const Component& component, double maxWidth) const
    {
        ResolvedTextStyle style = resolveTextStyle(component, options_.model);
        auto textSize = [&](const std::string& value, double reserved = 0) {
            return measurer_->measure(value, style, maxWidth > 0 ? std::max(1.0, maxWidth - reserved) : 0);
        };
        const std::string& type = component.type;
        if (type == "text") {
            return textSize(propertyString(component, "content"));
        }
        if (type == "button") {
            Size label = textSize(propertyString(component, "label"), 32);
            return {label.width + 32, std::max(36.0, label.height + 16)};
        }
        if (type == "textinput")
            return {200, std::max(36.0, std::ceil(style.size * 1.3) + 16)};
        if (type == "dropdown")
            return {200, std::max(36.0, std::ceil(style.size * 1.3) + 16)};
        if (type == "checkbox" || type == "radio") {
            Size label = textSize(propertyString(component, "label"), 28);
            return {label.width + 28, std::max(20.0, label.height)};
        }
        if (type == "switch") {
            Size label = textSize(propertyString(component, "label"), 52);
            return {label.width + (label.width > 0 ? 52 : 44), std::max(24.0, label.height)};
        }
        if (type == "slider")
            return {200, 24};
        if (type == "navbar") {
            Size label = textSize(propertyString(component, "title"));
            return {label.width + 32, std::max(52.0, label.height + 24)};
        }
        if (type == "tabs")
            return {240, 40};
        if (type == "divider")
            return {1, 1};
        if (type == "icon")
            return {24, 24};
        if (type == "image" || type == "media")
            return {160, 120};
        if (type == "list" || type == "table" || type == "datatable" || type == "chart" || type == "map")
            return {240, 200};
        if (type == "shape")
            return {80, 80};
        if (type == "spacer")
            return {8, 8};
        return {100, 40};
    }

    double dimension(const SizeSpec& spec, double available, double intrinsic, double fill) const
    {
        double value = intrinsic;
        switch (spec.mode) {
        case SizeMode::Fixed:
            value = spec.value;
            break;
        case SizeMode::Percent:
            value = available * spec.value / 100.0;
            break;
        case SizeMode::Fill:
            value = fill;
            break;
        case SizeMode::Hug:
            value = intrinsic;
            break;
        }
        return clampSize(value, spec);
    }

    bool hasChildrenLayout(const Component& component) const
    {
        if (!component.children.empty())
            return true;
        const ComponentSchema* schema = builtinCatalog().find(component.type);
        return component.type == "screen" || (schema != nullptr && schema->container);
    }

    Size measure(const Component& source, double availableWidth, double availableHeight)
    {
        const Component& component = hold(source);
        double width = -1;
        double height = -1;
        if (component.width.mode == SizeMode::Fixed)
            width = component.width.value;
        else if (component.width.mode == SizeMode::Percent)
            width = availableWidth * component.width.value / 100.0;
        else if (component.width.mode == SizeMode::Fill && availableWidth > 0)
            width = availableWidth - component.margin.left - component.margin.right;
        if (component.height.mode == SizeMode::Fixed)
            height = component.height.value;
        else if (component.height.mode == SizeMode::Percent)
            height = availableHeight * component.height.value / 100.0;
        if (width >= 0)
            width = clampSize(width, component.width);
        if (height >= 0)
            height = clampSize(height, component.height);
        if (width >= 0 && height >= 0)
            return {width, height};

        Size content;
        if (hasChildrenLayout(component)) {
            double innerMax = width >= 0 ? width : availableWidth;
            content = measureContent(component, innerMax);
        } else {
            double maxWidth = width >= 0 ? width : availableWidth - component.margin.left - component.margin.right;
            content = leafIntrinsic(component, maxWidth > 0 ? maxWidth : 0);
        }
        if (width < 0)
            width = clampSize(content.width, component.width);
        if (height < 0)
            height = clampSize(content.height, component.height);
        return {width, height};
    }

    static bool isFlowKind(LayoutKind kind)
    {
        return kind == LayoutKind::Vertical || kind == LayoutKind::Horizontal || kind == LayoutKind::Flex || kind == LayoutKind::Scroll;
    }

    static bool isHorizontal(const LayoutSpec& layout)
    {
        return layout.kind == LayoutKind::Horizontal || layout.kind == LayoutKind::Flex
            || (layout.kind == LayoutKind::Scroll && layout.scroll == ScrollAxis::Horizontal);
    }

    Size measureContent(const Component& component, double availableWidth)
    {
        const LayoutSpec& layout = component.layout;
        double innerWidth = availableWidth > 0 ? std::max(0.0, availableWidth - layout.padding.left - layout.padding.right) : 0;
        double outerH = layout.padding.left + layout.padding.right;
        double outerV = layout.padding.top + layout.padding.bottom;
        std::vector<const Component*> children;
        for (const auto& child : component.children) {
            if (child.visible)
                children.push_back(&child);
        }
        if (children.empty())
            return {outerH, outerV};

        if (layout.kind == LayoutKind::Absolute || layout.kind == LayoutKind::Constraint) {
            double right = 0;
            double bottom = 0;
            for (const Component* child : children) {
                Size size = measure(*child, innerWidth, 0);
                right = std::max(right, child->x + size.width);
                bottom = std::max(bottom, child->y + size.height);
            }
            return {right + outerH, bottom + outerV};
        }
        if (layout.kind == LayoutKind::Grid) {
            auto [columnWidths, rowHeights] = gridTracks(component, innerWidth, 0);
            double width = 0;
            for (double value : columnWidths)
                width += value;
            double height = 0;
            for (double value : rowHeights)
                height += value;
            width += layout.spacing * std::max(0.0, static_cast<double>(columnWidths.size()) - 1);
            height += layout.rowSpacing.value_or(layout.spacing) * std::max(0.0, static_cast<double>(rowHeights.size()) - 1);
            return {width + outerH, height + outerV};
        }
        if (isHorizontal(layout)) {
            if (layout.wrap && innerWidth > 0) {
                auto rows = wrapRows(children, layout, innerWidth);
                double width = 0;
                double height = 0;
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    width = std::max(width, rows[i].width);
                    height += rows[i].height;
                    if (i > 0)
                        height += layout.rowSpacing.value_or(layout.spacing);
                }
                return {width + outerH, height + outerV};
            }
            double width = 0;
            double height = 0;
            bool first = true;
            for (const Component* child : children) {
                Size size = measure(*child, 0, 0);
                width += size.width + child->margin.left + child->margin.right;
                height = std::max(height, size.height + child->margin.top + child->margin.bottom);
                if (!first)
                    width += layout.spacing;
                first = false;
            }
            return {width + outerH, height + outerV};
        }
        double width = 0;
        double height = 0;
        bool first = true;
        for (const Component* child : children) {
            Size size = measure(*child, innerWidth, 0);
            width = std::max(width, size.width + child->margin.left + child->margin.right);
            height += size.height + child->margin.top + child->margin.bottom;
            if (!first)
                height += layout.spacing;
            first = false;
        }
        return {width + outerH, height + outerV};
    }

    struct WrapRow {
        std::vector<const Component*> items;
        std::vector<Size> sizes;
        double width = 0;
        double height = 0;
    };

    std::vector<WrapRow> wrapRows(const std::vector<const Component*>& children, const LayoutSpec& layout, double innerWidth)
    {
        std::vector<WrapRow> rows;
        WrapRow current;
        for (const Component* child : children) {
            Size size = measure(*child, innerWidth, 0);
            double total = size.width + child->margin.left + child->margin.right;
            double next = current.items.empty() ? total : current.width + layout.spacing + total;
            if (!current.items.empty() && next > innerWidth) {
                rows.push_back(std::move(current));
                current = {};
                next = total;
            }
            current.items.push_back(child);
            current.sizes.push_back(size);
            current.width = next;
            current.height = std::max(current.height, size.height + child->margin.top + child->margin.bottom);
        }
        if (!current.items.empty())
            rows.push_back(std::move(current));
        return rows;
    }

    std::pair<std::vector<double>, std::vector<double>> gridTracks(const Component& component, double innerWidth, double innerHeight)
    {
        const LayoutSpec& layout = component.layout;
        std::vector<GridTrack> columns = layout.columns;
        if (columns.empty())
            columns.push_back(GridTrack{});
        int rowCount = static_cast<int>(layout.rows.size());
        std::vector<const Component*> visible;
        int cursorColumn = 0;
        int cursorRow = 0;
        struct Placed {
            const Component* component;
            int column;
            int row;
            int columnSpan;
            int rowSpan;
        };
        std::vector<Placed> placed;
        for (const auto& child : component.children) {
            if (!child.visible)
                continue;
            int column = child.grid.column;
            int row = child.grid.row;
            if (child.grid == GridPlacement{}) {
                column = cursorColumn;
                row = cursorRow;
                ++cursorColumn;
                if (cursorColumn >= static_cast<int>(columns.size())) {
                    cursorColumn = 0;
                    ++cursorRow;
                }
            }
            placed.push_back({&child, column, row, child.grid.columnSpan, child.grid.rowSpan});
            rowCount = std::max(rowCount, row + child.grid.rowSpan);
        }
        rowCount = std::max(rowCount, 1);
        double spacing = layout.spacing;
        double rowSpacing = layout.rowSpacing.value_or(layout.spacing);

        std::vector<double> columnWidths(columns.size(), 0.0);
        double fractionTotal = 0;
        double used = spacing * std::max(0.0, static_cast<double>(columns.size()) - 1);
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].kind == TrackKind::Fixed) {
                columnWidths[i] = columns[i].value;
                used += columns[i].value;
            } else if (columns[i].kind == TrackKind::Auto) {
                double widest = 0;
                for (const auto& item : placed) {
                    if (item.column == static_cast<int>(i) && item.columnSpan == 1) {
                        Size size = measure(*item.component, 0, 0);
                        widest = std::max(widest, size.width + item.component->margin.left + item.component->margin.right);
                    }
                }
                columnWidths[i] = widest;
                used += widest;
            } else {
                fractionTotal += columns[i].value;
            }
        }
        if (fractionTotal > 0) {
            double remaining = std::max(0.0, innerWidth - used);
            for (std::size_t i = 0; i < columns.size(); ++i) {
                if (columns[i].kind == TrackKind::Fraction) {
                    if (innerWidth > 0) {
                        columnWidths[i] = remaining * columns[i].value / fractionTotal;
                    } else {
                        double widest = 0;
                        for (const auto& item : placed) {
                            if (item.column == static_cast<int>(i)) {
                                Size size = measure(*item.component, 0, 0);
                                widest = std::max(widest, size.width + item.component->margin.left + item.component->margin.right);
                            }
                        }
                        columnWidths[i] = widest;
                    }
                }
            }
        }

        std::vector<GridTrack> rows = layout.rows;
        rows.resize(static_cast<std::size_t>(rowCount), GridTrack{TrackKind::Auto, 0});
        std::vector<double> rowHeights(rows.size(), 0.0);
        double rowFraction = 0;
        double rowUsed = rowSpacing * std::max(0.0, static_cast<double>(rows.size()) - 1);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].kind == TrackKind::Fixed) {
                rowHeights[i] = rows[i].value;
                rowUsed += rows[i].value;
            } else if (rows[i].kind == TrackKind::Auto) {
                double tallest = 0;
                for (const auto& item : placed) {
                    if (item.row == static_cast<int>(i) && item.rowSpan == 1) {
                        double cellWidth = 0;
                        for (int c = item.column; c < item.column + item.columnSpan && c < static_cast<int>(columnWidths.size()); ++c)
                            cellWidth += columnWidths[static_cast<std::size_t>(c)];
                        Size size = measure(*item.component, cellWidth, 0);
                        tallest = std::max(tallest, size.height + item.component->margin.top + item.component->margin.bottom);
                    }
                }
                rowHeights[i] = tallest;
                rowUsed += tallest;
            } else {
                rowFraction += rows[i].value;
            }
        }
        if (rowFraction > 0) {
            double remaining = std::max(0.0, innerHeight - rowUsed);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].kind == TrackKind::Fraction)
                    rowHeights[i] = remaining * rows[i].value / rowFraction;
            }
        }
        return {columnWidths, rowHeights};
    }

    void place(const Component& component, const Rect& frame, int parentIndex, int depth, LayoutResult& result)
    {
        LayoutNode node;
        node.id = component.id;
        node.frame = frame;
        node.component = &component;
        node.depth = depth;
        node.parentIndex = parentIndex;
        node.visible = component.visible;
        const LayoutSpec& layout = component.layout;
        node.contentFrame = Rect{frame.x + layout.padding.left, frame.y + layout.padding.top,
                                 std::max(0.0, frame.width - layout.padding.left - layout.padding.right),
                                 std::max(0.0, frame.height - layout.padding.top - layout.padding.bottom)};
        int index = static_cast<int>(result.nodes.size());
        result.index[node.id] = index;
        result.nodes.push_back(node);
        Rect content = result.nodes.back().contentFrame;

        if (component.children.empty())
            return;
        switch (layout.kind) {
        case LayoutKind::Absolute:
            placeAbsolute(component, content, index, depth, result);
            break;
        case LayoutKind::Constraint:
            placeConstraint(component, content, index, depth, result);
            break;
        case LayoutKind::Grid:
            placeGrid(component, content, index, depth, result);
            break;
        default:
            if (isHorizontal(layout)) {
                if (layout.wrap)
                    placeWrapped(component, content, index, depth, result);
                else
                    placeLine(component, content, index, depth, result, true);
            } else {
                placeLine(component, content, index, depth, result, false);
            }
            break;
        }
    }

    void placeHidden(const Component& component, const Rect& parent, int parentIndex, int depth, LayoutResult& result)
    {
        place(hold(component), Rect{parent.x, parent.y, 0, 0}, parentIndex, depth, result);
    }

    void placeAbsolute(const Component& parent, const Rect& content, int parentIndex, int depth, LayoutResult& result)
    {
        for (const auto& source : parent.children) {
            const Component& child = hold(source);
            if (!child.visible) {
                placeHidden(child, content, parentIndex, depth + 1, result);
                continue;
            }
            Size size = measure(child, content.width, content.height);
            double width = child.width.mode == SizeMode::Fill ? content.width : size.width;
            double height = child.height.mode == SizeMode::Fill ? content.height : size.height;
            place(child, Rect{content.x + child.x, content.y + child.y, width, height}, parentIndex, depth + 1, result);
        }
    }

    void placeConstraint(const Component& parent, const Rect& content, int parentIndex, int depth, LayoutResult& result)
    {
        for (const auto& source : parent.children) {
            const Component& child = hold(source);
            if (!child.visible) {
                placeHidden(child, content, parentIndex, depth + 1, result);
                continue;
            }
            Size size = measure(child, content.width, content.height);
            double width = child.width.mode == SizeMode::Fill ? content.width : size.width;
            double height = child.height.mode == SizeMode::Fill ? content.height : size.height;
            double x = child.x;
            double y = child.y;
            const Pins& pins = child.pins;
            if (pins.left && pins.right) {
                x = *pins.left;
                width = std::max(0.0, content.width - *pins.left - *pins.right);
            } else if (pins.left) {
                x = *pins.left;
            } else if (pins.right) {
                x = content.width - width - *pins.right;
            } else if (pins.centerX) {
                x = (content.width - width) / 2 + *pins.centerX;
            }
            if (pins.top && pins.bottom) {
                y = *pins.top;
                height = std::max(0.0, content.height - *pins.top - *pins.bottom);
            } else if (pins.top) {
                y = *pins.top;
            } else if (pins.bottom) {
                y = content.height - height - *pins.bottom;
            } else if (pins.centerY) {
                y = (content.height - height) / 2 + *pins.centerY;
            }
            width = clampSize(width, child.width);
            height = clampSize(height, child.height);
            place(child, Rect{content.x + x, content.y + y, width, height}, parentIndex, depth + 1, result);
        }
    }

    void placeLine(const Component& parent, const Rect& content, int parentIndex, int depth, LayoutResult& result, bool horizontal)
    {
        const LayoutSpec& layout = parent.layout;
        struct Item {
            const Component* component;
            Size size;
            double grow;
        };
        std::vector<Item> items;
        std::vector<const Component*> hidden;
        for (const auto& source : parent.children) {
            const Component& child = hold(source);
            if (!child.visible) {
                hidden.push_back(&child);
                continue;
            }
            Size size = measure(child, horizontal ? 0 : content.width, 0);
            const SizeSpec& main = horizontal ? child.width : child.height;
            double grow = child.grow;
            if (main.mode == SizeMode::Fill && grow == 0)
                grow = 1;
            items.push_back({&child, size, grow});
        }

        double mainSize = horizontal ? content.width : content.height;
        double crossSize = horizontal ? content.height : content.width;
        double used = layout.spacing * std::max(0.0, static_cast<double>(items.size()) - 1);
        double growTotal = 0;
        for (auto& item : items) {
            const Component& child = *item.component;
            double margins = horizontal ? child.margin.left + child.margin.right : child.margin.top + child.margin.bottom;
            const SizeSpec& main = horizontal ? child.width : child.height;
            if (main.mode == SizeMode::Fill) {
                used += margins;
                if (horizontal)
                    item.size.width = main.min.value_or(0);
                else
                    item.size.height = main.min.value_or(0);
                used += horizontal ? item.size.width : item.size.height;
            } else {
                used += (horizontal ? item.size.width : item.size.height) + margins;
            }
            growTotal += item.grow;
        }

        double remaining = mainSize - used;
        if (remaining > 0 && growTotal > 0) {
            double leftover = remaining;
            for (auto& item : items) {
                if (item.grow <= 0)
                    continue;
                double share = remaining * item.grow / growTotal;
                const SizeSpec& main = horizontal ? item.component->width : item.component->height;
                double& target = horizontal ? item.size.width : item.size.height;
                double next = clampSize(target + share, main);
                leftover -= next - target;
                target = next;
            }
            remaining = std::max(0.0, leftover);
        } else if (remaining < 0) {
            double shrinkTotal = 0;
            for (const auto& item : items)
                shrinkTotal += item.component->shrink * (horizontal ? item.size.width : item.size.height);
            if (shrinkTotal > 0 && layout.kind != LayoutKind::Scroll) {
                for (auto& item : items) {
                    double& target = horizontal ? item.size.width : item.size.height;
                    const SizeSpec& main = horizontal ? item.component->width : item.component->height;
                    double reduction = -remaining * item.component->shrink * target / shrinkTotal;
                    target = clampSize(std::max(0.0, target - reduction), main);
                }
            }
            remaining = 0;
        }

        double offset = 0;
        double gap = layout.spacing;
        int count = static_cast<int>(items.size());
        switch (layout.justify) {
        case Justify::Start:
            break;
        case Justify::Center:
            offset = remaining / 2;
            break;
        case Justify::End:
            offset = remaining;
            break;
        case Justify::SpaceBetween:
            if (count > 1)
                gap += remaining / (count - 1);
            break;
        case Justify::SpaceAround:
            if (count > 0) {
                gap += remaining / count;
                offset = remaining / count / 2;
            }
            break;
        case Justify::SpaceEvenly:
            if (count > 0) {
                gap += remaining / (count + 1);
                offset = remaining / (count + 1);
            }
            break;
        }

        double cursor = (horizontal ? content.x : content.y) + offset;
        for (auto& item : items) {
            const Component& child = *item.component;
            Align align = child.alignSelf.value_or(layout.align);
            double marginBefore = horizontal ? child.margin.left : child.margin.top;
            double marginAfter = horizontal ? child.margin.right : child.margin.bottom;
            double crossBefore = horizontal ? child.margin.top : child.margin.left;
            double crossAfter = horizontal ? child.margin.bottom : child.margin.right;
            const SizeSpec& crossSpec = horizontal ? child.height : child.width;
            double cross = horizontal ? item.size.height : item.size.width;
            double available = crossSize - crossBefore - crossAfter;
            if (crossSpec.mode == SizeMode::Fill || (align == Align::Stretch && crossSpec.mode == SizeMode::Hug))
                cross = clampSize(available, crossSpec);
            if (crossSpec.mode == SizeMode::Percent)
                cross = clampSize(crossSize * crossSpec.value / 100.0, crossSpec);
            double crossOffset = crossBefore;
            if (align == Align::Center)
                crossOffset = crossBefore + (available - cross) / 2;
            else if (align == Align::End)
                crossOffset = crossSize - crossAfter - cross;
            double mainValue = horizontal ? item.size.width : item.size.height;
            cursor += marginBefore;
            Rect frame = horizontal ? Rect{cursor, content.y + crossOffset, mainValue, cross}
                                    : Rect{content.x + crossOffset, cursor, cross, mainValue};
            if (!horizontal && child.height.mode == SizeMode::Hug && child.width.mode != SizeMode::Fixed) {
                Size remeasured = measure(child, cross, 0);
                frame.height = remeasured.height;
                mainValue = frame.height;
            }
            place(child, frame, parentIndex, depth + 1, result);
            cursor += mainValue + marginAfter + gap;
        }
        for (const Component* child : hidden)
            placeHidden(*child, content, parentIndex, depth + 1, result);
    }

    void placeWrapped(const Component& parent, const Rect& content, int parentIndex, int depth, LayoutResult& result)
    {
        const LayoutSpec& layout = parent.layout;
        std::vector<const Component*> children;
        std::vector<const Component*> hidden;
        for (const auto& source : parent.children) {
            const Component& child = hold(source);
            (child.visible ? children : hidden).push_back(&child);
        }
        auto rows = wrapRows(children, layout, content.width);
        double y = content.y;
        double rowGap = layout.rowSpacing.value_or(layout.spacing);
        for (const auto& row : rows) {
            double remaining = content.width - row.width;
            double x = content.x;
            double gap = layout.spacing;
            int count = static_cast<int>(row.items.size());
            switch (layout.justify) {
            case Justify::Center:
                x += remaining / 2;
                break;
            case Justify::End:
                x += remaining;
                break;
            case Justify::SpaceBetween:
                if (count > 1)
                    gap += remaining / (count - 1);
                break;
            case Justify::SpaceAround:
                gap += remaining / count;
                x += remaining / count / 2;
                break;
            case Justify::SpaceEvenly:
                gap += remaining / (count + 1);
                x += remaining / (count + 1);
                break;
            default:
                break;
            }
            for (std::size_t i = 0; i < row.items.size(); ++i) {
                const Component& child = *row.items[i];
                Size size = row.sizes[i];
                x += child.margin.left;
                double crossOffset = child.margin.top;
                Align align = child.alignSelf.value_or(layout.align);
                double available = row.height - child.margin.top - child.margin.bottom;
                if (align == Align::Center)
                    crossOffset += (available - size.height) / 2;
                else if (align == Align::End)
                    crossOffset += available - size.height;
                else if (align == Align::Stretch && child.height.mode != SizeMode::Fixed)
                    size.height = available;
                place(child, Rect{x, y + crossOffset, size.width, size.height}, parentIndex, depth + 1, result);
                x += size.width + child.margin.right + gap;
            }
            y += row.height + rowGap;
        }
        for (const Component* child : hidden)
            placeHidden(*child, content, parentIndex, depth + 1, result);
    }

    void placeGrid(const Component& parent, const Rect& content, int parentIndex, int depth, LayoutResult& result)
    {
        const LayoutSpec& layout = parent.layout;
        auto [columnWidths, rowHeights] = gridTracks(parent, content.width, content.height);
        double spacing = layout.spacing;
        double rowSpacing = layout.rowSpacing.value_or(layout.spacing);
        std::vector<double> columnX(columnWidths.size(), content.x);
        for (std::size_t i = 1; i < columnWidths.size(); ++i)
            columnX[i] = columnX[i - 1] + columnWidths[i - 1] + spacing;
        std::vector<double> rowY(rowHeights.size(), content.y);
        for (std::size_t i = 1; i < rowHeights.size(); ++i)
            rowY[i] = rowY[i - 1] + rowHeights[i - 1] + rowSpacing;

        int cursorColumn = 0;
        int cursorRow = 0;
        int columns = static_cast<int>(columnWidths.size());
        for (const auto& source : parent.children) {
            const Component& child = hold(source);
            if (!child.visible) {
                placeHidden(child, content, parentIndex, depth + 1, result);
                continue;
            }
            int column = child.grid.column;
            int row = child.grid.row;
            if (source.grid == GridPlacement{}) {
                column = cursorColumn;
                row = cursorRow;
                ++cursorColumn;
                if (cursorColumn >= columns) {
                    cursorColumn = 0;
                    ++cursorRow;
                }
            }
            column = std::clamp(column, 0, columns - 1);
            row = std::clamp(row, 0, static_cast<int>(rowHeights.size()) - 1);
            int endColumn = std::min(column + child.grid.columnSpan, columns);
            int endRow = std::min(row + child.grid.rowSpan, static_cast<int>(rowHeights.size()));
            double cellWidth = columnX[static_cast<std::size_t>(endColumn - 1)] + columnWidths[static_cast<std::size_t>(endColumn - 1)]
                - columnX[static_cast<std::size_t>(column)];
            double cellHeight = rowY[static_cast<std::size_t>(endRow - 1)] + rowHeights[static_cast<std::size_t>(endRow - 1)]
                - rowY[static_cast<std::size_t>(row)];
            double innerWidth = cellWidth - child.margin.left - child.margin.right;
            double innerHeight = cellHeight - child.margin.top - child.margin.bottom;
            Size size = measure(child, innerWidth, innerHeight);
            double width = child.width.mode == SizeMode::Fill || (child.width.mode == SizeMode::Hug && layout.align == Align::Stretch)
                ? innerWidth
                : size.width;
            double height = child.height.mode == SizeMode::Fill ? innerHeight : size.height;
            double x = columnX[static_cast<std::size_t>(column)] + child.margin.left;
            double y = rowY[static_cast<std::size_t>(row)] + child.margin.top;
            Align align = child.alignSelf.value_or(layout.align);
            if (align == Align::Center) {
                x += (innerWidth - width) / 2;
                y += (innerHeight - height) / 2;
            } else if (align == Align::End) {
                x += innerWidth - width;
                y += innerHeight - height;
            }
            place(child, Rect{x, y, width, height}, parentIndex, depth + 1, result);
        }
    }

    LayoutOptions options_;
    const ITextMeasurer* measurer_ = nullptr;
    std::shared_ptr<std::deque<Component>> store_;
};

}

LayoutResult computeLayout(const Component& root, Size viewport, const LayoutOptions& options)
{
    Engine engine(options);
    return engine.run(root, viewport);
}

LayoutResult computeScreenLayout(const Screen& screen, const LayoutOptions& options)
{
    Size viewport{screen.root.width.mode == SizeMode::Fixed ? screen.root.width.value : 390,
                  screen.root.height.mode == SizeMode::Fixed ? screen.root.height.value : 844};
    LayoutOptions effective = options;
    if (effective.breakpoint.empty() && options.model != nullptr)
        effective.breakpoint = breakpointForWidth(*options.model, viewport.width);
    return computeLayout(screen.root, viewport, effective);
}

}
