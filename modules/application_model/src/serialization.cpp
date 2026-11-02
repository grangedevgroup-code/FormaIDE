#include "forma/ir/serialization.h"

#include <algorithm>
#include <set>

#include "forma/core/text.h"

namespace forma::ir {

namespace {

class Reader {
public:
    explicit Reader(std::string path = {}) : path_(std::move(path)) {}

    void error(std::string_view key, std::string_view expected)
    {
        if (errors_.size() < 8)
            errors_.push_back(path_ + "." + std::string(key) + ": expected " + std::string(expected));
    }

    void number(const Json& json, const char* key, double& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (it->is_number())
            out = it->get<double>();
        else
            error(key, "a number");
    }

    void optionalNumber(const Json& json, const char* key, std::optional<double>& out)
    {
        auto it = json.find(key);
        if (it == json.end() || it->is_null())
            return;
        if (it->is_number())
            out = it->get<double>();
        else
            error(key, "a number");
    }

    void integer(const Json& json, const char* key, int& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (it->is_number())
            out = it->get<int>();
        else
            error(key, "a whole number");
    }

    void optionalInteger(const Json& json, const char* key, std::optional<int>& out)
    {
        auto it = json.find(key);
        if (it == json.end() || it->is_null())
            return;
        if (it->is_number())
            out = it->get<int>();
        else
            error(key, "a whole number");
    }

    void boolean(const Json& json, const char* key, bool& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (it->is_boolean())
            out = it->get<bool>();
        else
            error(key, "true or false");
    }

    void optionalBoolean(const Json& json, const char* key, std::optional<bool>& out)
    {
        auto it = json.find(key);
        if (it == json.end() || it->is_null())
            return;
        if (it->is_boolean())
            out = it->get<bool>();
        else
            error(key, "true or false");
    }

    void string(const Json& json, const char* key, std::string& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (it->is_string())
            out = it->get<std::string>();
        else
            error(key, "text");
    }

    void optionalString(const Json& json, const char* key, std::optional<std::string>& out)
    {
        auto it = json.find(key);
        if (it == json.end() || it->is_null())
            return;
        if (it->is_string())
            out = it->get<std::string>();
        else
            error(key, "text");
    }

    void stringList(const Json& json, const char* key, std::vector<std::string>& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (!it->is_array()) {
            error(key, "a list");
            return;
        }
        for (const auto& item : *it) {
            if (item.is_string())
                out.push_back(item.get<std::string>());
            else
                error(key, "a list of text");
        }
    }

    void stringMap(const Json& json, const char* key, std::map<std::string, std::string>& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (!it->is_object()) {
            error(key, "an object");
            return;
        }
        for (auto item = it->begin(); item != it->end(); ++item) {
            if (item->is_string())
                out[item.key()] = item->get<std::string>();
            else
                error(key, "an object of text values");
        }
    }

    void jsonMap(const Json& json, const char* key, std::map<std::string, Json>& out)
    {
        auto it = json.find(key);
        if (it == json.end())
            return;
        if (!it->is_object()) {
            error(key, "an object");
            return;
        }
        for (auto item = it->begin(); item != it->end(); ++item)
            out[item.key()] = *item;
    }

    bool ok() const { return errors_.empty(); }
    Error failure() const { return makeError("ir.parse", text::join(errors_, "; ")); }

