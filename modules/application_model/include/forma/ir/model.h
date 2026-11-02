#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"

namespace forma::ir {

constexpr int kFormatVersion = 1;

struct EdgeInsets {
    double top = 0;
    double right = 0;
    double bottom = 0;
    double left = 0;
    bool operator==(const EdgeInsets&) const = default;
    bool isZero() const { return top == 0 && right == 0 && bottom == 0 && left == 0; }
    bool isUniform() const { return top == right && right == bottom && bottom == left; }
};

enum class SizeMode { Fixed, Fill, Hug, Percent };

struct SizeSpec {
    SizeMode mode = SizeMode::Hug;
    double value = 0;
    std::optional<double> min;
    std::optional<double> max;
    bool operator==(const SizeSpec&) const = default;
};

enum class LayoutKind { Absolute, Vertical, Horizontal, Grid, Flex, Scroll, Constraint };
enum class Align { Start, Center, End, Stretch };
enum class Justify { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
enum class TrackKind { Fixed, Fraction, Auto };
enum class ScrollAxis { Vertical, Horizontal, Both };

struct GridTrack {
    TrackKind kind = TrackKind::Fraction;
    double value = 1;
    bool operator==(const GridTrack&) const = default;
};

struct LayoutSpec {
    LayoutKind kind = LayoutKind::Absolute;
    double spacing = 0;
    std::optional<double> rowSpacing;
    EdgeInsets padding;
    Align align = Align::Start;
    Justify justify = Justify::Start;
    bool wrap = false;
    std::vector<GridTrack> columns;
    std::vector<GridTrack> rows;
    ScrollAxis scroll = ScrollAxis::Vertical;
    bool operator==(const LayoutSpec&) const = default;
};

struct Pins {
    std::optional<double> left;
    std::optional<double> right;
    std::optional<double> top;
    std::optional<double> bottom;
    std::optional<double> centerX;
    std::optional<double> centerY;
    bool operator==(const Pins&) const = default;
    bool any() const { return left || right || top || bottom || centerX || centerY; }
};

struct GridPlacement {
    int column = 0;
    int row = 0;
    int columnSpan = 1;
    int rowSpan = 1;
    bool operator==(const GridPlacement&) const = default;
};

struct Shadow {
    double x = 0;
    double y = 0;
    double blur = 0;
    double spread = 0;
    std::string color;
    bool operator==(const Shadow&) const = default;
};

struct Style {
    std::optional<std::string> background;
    std::optional<std::string> borderColor;
    double borderWidth = 0;
    std::string borderStyle;
    double radius = 0;
    std::optional<Shadow> shadow;
    double opacity = 1;
    bool clip = false;
    bool operator==(const Style&) const = default;
};

struct TextStyle {
    std::string ref;
    std::string font;
    std::optional<double> size;
    std::optional<int> weight;
    std::optional<bool> italic;
    std::optional<bool> underline;
    std::optional<std::string> color;
    std::optional<std::string> align;
    std::optional<double> lineHeight;
    std::optional<double> letterSpacing;
    bool operator==(const TextStyle&) const = default;
};

struct Binding {
    std::string source;
    std::string mode = "oneWay";
    bool operator==(const Binding&) const = default;
};

struct Component {
    std::string id;
    std::string type;
    std::string name;
    bool visible = true;
    bool locked = false;
    double x = 0;
    double y = 0;
    SizeSpec width;
    SizeSpec height;
    EdgeInsets margin;
    double grow = 0;
    double shrink = 1;
    std::optional<Align> alignSelf;
    Pins pins;
    GridPlacement grid;
    LayoutSpec layout;
    Style style;
    TextStyle typography;
    Json properties = Json::object();
    std::map<std::string, std::string> events;
    std::map<std::string, Binding> bindings;
    std::map<std::string, std::string> tokens;
    std::map<std::string, Json> variants;
    std::map<std::string, Json> states;
    std::map<std::string, Json> platform;
    std::string instanceOf;
    Json overrides = Json::object();
    std::string codeManaged;
    Json extra = Json::object();
    std::vector<Component> children;