    void merge(const Reader& other)
    {
        for (const auto& item : other.errors_) {
            if (errors_.size() < 8)
                errors_.push_back(item);
        }
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::vector<std::string> errors_;
};

template <class Enum>
struct EnumName {
    Enum value;
    const char* name;
};

template <class Enum, std::size_t N>
void readEnum(Reader& reader, const Json& json, const char* key, Enum& out, const EnumName<Enum> (&names)[N])
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    if (it->is_string()) {
        for (const auto& item : names) {
            if (*it == item.name) {
                out = item.value;
                return;
            }
        }
    }
    std::string expected = "one of";
    for (const auto& item : names)
        expected += std::string(" ") + item.name;
    reader.error(key, expected);
}

constexpr EnumName<LayoutKind> kLayoutKinds[] = {
    {LayoutKind::Absolute, "absolute"}, {LayoutKind::Vertical, "vertical"}, {LayoutKind::Horizontal, "horizontal"},
    {LayoutKind::Grid, "grid"},         {LayoutKind::Flex, "flex"},         {LayoutKind::Scroll, "scroll"},
    {LayoutKind::Constraint, "constraint"}};
constexpr EnumName<Align> kAligns[] = {
    {Align::Start, "start"}, {Align::Center, "center"}, {Align::End, "end"}, {Align::Stretch, "stretch"}};
constexpr EnumName<Justify> kJustifies[] = {{Justify::Start, "start"},
                                            {Justify::Center, "center"},
                                            {Justify::End, "end"},
                                            {Justify::SpaceBetween, "spaceBetween"},
                                            {Justify::SpaceAround, "spaceAround"},
                                            {Justify::SpaceEvenly, "spaceEvenly"}};
constexpr EnumName<ScrollAxis> kScrollAxes[] = {
    {ScrollAxis::Vertical, "vertical"}, {ScrollAxis::Horizontal, "horizontal"}, {ScrollAxis::Both, "both"}};

Json insetsToJson(const EdgeInsets& insets)
{
    if (insets.isUniform())
        return insets.top;
    return Json::array({insets.top, insets.right, insets.bottom, insets.left});
}

void readInsets(Reader& reader, const Json& json, const char* key, EdgeInsets& out)
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    if (it->is_number()) {
        double value = it->get<double>();
        out = EdgeInsets{value, value, value, value};
    } else if (it->is_array() && it->size() == 4 && std::all_of(it->begin(), it->end(), [](const Json& v) { return v.is_number(); })) {
        out = EdgeInsets{(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>(), (*it)[3].get<double>()};
    } else if (it->is_array() && it->size() == 2 && std::all_of(it->begin(), it->end(), [](const Json& v) { return v.is_number(); })) {
        double vertical = (*it)[0].get<double>();
        double horizontal = (*it)[1].get<double>();
        out = EdgeInsets{vertical, horizontal, vertical, horizontal};
    } else {
        reader.error(key, "a number or a list of four numbers");
    }
}

Json sizeToJson(const SizeSpec& size)
{
    Json value;
    bool hasBounds = size.min.has_value() || size.max.has_value();
    switch (size.mode) {
    case SizeMode::Fixed:
        value = size.value;
        break;
    case SizeMode::Fill:
        value = "fill";
        break;
    case SizeMode::Hug:
        value = "hug";
        break;
    case SizeMode::Percent:
        value = text::formatNumber(size.value) + "%";
        break;
    }
    if (!hasBounds)
        return value;
    Json object = Json::object();
    object["mode"] = sizeModeName(size.mode);
    if (size.mode == SizeMode::Fixed || size.mode == SizeMode::Percent)
        object["value"] = size.value;
    if (size.min)
        object["min"] = *size.min;
    if (size.max)
        object["max"] = *size.max;
    return object;
}

void readSize(Reader& reader, const Json& json, const char* key, SizeSpec& out)
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    auto fromString = [&](const std::string& value) {
        if (value == "fill")
            out.mode = SizeMode::Fill;
        else if (value == "hug")
            out.mode = SizeMode::Hug;
        else if (!value.empty() && value.back() == '%') {
            try {
                out.mode = SizeMode::Percent;
                out.value = std::stod(value.substr(0, value.size() - 1));
            } catch (const std::exception&) {
                reader.error(key, "a percentage");
            }
        } else {
            reader.error(key, "a number, fill, hug or a percentage");
        }
    };
    if (it->is_number()) {
        out.mode = SizeMode::Fixed;
        out.value = it->get<double>();
    } else if (it->is_string()) {
        fromString(it->get<std::string>());
    } else if (it->is_object()) {
        std::string mode = jsonString(*it, "mode", "fixed");
        if (mode == "fixed") {
            out.mode = SizeMode::Fixed;
            out.value = jsonNumber(*it, "value");
        } else if (mode == "fill") {
            out.mode = SizeMode::Fill;
        } else if (mode == "hug") {
            out.mode = SizeMode::Hug;
        } else if (mode == "percent") {
            out.mode = SizeMode::Percent;
            out.value = jsonNumber(*it, "value");
        } else {
            reader.error(key, "mode fixed, fill, hug or percent");
        }
        reader.optionalNumber(*it, "min", out.min);
        reader.optionalNumber(*it, "max", out.max);
    } else {
        reader.error(key, "a size");
    }
}

Json trackToJson(const GridTrack& track)
{
    switch (track.kind) {
    case TrackKind::Fixed:
        return track.value;
    case TrackKind::Auto:
        return "auto";
    case TrackKind::Fraction:
        return text::formatNumber(track.value) + "fr";
    }
    return "auto";
}

void readTracks(Reader& reader, const Json& json, const char* key, std::vector<GridTrack>& out)
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    if (!it->is_array()) {
        reader.error(key, "a list of tracks");
        return;
    }
    for (const auto& item : *it) {
        GridTrack track;
        if (item.is_number()) {
            track.kind = TrackKind::Fixed;
            track.value = item.get<double>();
        } else if (item.is_string()) {
            auto value = item.get<std::string>();
            if (value == "auto") {
                track.kind = TrackKind::Auto;
            } else if (text::endsWith(value, "fr")) {
                try {
                    track.kind = TrackKind::Fraction;
                    track.value = std::stod(value.substr(0, value.size() - 2));
                } catch (const std::exception&) {
                    reader.error(key, "a track such as 1fr");
                }
            } else {
                reader.error(key, "a number, auto or 1fr");
            }
        } else {
            reader.error(key, "a number, auto or 1fr");
        }
        out.push_back(track);
    }
}

Json layoutToJson(const LayoutSpec& layout)
{
    Json json = Json::object();
    json["type"] = layoutKindName(layout.kind);
    if (layout.spacing != 0)
        json["spacing"] = layout.spacing;
    if (layout.rowSpacing)
        json["rowSpacing"] = *layout.rowSpacing;
    if (!layout.padding.isZero())
        json["padding"] = insetsToJson(layout.padding);
    if (layout.align != Align::Start)
        json["align"] = alignName(layout.align);
    if (layout.justify != Justify::Start)
        json["justify"] = justifyName(layout.justify);
    if (layout.wrap)
        json["wrap"] = true;
    if (!layout.columns.empty()) {
        Json tracks = Json::array();
        for (const auto& track : layout.columns)
            tracks.push_back(trackToJson(track));
        json["columns"] = tracks;
    }
    if (!layout.rows.empty()) {
        Json tracks = Json::array();
        for (const auto& track : layout.rows)
            tracks.push_back(trackToJson(track));
        json["rows"] = tracks;
    }
    if (layout.kind == LayoutKind::Scroll && layout.scroll != ScrollAxis::Vertical)
        json["scroll"] = layout.scroll == ScrollAxis::Horizontal ? "horizontal" : "both";
    return json;
}

void readLayout(Reader& reader, const Json& json, LayoutSpec& out)
{
    readEnum(reader, json, "type", out.kind, kLayoutKinds);
    reader.number(json, "spacing", out.spacing);
    reader.optionalNumber(json, "rowSpacing", out.rowSpacing);
    readInsets(reader, json, "padding", out.padding);
    readEnum(reader, json, "align", out.align, kAligns);
    readEnum(reader, json, "justify", out.justify, kJustifies);
    reader.boolean(json, "wrap", out.wrap);
    readTracks(reader, json, "columns", out.columns);
    readTracks(reader, json, "rows", out.rows);
    readEnum(reader, json, "scroll", out.scroll, kScrollAxes);
}

Json shadowToJson(const Shadow& shadow)
{
    Json json = Json::object();
    json["x"] = shadow.x;
    json["y"] = shadow.y;
    json["blur"] = shadow.blur;
    if (shadow.spread != 0)
        json["spread"] = shadow.spread;
    json["color"] = shadow.color;
    return json;
}

Shadow shadowFromJson(Reader& reader, const Json& json)
{
    Shadow shadow;
    reader.number(json, "x", shadow.x);
    reader.number(json, "y", shadow.y);
    reader.number(json, "blur", shadow.blur);
    reader.number(json, "spread", shadow.spread);
    reader.string(json, "color", shadow.color);
    return shadow;
}

Json styleToJson(const Style& style)
{
    Json json = Json::object();
    if (style.background)
        json["background"] = *style.background;
    if (style.borderColor)
        json["borderColor"] = *style.borderColor;
    if (style.borderWidth != 0)
        json["borderWidth"] = style.borderWidth;
    if (!style.borderStyle.empty())
        json["borderStyle"] = style.borderStyle;
    if (style.radius != 0)
        json["radius"] = style.radius;
    if (style.shadow)
        json["shadow"] = shadowToJson(*style.shadow);
    if (style.opacity != 1)
        json["opacity"] = style.opacity;
    if (style.clip)
        json["clip"] = true;
    return json;
}

void readStyle(Reader& reader, const Json& json, Style& out)
{
    reader.optionalString(json, "background", out.background);
    reader.optionalString(json, "borderColor", out.borderColor);
    reader.number(json, "borderWidth", out.borderWidth);
    reader.string(json, "borderStyle", out.borderStyle);
    reader.number(json, "radius", out.radius);
    auto shadow = json.find("shadow");
    if (shadow != json.end() && shadow->is_object())
        out.shadow = shadowFromJson(reader, *shadow);
    reader.number(json, "opacity", out.opacity);
    reader.boolean(json, "clip", out.clip);
}

Json textStyleToJson(const TextStyle& style)
{
    Json json = Json::object();
    if (!style.ref.empty())
        json["ref"] = style.ref;
    if (!style.font.empty())
        json["font"] = style.font;
    if (style.size)
        json["size"] = *style.size;
    if (style.weight)
        json["weight"] = *style.weight;
    if (style.italic)
        json["italic"] = *style.italic;
    if (style.underline)
        json["underline"] = *style.underline;
    if (style.color)
        json["color"] = *style.color;
    if (style.align)
        json["align"] = *style.align;
    if (style.lineHeight)
        json["lineHeight"] = *style.lineHeight;
    if (style.letterSpacing)
        json["letterSpacing"] = *style.letterSpacing;
    return json;
}

void readTextStyle(Reader& reader, const Json& json, TextStyle& out)
{
    reader.string(json, "ref", out.ref);
    reader.string(json, "font", out.font);
    reader.optionalNumber(json, "size", out.size);
    reader.optionalInteger(json, "weight", out.weight);
    reader.optionalBoolean(json, "italic", out.italic);
    reader.optionalBoolean(json, "underline", out.underline);
    reader.optionalString(json, "color", out.color);
    reader.optionalString(json, "align", out.align);
    reader.optionalNumber(json, "lineHeight", out.lineHeight);
    reader.optionalNumber(json, "letterSpacing", out.letterSpacing);
}

const std::set<std::string>& knownComponentKeys()
{
    static const std::set<std::string> keys = {
        "id", "type", "name", "visible", "locked", "x", "y", "width", "height", "margin", "grow", "shrink", "alignSelf",
        "pins", "grid", "layout", "style", "typography", "properties", "events", "bindings", "tokens", "variants", "states",
        "platform", "instanceOf", "overrides", "codeManaged", "children"};
    return keys;
}