    bool operator==(const Component&) const = default;
};

struct Point {
    double x = 0;
    double y = 0;
    bool operator==(const Point&) const = default;
};

struct Screen {
    Component root;
    Point canvas;
    std::string device;
    std::string orientation = "portrait";
    Json extra = Json::object();
    bool operator==(const Screen&) const = default;
    const std::string& id() const { return root.id; }
};

struct ComponentParameter {
    std::string name;
    std::string target;
    std::string key;
    Json defaultValue;
    bool operator==(const ComponentParameter&) const = default;
};

struct ComponentDefinition {
    Component root;
    std::string description;
    std::vector<ComponentParameter> parameters;
    bool operator==(const ComponentDefinition&) const = default;
    const std::string& id() const { return root.id; }
};

struct DesignTokens {
    std::map<std::string, std::string> colors;
    std::map<std::string, TextStyle> typography;
    std::map<std::string, double> spacing;
    std::map<std::string, double> radius;
    std::map<std::string, double> borders;
    std::map<std::string, Shadow> shadows;
    std::map<std::string, std::string> icons;
    bool operator==(const DesignTokens&) const = default;
};

struct Theme {
    std::string id;
    std::string name;
    std::string mode = "light";
    DesignTokens tokens;
    bool operator==(const Theme&) const = default;
};

struct Route {
    std::string id;
    std::string from;
    std::string to;
    std::string transition;
    bool operator==(const Route&) const = default;
};

struct Navigation {
    std::string kind = "stack";
    std::string start;
    std::vector<Route> routes;
    bool operator==(const Navigation&) const = default;
};

struct StateVariable {
    std::string name;
    std::string type = "string";
    Json initial;
    std::string scope = "app";
    bool operator==(const StateVariable&) const = default;
};

struct DataField {
    std::string name;
    std::string type = "string";
    bool required = false;
    bool operator==(const DataField&) const = default;
};

struct DataModel {
    std::string name;
    std::vector<DataField> fields;
    bool operator==(const DataModel&) const = default;
};

struct Action {
    std::string id;
    std::string kind;
    std::string target;
    Json value;
    std::vector<std::string> steps;
    bool operator==(const Action&) const = default;
};

struct Asset {
    std::string id;
    std::string path;
    std::string kind;
    double width = 0;
    double height = 0;
    std::string hash;
    bool operator==(const Asset&) const = default;
};

struct Dependency {
    std::string name;
    std::string version;
    std::string ecosystem;
    bool operator==(const Dependency&) const = default;
};

struct BuildConfiguration {
    std::string id;
    std::string name;
    std::string kind = "debug";
    std::string architecture;
    std::string minimumOs;
    std::string compiler;
    std::string optimization;
    std::vector<std::string> flags;
    std::map<std::string, std::string> environment;
    std::string outputDirectory;
    bool operator==(const BuildConfiguration&) const = default;
};

struct Breakpoint {
    std::string id;
    std::string name;
    double minWidth = 0;
    std::optional<double> maxWidth;
    bool operator==(const Breakpoint&) const = default;
};

struct ModuleDefinition {
    std::string id;
    std::string name;
    std::string kind;
    std::string path;
    bool operator==(const ModuleDefinition&) const = default;
};

struct PlatformRequirement {
    std::string platform;
    std::string minimumVersion;
    std::vector<std::string> capabilities;
    bool operator==(const PlatformRequirement&) const = default;
};

struct AppMeta {
    std::string id;
    std::string name;
    std::string bundleId;
    std::string version = "0.1.0";
    std::string description;
    bool operator==(const AppMeta&) const = default;
};

struct ApplicationModel {
    AppMeta meta;
    std::vector<ModuleDefinition> modules;
    std::vector<Screen> screens;
    std::vector<ComponentDefinition> components;
    std::vector<Theme> themes;
    std::string activeTheme;
    std::vector<Breakpoint> breakpoints;
    Navigation navigation;
    std::vector<StateVariable> state;
    std::vector<DataModel> dataModels;
    std::vector<Action> actions;
    std::vector<Asset> assets;
    std::vector<Dependency> dependencies;
    std::vector<BuildConfiguration> buildConfigurations;
    std::vector<PlatformRequirement> platforms;
    Json extra = Json::object();

    bool operator==(const ApplicationModel&) const = default;

    const Screen* findScreen(std::string_view id) const;
    Screen* findScreen(std::string_view id);
    const ComponentDefinition* findDefinition(std::string_view id) const;
    const Action* findAction(std::string_view id) const;
    const StateVariable* findState(std::string_view name) const;
    const Asset* findAsset(std::string_view idOrPath) const;
    const Theme* findTheme(std::string_view id) const;
    const Theme* theme() const;
};

const Component* findComponent(const Component& root, std::string_view id);
Component* findComponent(Component& root, std::string_view id);
const Component* findParent(const Component& root, std::string_view id);
Component* findParent(Component& root, std::string_view id);
int indexInParent(const Component& parent, std::string_view id);
void visitComponents(const Component& root, const std::function<void(const Component&, const Component*)>& visitor);

std::string layoutKindName(LayoutKind kind);
std::string alignName(Align align);
std::string justifyName(Justify justify);
std::string sizeModeName(SizeMode mode);

std::string propertyString(const Component& component, std::string_view key, std::string_view fallback = {});
double propertyNumber(const Component& component, std::string_view key, double fallback = 0);
bool propertyBool(const Component& component, std::string_view key, bool fallback = false);
std::vector<std::string> propertyStrings(const Component& component, std::string_view key);

std::string resolveColor(const ApplicationModel& model, std::string_view value, std::string_view fallback = {});

Component makeComponent(std::string id, std::string type, std::string name = {});
Screen makeScreen(std::string id, std::string name, double width, double height);
ApplicationModel makeEmptyApplication(std::string name, std::string bundleId);
Theme makeDefaultTheme(std::string id, std::string name, std::string mode);

}