void readComponent(Reader& reader, const Json& json, Component& out)
{
    if (!json.is_object()) {
        reader.error("component", "an object");
        return;
    }
    reader.string(json, "id", out.id);
    reader.string(json, "type", out.type);
    reader.string(json, "name", out.name);
    reader.boolean(json, "visible", out.visible);
    reader.boolean(json, "locked", out.locked);
    reader.number(json, "x", out.x);
    reader.number(json, "y", out.y);
    readSize(reader, json, "width", out.width);
    readSize(reader, json, "height", out.height);
    readInsets(reader, json, "margin", out.margin);
    reader.number(json, "grow", out.grow);
    reader.number(json, "shrink", out.shrink);
    auto alignSelf = json.find("alignSelf");
    if (alignSelf != json.end() && !alignSelf->is_null()) {
        Align value = Align::Start;
        readEnum(reader, json, "alignSelf", value, kAligns);
        out.alignSelf = value;
    }
    auto pins = json.find("pins");
    if (pins != json.end() && pins->is_object()) {
        reader.optionalNumber(*pins, "left", out.pins.left);
        reader.optionalNumber(*pins, "right", out.pins.right);
        reader.optionalNumber(*pins, "top", out.pins.top);
        reader.optionalNumber(*pins, "bottom", out.pins.bottom);
        reader.optionalNumber(*pins, "centerX", out.pins.centerX);
        reader.optionalNumber(*pins, "centerY", out.pins.centerY);
    }
    auto grid = json.find("grid");
    if (grid != json.end() && grid->is_object()) {
        reader.integer(*grid, "column", out.grid.column);
        reader.integer(*grid, "row", out.grid.row);
        reader.integer(*grid, "columnSpan", out.grid.columnSpan);
        reader.integer(*grid, "rowSpan", out.grid.rowSpan);
    }
    auto layout = json.find("layout");
    if (layout != json.end() && layout->is_object())
        readLayout(reader, *layout, out.layout);
    auto style = json.find("style");
    if (style != json.end() && style->is_object())
        readStyle(reader, *style, out.style);
    auto typography = json.find("typography");
    if (typography != json.end() && typography->is_object())
        readTextStyle(reader, *typography, out.typography);
    auto properties = json.find("properties");
    if (properties != json.end()) {
        if (properties->is_object())
            out.properties = *properties;
        else
            reader.error("properties", "an object");
    }
    reader.stringMap(json, "events", out.events);
    auto bindings = json.find("bindings");
    if (bindings != json.end() && bindings->is_object()) {
        for (auto item = bindings->begin(); item != bindings->end(); ++item) {
            Binding binding;
            if (item->is_string()) {
                binding.source = item->get<std::string>();
            } else if (item->is_object()) {
                reader.string(*item, "source", binding.source);
                reader.string(*item, "mode", binding.mode);
            } else {
                reader.error("bindings", "text or an object");
            }
            out.bindings[item.key()] = binding;
        }
    }
    reader.stringMap(json, "tokens", out.tokens);
    reader.jsonMap(json, "variants", out.variants);
    reader.jsonMap(json, "states", out.states);
    reader.jsonMap(json, "platform", out.platform);
    reader.string(json, "instanceOf", out.instanceOf);
    auto overrides = json.find("overrides");
    if (overrides != json.end() && overrides->is_object())
        out.overrides = *overrides;
    reader.string(json, "codeManaged", out.codeManaged);
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (knownComponentKeys().count(item.key()) == 0)
            out.extra[item.key()] = *item;
    }
    auto children = json.find("children");
    if (children != json.end()) {
        if (!children->is_array()) {
            reader.error("children", "a list");
            return;
        }
        for (const auto& child : *children) {
            Component parsed;
            Reader childReader(reader.path() + "/" + jsonString(child, "id", "?"));
            readComponent(childReader, child, parsed);
            reader.merge(childReader);
            out.children.push_back(std::move(parsed));
        }
    }
}

Json tokensToJson(const DesignTokens& tokens)
{
    Json json = Json::object();
    if (!tokens.colors.empty())
        json["colors"] = tokens.colors;
    if (!tokens.typography.empty()) {
        Json styles = Json::object();
        for (const auto& [name, style] : tokens.typography)
            styles[name] = textStyleToJson(style);
        json["typography"] = styles;
    }
    if (!tokens.spacing.empty())
        json["spacing"] = tokens.spacing;
    if (!tokens.radius.empty())
        json["radius"] = tokens.radius;
    if (!tokens.borders.empty())
        json["borders"] = tokens.borders;
    if (!tokens.shadows.empty()) {
        Json shadows = Json::object();
        for (const auto& [name, shadow] : tokens.shadows)
            shadows[name] = shadowToJson(shadow);
        json["shadows"] = shadows;
    }
    if (!tokens.icons.empty())
        json["icons"] = tokens.icons;
    return json;
}

void readNumberMap(Reader& reader, const Json& json, const char* key, std::map<std::string, double>& out)
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    if (!it->is_object()) {
        reader.error(key, "an object");
        return;
    }
    for (auto item = it->begin(); item != it->end(); ++item) {
        if (item->is_number())
            out[item.key()] = item->get<double>();
        else
            reader.error(key, "an object of numbers");
    }
}

void readTokens(Reader& reader, const Json& json, DesignTokens& out)
{
    reader.stringMap(json, "colors", out.colors);
    auto typography = json.find("typography");
    if (typography != json.end() && typography->is_object()) {
        for (auto item = typography->begin(); item != typography->end(); ++item) {
            TextStyle style;
            readTextStyle(reader, *item, style);
            out.typography[item.key()] = style;
        }
    }
    readNumberMap(reader, json, "spacing", out.spacing);
    readNumberMap(reader, json, "radius", out.radius);
    readNumberMap(reader, json, "borders", out.borders);
    auto shadows = json.find("shadows");
    if (shadows != json.end() && shadows->is_object()) {
        for (auto item = shadows->begin(); item != shadows->end(); ++item)
            out.shadows[item.key()] = shadowFromJson(reader, *item);
    }
    reader.stringMap(json, "icons", out.icons);
}

template <class Item, class Parse>
void readList(Reader& reader, const Json& json, const char* key, std::vector<Item>& out, Parse parse)
{
    auto it = json.find(key);
    if (it == json.end())
        return;
    if (!it->is_array()) {
        reader.error(key, "a list");
        return;
    }
    for (const auto& element : *it) {
        Item item;
        parse(element, item);
        out.push_back(std::move(item));
    }
}

const std::set<std::string>& knownApplicationKeys()
{
    static const std::set<std::string> keys = {
        "formaIr", "meta", "modules", "screens", "components", "themes", "activeTheme", "breakpoints", "navigation",
        "state", "dataModels", "actions", "assets", "dependencies", "buildConfigurations", "platforms"};
    return keys;
}

Json applicationCommonJson(const ApplicationModel& model)
{
    Json json = Json::object();
    json["formaIr"] = kFormatVersion;
    json["meta"] = Json{{"id", model.meta.id}, {"name", model.meta.name}, {"bundleId", model.meta.bundleId},
                        {"version", model.meta.version}, {"description", model.meta.description}};
    json["activeTheme"] = model.activeTheme;
    if (!model.modules.empty()) {
        Json list = Json::array();
        for (const auto& item : model.modules)
            list.push_back(Json{{"id", item.id}, {"name", item.name}, {"kind", item.kind}, {"path", item.path}});
        json["modules"] = list;
    }
    {
        Json list = Json::array();
        for (const auto& item : model.breakpoints) {
            Json entry{{"id", item.id}, {"name", item.name}, {"minWidth", item.minWidth}};
            if (item.maxWidth)
                entry["maxWidth"] = *item.maxWidth;
            list.push_back(entry);
        }
        json["breakpoints"] = list;
    }
    {
        Json navigation = Json::object();
        navigation["kind"] = model.navigation.kind;
        navigation["start"] = model.navigation.start;
        Json routes = Json::array();
        for (const auto& route : model.navigation.routes) {
            Json entry{{"id", route.id}, {"from", route.from}, {"to", route.to}};
            if (!route.transition.empty())
                entry["transition"] = route.transition;
            routes.push_back(entry);
        }
        navigation["routes"] = routes;
        json["navigation"] = navigation;
    }
    if (!model.state.empty()) {
        Json list = Json::array();
        for (const auto& item : model.state) {
            Json entry{{"name", item.name}, {"type", item.type}, {"initial", item.initial}};
            if (item.scope != "app")
                entry["scope"] = item.scope;
            list.push_back(entry);
        }
        json["state"] = list;
    }
    if (!model.dataModels.empty()) {
        Json list = Json::array();
        for (const auto& item : model.dataModels) {
            Json fields = Json::array();
            for (const auto& field : item.fields)
                fields.push_back(Json{{"name", field.name}, {"type", field.type}, {"required", field.required}});
            list.push_back(Json{{"name", item.name}, {"fields", fields}});
        }
        json["dataModels"] = list;
    }
    if (!model.actions.empty()) {
        Json list = Json::array();
        for (const auto& item : model.actions) {
            Json entry{{"id", item.id}, {"kind", item.kind}};
            if (!item.target.empty())
                entry["target"] = item.target;
            if (!item.value.is_null())
                entry["value"] = item.value;
            if (!item.steps.empty())
                entry["steps"] = item.steps;
            list.push_back(entry);
        }
        json["actions"] = list;
    }
    if (!model.assets.empty()) {
        Json list = Json::array();
        for (const auto& item : model.assets) {
            Json entry{{"id", item.id}, {"path", item.path}, {"kind", item.kind}};
            if (item.width > 0)
                entry["width"] = item.width;
            if (item.height > 0)
                entry["height"] = item.height;
            if (!item.hash.empty())
                entry["hash"] = item.hash;
            list.push_back(entry);
        }
        json["assets"] = list;
    }
    if (!model.dependencies.empty()) {
        Json list = Json::array();
        for (const auto& item : model.dependencies)
            list.push_back(Json{{"name", item.name}, {"version", item.version}, {"ecosystem", item.ecosystem}});
        json["dependencies"] = list;
    }
    {
        Json list = Json::array();
        for (const auto& item : model.buildConfigurations) {
            Json entry{{"id", item.id}, {"name", item.name}, {"kind", item.kind}};
            if (!item.architecture.empty())
                entry["architecture"] = item.architecture;
            if (!item.minimumOs.empty())
                entry["minimumOs"] = item.minimumOs;
            if (!item.compiler.empty())
                entry["compiler"] = item.compiler;
            if (!item.optimization.empty())
                entry["optimization"] = item.optimization;
            if (!item.flags.empty())
                entry["flags"] = item.flags;
            if (!item.environment.empty())
                entry["environment"] = item.environment;
            if (!item.outputDirectory.empty())
                entry["outputDirectory"] = item.outputDirectory;
            list.push_back(entry);
        }
        json["buildConfigurations"] = list;
    }
    if (!model.platforms.empty()) {
        Json list = Json::array();
        for (const auto& item : model.platforms)
            list.push_back(Json{{"platform", item.platform}, {"minimumVersion", item.minimumVersion}, {"capabilities", item.capabilities}});
        json["platforms"] = list;
    }
    for (auto item = model.extra.begin(); item != model.extra.end(); ++item)
        json[item.key()] = *item;
    return json;
}

void readApplicationCommon(Reader& reader, const Json& json, ApplicationModel& model)
{
    auto meta = json.find("meta");
    if (meta != json.end() && meta->is_object()) {
        reader.string(*meta, "id", model.meta.id);
        reader.string(*meta, "name", model.meta.name);
        reader.string(*meta, "bundleId", model.meta.bundleId);
        reader.string(*meta, "version", model.meta.version);
        reader.string(*meta, "description", model.meta.description);
    }
    reader.string(json, "activeTheme", model.activeTheme);
    readList(reader, json, "modules", model.modules, [&](const Json& element, ModuleDefinition& item) {
        reader.string(element, "id", item.id);
        reader.string(element, "name", item.name);
        reader.string(element, "kind", item.kind);
        reader.string(element, "path", item.path);
    });
    readList(reader, json, "breakpoints", model.breakpoints, [&](const Json& element, Breakpoint& item) {
        reader.string(element, "id", item.id);
        reader.string(element, "name", item.name);
        reader.number(element, "minWidth", item.minWidth);
        reader.optionalNumber(element, "maxWidth", item.maxWidth);
    });
    auto navigation = json.find("navigation");
    if (navigation != json.end() && navigation->is_object()) {
        reader.string(*navigation, "kind", model.navigation.kind);
        reader.string(*navigation, "start", model.navigation.start);
        readList(reader, *navigation, "routes", model.navigation.routes, [&](const Json& element, Route& item) {
            reader.string(element, "id", item.id);
            reader.string(element, "from", item.from);
            reader.string(element, "to", item.to);
            reader.string(element, "transition", item.transition);
        });
    }
    readList(reader, json, "state", model.state, [&](const Json& element, StateVariable& item) {
        reader.string(element, "name", item.name);
        reader.string(element, "type", item.type);
        reader.string(element, "scope", item.scope);
        if (element.contains("initial"))
            item.initial = element["initial"];
    });
    readList(reader, json, "dataModels", model.dataModels, [&](const Json& element, DataModel& item) {
        reader.string(element, "name", item.name);
        readList(reader, element, "fields", item.fields, [&](const Json& inner, DataField& field) {
            reader.string(inner, "name", field.name);
            reader.string(inner, "type", field.type);
            reader.boolean(inner, "required", field.required);
        });
    });
    readList(reader, json, "actions", model.actions, [&](const Json& element, Action& item) {
        reader.string(element, "id", item.id);
        reader.string(element, "kind", item.kind);
        reader.string(element, "target", item.target);
        if (element.contains("value"))
            item.value = element["value"];
        reader.stringList(element, "steps", item.steps);
    });
    readList(reader, json, "assets", model.assets, [&](const Json& element, Asset& item) {
        reader.string(element, "id", item.id);
        reader.string(element, "path", item.path);
        reader.string(element, "kind", item.kind);
        reader.number(element, "width", item.width);
        reader.number(element, "height", item.height);
        reader.string(element, "hash", item.hash);
    });
    readList(reader, json, "dependencies", model.dependencies, [&](const Json& element, Dependency& item) {
        reader.string(element, "name", item.name);
        reader.string(element, "version", item.version);
        reader.string(element, "ecosystem", item.ecosystem);
    });
    readList(reader, json, "buildConfigurations", model.buildConfigurations, [&](const Json& element, BuildConfiguration& item) {
        reader.string(element, "id", item.id);
        reader.string(element, "name", item.name);
        reader.string(element, "kind", item.kind);
        reader.string(element, "architecture", item.architecture);
        reader.string(element, "minimumOs", item.minimumOs);
        reader.string(element, "compiler", item.compiler);
        reader.string(element, "optimization", item.optimization);
        reader.stringList(element, "flags", item.flags);
        reader.stringMap(element, "environment", item.environment);
        reader.string(element, "outputDirectory", item.outputDirectory);
    });
    readList(reader, json, "platforms", model.platforms, [&](const Json& element, PlatformRequirement& item) {
        reader.string(element, "platform", item.platform);
        reader.string(element, "minimumVersion", item.minimumVersion);
        reader.stringList(element, "capabilities", item.capabilities);
    });
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (knownApplicationKeys().count(item.key()) == 0)
            model.extra[item.key()] = *item;
    }
}

Result<Json> readJsonFile(const IFilesystemService& filesystem, const std::string& path)
{
    auto content = filesystem.readFile(path);
    if (!content)
        return content.error();
    auto parsed = parseJson(*content);
    if (!parsed)
        return makeError("ir.parse", path + ": " + parsed.error().message);
    return parsed;
}

}

Json componentToJson(const Component& component, bool includeChildren)
{
    Json json = Json::object();
    json["id"] = component.id;
    json["type"] = component.type;
    if (!component.name.empty())
        json["name"] = component.name;
    if (!component.visible)
        json["visible"] = false;
    if (component.locked)
        json["locked"] = true;
    if (component.x != 0)
        json["x"] = component.x;
    if (component.y != 0)
        json["y"] = component.y;
    if (!(component.width == SizeSpec{}))
        json["width"] = sizeToJson(component.width);
    if (!(component.height == SizeSpec{}))
        json["height"] = sizeToJson(component.height);
    if (!component.margin.isZero())
        json["margin"] = insetsToJson(component.margin);
    if (component.grow != 0)
        json["grow"] = component.grow;
    if (component.shrink != 1)
        json["shrink"] = component.shrink;
    if (component.alignSelf)
        json["alignSelf"] = alignName(*component.alignSelf);
    if (component.pins.any()) {
        Json pins = Json::object();
        if (component.pins.left)
            pins["left"] = *component.pins.left;
        if (component.pins.right)
            pins["right"] = *component.pins.right;
        if (component.pins.top)
            pins["top"] = *component.pins.top;
        if (component.pins.bottom)
            pins["bottom"] = *component.pins.bottom;
        if (component.pins.centerX)
            pins["centerX"] = *component.pins.centerX;
        if (component.pins.centerY)
            pins["centerY"] = *component.pins.centerY;
        json["pins"] = pins;
    }
    if (!(component.grid == GridPlacement{})) {
        json["grid"] = Json{{"column", component.grid.column}, {"row", component.grid.row},
                            {"columnSpan", component.grid.columnSpan}, {"rowSpan", component.grid.rowSpan}};
    }
    if (!(component.layout == LayoutSpec{}))
        json["layout"] = layoutToJson(component.layout);
    if (!(component.style == Style{}))
        json["style"] = styleToJson(component.style);
    if (!(component.typography == TextStyle{}))
        json["typography"] = textStyleToJson(component.typography);
    if (!component.properties.empty())
        json["properties"] = component.properties;
    if (!component.events.empty())
        json["events"] = component.events;
    if (!component.bindings.empty()) {
        Json bindings = Json::object();
        for (const auto& [key, binding] : component.bindings) {
            if (binding.mode == "oneWay")
                bindings[key] = binding.source;
            else
                bindings[key] = Json{{"source", binding.source}, {"mode", binding.mode}};
        }
        json["bindings"] = bindings;
    }
    if (!component.tokens.empty())
        json["tokens"] = component.tokens;
    if (!component.variants.empty())
        json["variants"] = component.variants;
    if (!component.states.empty())
        json["states"] = component.states;
    if (!component.platform.empty())
        json["platform"] = component.platform;
    if (!component.instanceOf.empty())
        json["instanceOf"] = component.instanceOf;
    if (!component.overrides.empty())
        json["overrides"] = component.overrides;
    if (!component.codeManaged.empty())
        json["codeManaged"] = component.codeManaged;
    for (auto item = component.extra.begin(); item != component.extra.end(); ++item)
        json[item.key()] = *item;
    if (includeChildren && !component.children.empty()) {
        Json children = Json::array();
        for (const auto& child : component.children)
            children.push_back(componentToJson(child));
        json["children"] = children;
    }
    return json;
}

Result<Component> componentFromJson(const Json& json)
{
    Component component;
    Reader reader(jsonString(json, "id", "component"));
    readComponent(reader, json, component);
    if (!reader.ok())
        return reader.failure();
    return component;
}

Json screenToJson(const Screen& screen)
{
    Json json = componentToJson(screen.root);
    Json output = Json::object();
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (item.key() == "children")
            continue;
        output[item.key()] = *item;
    }
    output["canvas"] = Json{{"x", screen.canvas.x}, {"y", screen.canvas.y}};
    if (!screen.device.empty())
        output["device"] = screen.device;
    if (screen.orientation != "portrait")
        output["orientation"] = screen.orientation;
    if (json.contains("children"))
        output["children"] = json["children"];
    return output;
}

Result<Screen> screenFromJson(const Json& json)
{
    Screen screen;
    Reader reader(jsonString(json, "id", "screen"));
    Json componentJson = Json::object();
    if (!json.is_object())
        return makeError("ir.parse", "A screen must be an object");
    for (auto item = json.begin(); item != json.end(); ++item) {
        if (item.key() == "canvas" || item.key() == "device" || item.key() == "orientation")
            continue;
        componentJson[item.key()] = *item;
    }
    readComponent(reader, componentJson, screen.root);
    auto canvas = json.find("canvas");
    if (canvas != json.end() && canvas->is_object()) {
        reader.number(*canvas, "x", screen.canvas.x);
        reader.number(*canvas, "y", screen.canvas.y);
    }
    reader.string(json, "device", screen.device);
    reader.string(json, "orientation", screen.orientation);
    if (!reader.ok())
        return reader.failure();
    return screen;
}

Json definitionToJson(const ComponentDefinition& definition)
{
    Json json = componentToJson(definition.root);
    if (!definition.description.empty())
        json["description"] = definition.description;
    if (!definition.parameters.empty()) {
        Json list = Json::array();
        for (const auto& parameter : definition.parameters) {
            list.push_back(Json{{"name", parameter.name}, {"target", parameter.target}, {"key", parameter.key}, {"default", parameter.defaultValue}});
        }
        json["parameters"] = list;
    }
    return json;
}

Result<ComponentDefinition> definitionFromJson(const Json& json)
{
    ComponentDefinition definition;
    Reader reader(jsonString(json, "id", "definition"));
    if (!json.is_object())
        return makeError("ir.parse", "A component definition must be an object");
    Json componentJson = json;
    componentJson.erase("description");
    componentJson.erase("parameters");
    readComponent(reader, componentJson, definition.root);
    reader.string(json, "description", definition.description);
    readList(reader, json, "parameters", definition.parameters, [&](const Json& element, ComponentParameter& item) {
        reader.string(element, "name", item.name);
        reader.string(element, "target", item.target);
        reader.string(element, "key", item.key);
        if (element.contains("default"))
            item.defaultValue = element["default"];
    });
    if (!reader.ok())
        return reader.failure();
    return definition;
}

Json themeToJson(const Theme& theme)
{
    Json json = Json::object();
    json["id"] = theme.id;
    json["name"] = theme.name;
    json["mode"] = theme.mode;
    Json tokens = tokensToJson(theme.tokens);
    for (auto item = tokens.begin(); item != tokens.end(); ++item)
        json[item.key()] = *item;
    return json;
}

Result<Theme> themeFromJson(const Json& json)
{
    Theme theme;
    Reader reader(jsonString(json, "id", "theme"));
    if (!json.is_object())
        return makeError("ir.parse", "A theme must be an object");
    reader.string(json, "id", theme.id);
    reader.string(json, "name", theme.name);
    reader.string(json, "mode", theme.mode);
    readTokens(reader, json, theme.tokens);
    if (!reader.ok())
        return reader.failure();
    return theme;
}

Json applicationToJson(const ApplicationModel& model)
{
    Json json = applicationCommonJson(model);
    Json screens = Json::array();
    for (const auto& screen : model.screens)
        screens.push_back(screen.root.id);
    json["screens"] = screens;
    Json components = Json::array();
    for (const auto& definition : model.components)
        components.push_back(definition.root.id);
    json["components"] = components;
    Json themes = Json::array();
    for (const auto& theme : model.themes)
        themes.push_back(theme.id);
    json["themes"] = themes;
    return json;
}

Result<ApplicationModel> applicationFromJson(const Json& json)
{
    ApplicationModel model;
    Reader reader("application");
    if (!json.is_object())
        return makeError("ir.parse", "The application file must be an object");
    int version = jsonInt(json, "formaIr", 0);
    if (version < 1 || version > kFormatVersion)
        return makeError("ir.version", "This project uses format version " + std::to_string(version) + ", which this version of FormaIDE cannot read");
    readApplicationCommon(reader, json, model);
    if (!reader.ok())
        return reader.failure();
    return model;
}

Json applicationToFullJson(const ApplicationModel& model)
{
    Json json = applicationCommonJson(model);
    Json screens = Json::array();
    for (const auto& screen : model.screens)
        screens.push_back(screenToJson(screen));
    json["screens"] = screens;
    Json components = Json::array();
    for (const auto& definition : model.components)
        components.push_back(definitionToJson(definition));
    json["components"] = components;
    Json themes = Json::array();
    for (const auto& theme : model.themes)
        themes.push_back(themeToJson(theme));
    json["themes"] = themes;
    return json;
}

Result<ApplicationModel> applicationFromFullJson(const Json& json)
{
    auto base = applicationFromJson(json);
    if (!base)
        return base;
    ApplicationModel model = std::move(base.value());
    if (json.contains("screens")) {
        for (const auto& item : json["screens"]) {
            auto screen = screenFromJson(item);
            if (!screen)
                return screen.error();
            model.screens.push_back(std::move(screen.value()));
        }
    }
    if (json.contains("components")) {
        for (const auto& item : json["components"]) {
            auto definition = definitionFromJson(item);
            if (!definition)
                return definition.error();
            model.components.push_back(std::move(definition.value()));
        }
    }
    if (json.contains("themes")) {
        for (const auto& item : json["themes"]) {
            auto theme = themeFromJson(item);
            if (!theme)
                return theme.error();
            model.themes.push_back(std::move(theme.value()));
        }
    }
    return model;
}

Result<ApplicationModel> loadApplication(const IFilesystemService& filesystem, std::string_view designDirectory)
{
    std::string directory(designDirectory);
    auto root = readJsonFile(filesystem, directory + "/application.ir");
    if (!root)
        return root.error();
    auto base = applicationFromJson(*root);
    if (!base)
        return base;
    ApplicationModel model = std::move(base.value());
    for (const auto& id : jsonStrings(*root, "screens")) {
        std::string path = directory + "/screens/" + id + ".json";
        auto json = readJsonFile(filesystem, path);
        if (!json)
            return json.error();
        auto screen = screenFromJson(*json);
        if (!screen)
            return makeError("ir.parse", path + ": " + screen.error().message);
        model.screens.push_back(std::move(screen.value()));
    }
    for (const auto& id : jsonStrings(*root, "components")) {
        std::string path = directory + "/components/" + id + ".json";
        auto json = readJsonFile(filesystem, path);
        if (!json)
            return json.error();
        auto definition = definitionFromJson(*json);
        if (!definition)
            return makeError("ir.parse", path + ": " + definition.error().message);
        model.components.push_back(std::move(definition.value()));
    }
    for (const auto& id : jsonStrings(*root, "themes")) {
        std::string path = directory + "/themes/" + id + ".json";
        auto json = readJsonFile(filesystem, path);
        if (!json)
            return json.error();
        auto theme = themeFromJson(*json);
        if (!theme)
            return makeError("ir.parse", path + ": " + theme.error().message);
        model.themes.push_back(std::move(theme.value()));
    }
    return model;
}

Status saveApplication(IFilesystemService& filesystem, const ApplicationModel& model, std::string_view designDirectory, Origin origin)
{
    std::string directory(designDirectory);
    WriteOptions options;
    options.origin = origin;
    options.backup = false;

    auto saveList = [&](const std::string& folder, const std::vector<std::string>& keep) -> Status {
        std::string path = directory + "/" + folder;
        if (!filesystem.exists(path))
            return {};
        auto entries = filesystem.list(path);
        if (!entries)
            return entries.error();
        for (const auto& entry : *entries) {
            if (entry.directory || !text::endsWith(entry.path, ".json"))
                continue;
            std::string name = entry.path.substr(entry.path.find_last_of('/') + 1);
            name = name.substr(0, name.size() - 5);
            if (std::find(keep.begin(), keep.end(), name) == keep.end()) {
                auto removed = filesystem.remove(entry.path, origin);
                if (!removed)
                    return removed.error();
            }
        }
        return {};
    };

    std::vector<std::string> screenIds;
    for (const auto& screen : model.screens) {
        screenIds.push_back(screen.root.id);
        if (auto status = filesystem.writeFile(directory + "/screens/" + screen.root.id + ".json", dumpJson(screenToJson(screen)), options); !status)
            return status;
    }
    std::vector<std::string> definitionIds;
    for (const auto& definition : model.components) {
        definitionIds.push_back(definition.root.id);
        if (auto status = filesystem.writeFile(directory + "/components/" + definition.root.id + ".json", dumpJson(definitionToJson(definition)), options); !status)
            return status;
    }
    std::vector<std::string> themeIds;
    for (const auto& theme : model.themes) {
        themeIds.push_back(theme.id);
        if (auto status = filesystem.writeFile(directory + "/themes/" + theme.id + ".json", dumpJson(themeToJson(theme)), options); !status)
            return status;
    }
    if (auto status = filesystem.writeFile(directory + "/application.ir", dumpJson(applicationToJson(model)), options); !status)
        return status;
    if (auto status = saveList("screens", screenIds); !status)
        return status;
    if (auto status = saveList("components", definitionIds); !status)
        return status;
    if (auto status = saveList("themes", themeIds); !status)
        return status;
    return {};
}

Status setComponentField(Component& component, std::string_view key, const Json& value)
{
    if (key == "children")
        return makeError("ir.field", "Children cannot be set as a field");
    Json json = componentToJson(component, false);
    if (key == "properties.items" || !text::contains(key, ".")) {
        if (value.is_null())
            jsonErasePath(json, key);
        else
            jsonSetPath(json, key, value);
    } else if (value.is_null()) {
        jsonErasePath(json, key);
    } else {
        jsonSetPath(json, key, value);
    }
    Component parsed;
    Reader reader(component.id);
    readComponent(reader, json, parsed);
    if (!reader.ok())
        return reader.failure();
    parsed.children = std::move(component.children);
    component = std::move(parsed);
    return {};
}

Json getComponentField(const Component& component, std::string_view key)
{
    Json json = componentToJson(component, false);
    const Json* found = jsonFindPath(json, key);
    return found != nullptr ? *found : Json();
}

Status applyFieldOverrides(Component& component, const Json& overrides)
{
    if (!overrides.is_object())
        return makeError("ir.overrides", "Overrides must be an object");
    for (auto item = overrides.begin(); item != overrides.end(); ++item) {
        if (auto status = setComponentField(component, item.key(), *item); !status)
            return status;
    }
    return {};
}

}
