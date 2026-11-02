#include <algorithm>
#include <cmath>
#include <set>

#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "forma/layout/layout_engine.h"
#include "generator_support.h"

namespace forma::generators {

using namespace forma::codegen;
using namespace forma::ir;

namespace {

const std::set<std::string>& cppReserved()
{
    static const std::set<std::string> words = {
        "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const", "continue", "default", "delete", "do",
        "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
        "namespace", "new", "not", "nullptr", "operator", "or", "private", "protected", "public", "register", "return", "short", "signed", "sizeof",
        "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "while", "xor", "layout", "parent", "state", "navigator", "window", "style", "update", "show", "hide", "close",
        "font", "size", "pos", "rect", "width", "height", "palette", "signals", "slots", "emit"};
    return words;
}

std::string qtString(std::string_view value)
{
    return "u\"" + escapeLiteral(value, LiteralStyle::CLike) + "\"_s";
}

std::string number(double value)
{
    return text::formatNumber(value);
}

std::string intNumber(double value)
{
    return std::to_string(static_cast<long long>(std::llround(value)));
}

std::string widgetClass(const Component& component)
{
    const std::string& type = component.type;
    if (type == "text" || type == "image" || type == "icon")
        return "QLabel";
    if (type == "button")
        return "QPushButton";
    if (type == "textinput")
        return "QLineEdit";
    if (type == "checkbox" || type == "switch")
        return "QCheckBox";
    if (type == "radio")
        return "QRadioButton";
    if (type == "dropdown")
        return "QComboBox";
    if (type == "slider")
        return "QSlider";
    if (type == "tabs")
        return "QTabWidget";
    if (type == "scroll" || component.layout.kind == LayoutKind::Scroll)
        return "QScrollArea";
    if (type == "list")
        return "QListWidget";
    if (type == "table" || type == "datatable")
        return "QTableWidget";
    if (type == "chart")
        return "SimpleChart";
    if (type == "media")
        return "QVideoWidget";
    if (type == "spacer")
        return "QWidget";
    if (type == "custom") {
        std::string name = propertyString(component, "className");
        if (!propertyString(component, "include").empty() && !name.empty())
            return name;
        return "QWidget";
    }
    return "QFrame";
}

std::string stateTypeName(const StateVariable& variable)
{
    if (variable.type == "int" || variable.type == "integer")
        return "int";
    if (variable.type == "double" || variable.type == "number")
        return "double";
    if (variable.type == "bool" || variable.type == "boolean")
        return "bool";
    if (variable.type == "list")
        return "QStringList";
    return "QString";
}

std::string stateInitialLiteral(const StateVariable& variable)
{
    std::string type = stateTypeName(variable);
    if (type == "int")
        return intNumber(stateDefaultNumber(variable));
    if (type == "double")
        return number(stateDefaultNumber(variable));
    if (type == "bool")
        return stateDefaultBool(variable) ? "true" : "false";
    if (type == "QStringList") {
        std::string output = "QStringList{";
        bool first = true;
        for (const auto& item : stateDefaultList(variable)) {
            if (!first)
                output += ", ";
            output += qtString(item);
            first = false;
        }
        return output + "}";
    }
    return qtString(stateDefaultString(variable));
}

std::string setterName(const std::string& name)
{
    return "set" + text::toPascalCase(name);
}

std::string getterName(const std::string& name)
{
    return text::toCamelCase(name);
}

std::string jsonLiteralForState(const StateVariable* variable, const Json& value)
{
    if (variable != nullptr) {
        std::string type = stateTypeName(*variable);
        if (type == "QString")
            return qtString(value.is_string() ? value.get<std::string>() : dumpJsonCompact(value));
        if (type == "int")
            return intNumber(value.is_number() ? value.get<double>() : 0);
        if (type == "double")
            return number(value.is_number() ? value.get<double>() : 0);
        if (type == "bool")
            return value.is_boolean() && value.get<bool>() ? "true" : "false";
    }
    if (value.is_string())
        return qtString(value.get<std::string>());
    if (value.is_number())
        return number(value.get<double>());
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    return "{}";
}

struct Member {
    std::string type;
    std::string name;
};

struct Handler {
    std::string name;
    std::string action;
    std::string signature;
};

class WidgetTreeEmitter {
public:
    WidgetTreeEmitter(const ApplicationModel& model, std::string owner, CodeWriter& writer, std::set<std::string>& includes, std::vector<Diagnostic>& diagnostics, bool screenOwned)
        : model_(model), owner_(std::move(owner)), w_(writer), includes_(includes), diagnostics_(diagnostics), screenOwned_(screenOwned),
          names_(NameTable::Style::Camel, cppReserved())
    {
    }

    std::vector<Member> members;
    std::vector<Handler> handlers;
    std::set<std::string> customHandlers;
    std::vector<std::string> bindingLines;
    bool usesRadioGroups = false;

    void emitRoot(const Component& root, const std::string& self)
    {
        w_.beginAnchor(owner_, root.id);
        w_.line("setObjectName(" + qtString(root.id) + ");");
        w_.line("setAttribute(Qt::WA_StyledBackground, true);");
        emitStyle(root, self);
        emitSizeConstraints(root, self, true);
        emitChildren(root, self);
        w_.endAnchor();
    }

    std::string memberName(const std::string& id) { return names_.name(id); }

private:
    BindingTarget target(const Component& component, std::string key, ValueKind kind = ValueKind::String) const
    {
        return BindingTarget{owner_, component.id, std::move(key), kind, LiteralStyle::CLike};
    }

    void boundText(const Component& component, const std::string& var, const std::string& setter, const std::string& propertyKey, const std::string& fallback = {})
    {
        std::string value = propertyString(component, propertyKey, fallback);
        std::string prefix = var + "->" + setter + "(u\"";
        w_.bound(prefix, escapeLiteral(value, LiteralStyle::CLike), "\"_s);", target(component, "properties." + propertyKey));
    }

    std::string colorExpression(const std::string& value)
    {
        ColorRef reference = colorRef(model_, value);
        if (reference.isToken)
            return "Theme::color(" + qtString(reference.token) + ")";
        return {};
    }

    void styleColor(const Component& component, const char* cssName, const std::string& value, const char* key)
    {
        std::string expression = colorExpression(value);
        if (!expression.empty()) {
            w_.line("{\"" + std::string(cssName) + "\", " + expression + "},");
            return;
        }
        std::string prefix = "{\"" + std::string(cssName) + "\", u\"";
        if (key != nullptr)
            w_.bound(prefix, value, "\"_s},", target(component, key, ValueKind::Color));
        else
            w_.line(prefix + value + "\"_s},");
    }

    void styleNumber(const Component& component, const char* cssName, double value, const char* key, const char* unit = "px")
    {
        std::string prefix = "{\"" + std::string(cssName) + "\", u\"";
        std::string suffix = std::string(unit) + "\"_s},";
        if (key != nullptr)
            w_.bound(prefix, number(value), suffix, target(component, key, ValueKind::Number));
        else
            w_.line(prefix + number(value) + suffix);
    }

    void emitStyle(const Component& c, const std::string& var)
    {
        const Style& style = c.style;
        const TextStyle& typography = c.typography;
        std::string type = c.type;
        bool textual = type == "text" || type == "button" || type == "textinput" || type == "checkbox" || type == "radio" || type == "switch"
            || type == "dropdown" || type == "list" || type == "table" || type == "datatable" || type == "navbar" || type == "tabs";
        std::vector<std::function<void()>> entries;

        std::string variant = propertyString(c, "variant", "primary");
        std::string defaultBackground;
        std::string defaultColor;
        std::string defaultBorderColor;
        double defaultBorderWidth = 0;
        double defaultRadius = 0;
        std::string extraPadding;
        if (type == "button") {
            defaultRadius = 6;
            extraPadding = "8px 16px";
            if (variant == "primary") {
                defaultBackground = "$color.primary";
                defaultColor = "$color.onPrimary";
            } else if (variant == "secondary") {
                defaultBackground = "$color.surface";
                defaultColor = "$color.text";
                defaultBorderColor = "$color.border";
                defaultBorderWidth = 1;
            } else {
                defaultBackground = "#00000000";
                defaultColor = "$color.primary";
            }
        } else if (type == "textinput" || type == "dropdown") {
            defaultRadius = 6;
            defaultBackground = "$color.background";
            defaultColor = "$color.text";
            defaultBorderColor = "$color.border";
            defaultBorderWidth = 1;
            extraPadding = "0 10px";
        } else if (type == "list" || type == "table" || type == "datatable") {
            defaultBackground = "$color.background";
            defaultColor = "$color.text";
            defaultBorderColor = "$color.border";
            defaultBorderWidth = 1;
        } else if (textual) {
            defaultColor = "$color.text";
        }

        std::string background = style.background.value_or(defaultBackground);
        std::string borderColor = style.borderColor.value_or(defaultBorderColor);
        double borderWidth = style.borderWidth > 0 ? style.borderWidth : defaultBorderWidth;
        double radius = style.radius > 0 ? style.radius : defaultRadius;

        bool any = !background.empty() || borderWidth > 0 || radius > 0 || !extraPadding.empty() || !defaultColor.empty()
            || typography.size || typography.weight || typography.color || typography.italic || typography.underline || !typography.font.empty() || !typography.ref.empty();
        if (type == "shape" && propertyString(c, "shape") == "ellipse")
            any = true;
        if (!any)
            return;

        w_.line("applyStyle(" + var + ", {");
        w_.indent();
        if (!background.empty()) {
            if (background == "#00000000")
                w_.line("{\"background\", u\"transparent\"_s},");
            else
                styleColor(c, "background", background, style.background && style.background->rfind('$', 0) != 0 ? "style.background" : nullptr);
        }
        if (borderWidth > 0 && !borderColor.empty()) {
            styleNumber(c, "border-width", borderWidth, style.borderWidth > 0 ? "style.borderWidth" : nullptr);
            w_.line("{\"border-style\", " + qtString(style.borderStyle.empty() ? "solid" : style.borderStyle) + "},");
            styleColor(c, "border-color", borderColor, style.borderColor && style.borderColor->rfind('$', 0) != 0 ? "style.borderColor" : nullptr);
        } else if (type == "button" || type == "textinput" || type == "dropdown") {
            w_.line("{\"border\", u\"none\"_s},");
        }
        if (type == "shape" && propertyString(c, "shape") == "ellipse") {
            double diameter = std::min(c.width.mode == SizeMode::Fixed ? c.width.value : 80, c.height.mode == SizeMode::Fixed ? c.height.value : 80);
            w_.line("{\"border-radius\", u\"" + number(diameter / 2) + "px\"_s},");
        } else if (radius > 0) {
            styleNumber(c, "border-radius", radius, style.radius > 0 ? "style.radius" : nullptr);
        }
        if (!extraPadding.empty())
            w_.line("{\"padding\", u\"" + extraPadding + "\"_s},");
        if (typography.color)
            styleColor(c, "color", *typography.color, typography.color->rfind('$', 0) != 0 ? "typography.color" : nullptr);
        else if (!defaultColor.empty())
            styleColor(c, "color", defaultColor, nullptr);
        if (!typography.font.empty())
            w_.line("{\"font-family\", " + qtString(typography.font) + "},");
        if (!typography.ref.empty() && !typography.size) {
            layout::ResolvedTextStyle resolved = layout::resolveTextStyle(c, &model_);
            w_.line("{\"font-size\", u\"" + number(resolved.size) + "px\"_s},");
            w_.line("{\"font-weight\", u\"" + std::to_string(resolved.weight) + "\"_s},");
        }
        if (typography.size)
            styleNumber(c, "font-size", *typography.size, "typography.size");
        if (typography.weight)
            styleNumber(c, "font-weight", *typography.weight, "typography.weight", "");
        if (typography.italic && *typography.italic)
            w_.line("{\"font-style\", u\"italic\"_s},");
        if (typography.underline && *typography.underline)
            w_.line("{\"text-decoration\", u\"underline\"_s},");
        w_.dedent();
        w_.line("});");
    }

    void emitEffects(const Component& c, const std::string& var)
    {
        if (c.style.shadow) {
            includes_.insert("QGraphicsDropShadowEffect");
            const Shadow& shadow = *c.style.shadow;
            std::string effect = var + "Shadow";
            w_.line("auto *" + effect + " = new QGraphicsDropShadowEffect(" + var + ");");
            w_.line(effect + "->setOffset(" + number(shadow.x) + ", " + number(shadow.y) + ");");
            w_.line(effect + "->setBlurRadius(" + number(shadow.blur) + ");");
            std::string color = colorExpression(shadow.color);
            if (color.empty())
                color = qtString(shadow.color.empty() ? "#00000040" : shadow.color);
            w_.line(effect + "->setColor(QColor(" + color + "));");
            w_.line(var + "->setGraphicsEffect(" + effect + ");");
        } else if (c.style.opacity < 1) {
            includes_.insert("QGraphicsOpacityEffect");
            std::string effect = var + "Opacity";
            w_.line("auto *" + effect + " = new QGraphicsOpacityEffect(" + var + ");");
            w_.bound(effect + "->setOpacity(", number(c.style.opacity), ");", target(c, "style.opacity", ValueKind::Number));
            w_.line(var + "->setGraphicsEffect(" + effect + ");");
        }
    }

    void emitSizeConstraints(const Component& c, const std::string& var, bool isRoot)
    {
        if (isRoot)
            return;
        if (c.width.mode == SizeMode::Fixed)
            w_.bound(var + "->setFixedWidth(", intNumber(c.width.value), ");", target(c, "width", ValueKind::Number));
        if (c.height.mode == SizeMode::Fixed)
            w_.bound(var + "->setFixedHeight(", intNumber(c.height.value), ");", target(c, "height", ValueKind::Number));
        if (c.width.min)
            w_.line(var + "->setMinimumWidth(" + intNumber(*c.width.min) + ");");
        if (c.width.max)
            w_.line(var + "->setMaximumWidth(" + intNumber(*c.width.max) + ");");
        if (c.height.min)
            w_.line(var + "->setMinimumHeight(" + intNumber(*c.height.min) + ");");
        if (c.height.max)
            w_.line(var + "->setMaximumHeight(" + intNumber(*c.height.max) + ");");
    }

    void emitSizePolicy(const Component& c, const Component* parent, const std::string& var)
    {
        if (parent == nullptr)
            return;
        LayoutKind kind = parent->layout.kind;
        if (kind == LayoutKind::Absolute || kind == LayoutKind::Constraint)
            return;
        bool parentHorizontal = kind == LayoutKind::Horizontal || kind == LayoutKind::Flex || (kind == LayoutKind::Scroll && parent->layout.scroll == ScrollAxis::Horizontal);
        Align align = c.alignSelf.value_or(parent->layout.align);
        auto policy = [&](const SizeSpec& spec, bool crossAxis) -> std::string {
            if (spec.mode == SizeMode::Fixed)
                return "QSizePolicy::Fixed";
            if (spec.mode == SizeMode::Fill || spec.mode == SizeMode::Percent)
                return "QSizePolicy::Expanding";
            if (crossAxis && align == Align::Stretch)
                return "QSizePolicy::Expanding";
            return "QSizePolicy::Fixed";
        };
        std::string horizontal = policy(c.width, !parentHorizontal);
        std::string vertical = policy(c.height, parentHorizontal);
        if (kind == LayoutKind::Grid) {
            if (c.width.mode == SizeMode::Hug && parent->layout.align == Align::Stretch)
                horizontal = "QSizePolicy::Expanding";
        }
        w_.line(var + "->setSizePolicy(" + horizontal + ", " + vertical + ");");
    }

    void connectEvent(const Component& c, const std::string& var, const std::string& event, const std::string& signal, const std::string& signature = "")
    {
        auto it = c.events.find(event);
        if (it == c.events.end() || it->second.empty())
            return;
        std::string handler = "on" + text::toPascalCase(c.id) + text::toPascalCase(event.substr(2));
        handlers.push_back({handler, it->second, signature});
        w_.line("connect(" + var + ", " + signal + ", this, &" + className + "::" + handler + ");");
    }

    void emitBindings(const Component& c, const std::string& var)
    {
        for (const auto& [key, binding] : c.bindings) {
            std::string source = binding.source.substr(0, binding.source.find('.'));
            const StateVariable* variable = model_.findState(source);
            if (variable == nullptr)
                continue;
            std::string getter = "state().";
            getter += getterName(source) + "()";
            std::string type = stateTypeName(*variable);
            std::string asText = getter;
            if (type == "int" || type == "double")
                asText = "QString::number(" + getter + ")";
            else if (type == "bool")
                asText = getter + " ? u\"true\"_s : u\"false\"_s";
            std::string prop = text::startsWith(key, "properties.") ? key.substr(11) : key;
            std::string statement;
            if (prop == "content" || prop == "label" || prop == "title") {
                std::string setter = c.type == "navbar" ? (var + "Title->setText") : (var + "->setText");
                statement = setter + "(" + asText + ");";
            } else if (prop == "value" && c.type == "textinput") {
                statement = "if (" + var + "->text() != " + asText + ") " + var + "->setText(" + asText + ");";
            } else if (prop == "value" && c.type == "slider") {
                statement = var + "->setValue(static_cast<int>(" + getter + "));";
            } else if (prop == "checked") {
                statement = var + "->setChecked(" + getter + ");";
            } else if (prop == "visible" || key == "visible") {
                statement = var + "->setVisible(" + getter + ");";
            } else if (prop == "items" && c.type == "list") {
                statement = var + "->clear(); " + var + "->addItems(" + getter + ");";
            } else {
                diagnostics_.push_back({Severity::Warning, "generator.binding", "The Qt generator cannot bind " + key + " on " + c.type, owner_, c.id, "bindings." + key});
                continue;
            }
            std::string notify = "&AppState::" + getterName(source) + "Changed";
            bindingLines.push_back("connect(&state(), " + notify + ", this, [this] { " + statement + " });");
            bindingLines.push_back(statement);
            if (binding.mode == "twoWay") {
                std::string setter = "state()." + setterName(source);
                if (c.type == "textinput")
                    bindingLines.push_back("connect(" + var + ", &QLineEdit::textChanged, this, [this](const QString &value) { " + setter + "(value); });");
                else if (c.type == "checkbox" || c.type == "switch")
                    bindingLines.push_back("connect(" + var + ", &QCheckBox::toggled, this, [this](bool value) { " + setter + "(value); });");
                else if (c.type == "slider")
                    bindingLines.push_back("connect(" + var + ", &QSlider::valueChanged, this, [this](int value) { " + setter + "(value); });");
            }
        }
    }

    void emitLeafProperties(const Component& c, const std::string& var)
    {
        const std::string& type = c.type;
        if (type == "text") {
            if (!boundState(c, "properties.content"))
                boundText(c, var, "setText", "content");
            std::string align = c.typography.align.value_or("");
            bool wrapping = c.width.mode != SizeMode::Hug || true;
            if (wrapping)
                w_.line(var + "->setWordWrap(true);");
            if (align == "center")
                w_.line(var + "->setAlignment(Qt::AlignHCenter | Qt::AlignTop);");
            else if (align == "right")
                w_.line(var + "->setAlignment(Qt::AlignRight | Qt::AlignTop);");
            else
                w_.line(var + "->setAlignment(Qt::AlignLeft | Qt::AlignTop);");
        } else if (type == "button") {
            if (!boundState(c, "properties.label"))
                boundText(c, var, "setText", "label");
            if (propertyBool(c, "disabled"))
                w_.line(var + "->setEnabled(false);");
            connectEvent(c, var, "onClick", "&QPushButton::clicked");
        } else if (type == "textinput") {
            boundText(c, var, "setPlaceholderText", "placeholder");
            if (!propertyString(c, "value").empty())
                boundText(c, var, "setText", "value");
            if (propertyBool(c, "password"))
                w_.line(var + "->setEchoMode(QLineEdit::Password);");
            if (propertyBool(c, "disabled"))
                w_.line(var + "->setEnabled(false);");
            w_.line(var + "->setMinimumHeight(36);");
            connectEvent(c, var, "onChange", "&QLineEdit::textChanged", "const QString &");
            connectEvent(c, var, "onSubmit", "&QLineEdit::returnPressed");
        } else if (type == "checkbox" || type == "switch" || type == "radio") {
            if (!boundState(c, "properties.label"))
                boundText(c, var, "setText", "label");
            if (propertyBool(c, "checked"))
                w_.line(var + "->setChecked(true);");
            if (propertyBool(c, "disabled"))
                w_.line(var + "->setEnabled(false);");
            std::string signal = type == "radio" ? "&QRadioButton::toggled" : "&QCheckBox::toggled";
            connectEvent(c, var, "onChange", signal, "bool");
        } else if (type == "dropdown") {
            std::string items;
            for (const auto& item : propertyStrings(c, "items")) {
                if (!items.empty())
                    items += ", ";
                items += qtString(item);
            }
            w_.line(var + "->addItems({" + items + "});");
            w_.line(var + "->setCurrentIndex(" + intNumber(propertyNumber(c, "selected", 0)) + ");");
            w_.line(var + "->setMinimumHeight(36);");
            if (propertyBool(c, "disabled"))
                w_.line(var + "->setEnabled(false);");
            connectEvent(c, var, "onChange", "qOverload<int>(&QComboBox::currentIndexChanged)", "int");
        } else if (type == "slider") {
            w_.line(var + "->setOrientation(Qt::Horizontal);");
            w_.line(var + "->setRange(" + intNumber(propertyNumber(c, "minimum", 0)) + ", " + intNumber(propertyNumber(c, "maximum", 100)) + ");");
            w_.bound(var + "->setValue(", intNumber(propertyNumber(c, "value", 50)), ");", target(c, "properties.value", ValueKind::Number));
            if (propertyBool(c, "disabled"))
                w_.line(var + "->setEnabled(false);");
            connectEvent(c, var, "onChange", "&QSlider::valueChanged", "int");
        } else if (type == "list") {
            if (!boundState(c, "properties.items")) {
                std::string items;
                for (const auto& item : propertyStrings(c, "items")) {
                    if (!items.empty())
                        items += ", ";
                    items += qtString(item);
                }
                w_.line(var + "->addItems({" + items + "});");
            }
            connectEvent(c, var, "onSelect", "&QListWidget::currentRowChanged", "int");
        } else if (type == "table" || type == "datatable") {
            auto columns = propertyStrings(c, "columns");
            w_.line(var + "->setColumnCount(" + std::to_string(columns.size()) + ");");
            std::string labels;
            for (const auto& column : columns) {
                if (!labels.empty())
                    labels += ", ";
                labels += qtString(column);
            }
            w_.line(var + "->setHorizontalHeaderLabels({" + labels + "});");
            w_.line(var + "->horizontalHeader()->setStretchLastSection(true);");
            if (propertyBool(c, "sortable"))
                w_.line(var + "->setSortingEnabled(true);");
            connectEvent(c, var, "onSelect", "&QTableWidget::currentCellChanged", "int, int, int, int");
        } else if (type == "image") {
            std::string source = propertyString(c, "src");
            if (const Asset* asset = model_.findAsset(source))
                source = asset->path;
            std::string fit = propertyString(c, "fit", "cover");
            if (!source.empty())
                w_.line(var + "->setPixmap(QPixmap(" + qtString(":/" + source) + "));");
            if (fit != "none")
                w_.line(var + "->setScaledContents(true);");
            std::string alt = propertyString(c, "alt");
            if (!alt.empty())
                w_.line(var + "->setAccessibleName(" + qtString(alt) + ");");
        } else if (type == "icon") {
            std::string name = propertyString(c, "name", "star");
            w_.line(var + "->setPixmap(QIcon::fromTheme(" + qtString(name) + ").pixmap(" + intNumber(c.width.mode == SizeMode::Fixed ? c.width.value : 24) + "));");
            w_.line(var + "->setAccessibleName(" + qtString(name) + ");");
        } else if (type == "navbar") {
            std::string layoutVar = var + "Layout";
            w_.line("auto *" + layoutVar + " = new QHBoxLayout(" + var + ");");
            w_.line(layoutVar + "->setContentsMargins(12, 8, 16, 8);");
            w_.line(layoutVar + "->setSpacing(8);");
            if (propertyBool(c, "back")) {
                std::string back = var + "Back";
                w_.line("auto *" + back + " = new QToolButton(" + var + ");");
                w_.line(back + "->setArrowType(Qt::LeftArrow);");
                w_.line(back + "->setAutoRaise(true);");
                w_.line(layoutVar + "->addWidget(" + back + ");");
                includes_.insert("QToolButton");
                if (c.events.count("onBack") != 0)
                    connectEvent(c, back, "onBack", "&QToolButton::clicked");
                else
                    w_.line("connect(" + back + ", &QToolButton::clicked, this, [this] { navigator().back(); });");
            }
            std::string title = var + "Title";
            members.push_back({"QLabel", title});
            includes_.insert("QLabel");
            w_.line(title + " = new QLabel(" + var + ");");
            if (!boundState(c, "properties.title"))
                boundText(c, title, "setText", "title");
            layout::ResolvedTextStyle resolved = layout::resolveTextStyle(c, &model_);
            w_.line("applyStyle(" + title + ", {{\"font-size\", u\"" + number(std::max(resolved.size, 17.0)) + "px\"_s}, {\"font-weight\", u\"" + std::to_string(std::max(resolved.weight, 600)) + "\"_s}, {\"color\", Theme::color(u\"text\"_s)}});");
            w_.line(title + "->setObjectName(" + qtString(c.id + "_title") + ");");
            w_.line(layoutVar + "->addWidget(" + title + ", 1);");
            w_.line(var + "->setMinimumHeight(52);");
            includes_.insert("QHBoxLayout");
        } else if (type == "divider") {
            bool vertical = propertyString(c, "orientation") == "vertical";
            if (vertical)
                w_.line(var + "->setFixedWidth(1);");
            else
                w_.line(var + "->setFixedHeight(1);");
        } else if (type == "chart") {
            auto labels = propertyStrings(c, "labels");
            std::string items;
            for (const auto& item : labels) {
                if (!items.empty())
                    items += ", ";
                items += qtString(item);
            }
            w_.line(var + "->setLabels({" + items + "});");
            w_.line(var + "->setKind(" + qtString(propertyString(c, "chart", "bar")) + ");");
        } else if (type == "map") {
            diagnostics_.push_back({Severity::Warning, "generator.placeholder", "Qt Widgets has no map control, so " + c.id + " is an empty frame. Place your map widget in it from your screen's code.", owner_, c.id, {}});
        } else if (type == "media") {
            std::string source = propertyString(c, "src");
            std::string player = var + "Player";
            w_.line("auto *" + player + " = new QMediaPlayer(" + var + ");");
            w_.line(player + "->setVideoOutput(" + var + ");");
            if (!source.empty())
                w_.line(player + "->setSource(QUrl(" + qtString(source) + "));");
            if (propertyBool(c, "autoplay"))
                w_.line(player + "->play();");
            includes_.insert("QMediaPlayer");
        } else if (type == "native") {
            diagnostics_.push_back({Severity::Warning, "generator.placeholder", "Native control " + propertyString(c, "control") + " has no Qt equivalent, so " + c.id + " is an empty widget.", owner_, c.id, {}});
        }
        if (c.events.count("onClick") != 0 && type != "button") {
            diagnostics_.push_back({Severity::Warning, "generator.event", "Qt widgets of this type do not emit clicks; handle onClick for " + c.id + " in your screen's code.", owner_, c.id, "events.onClick"});
        }
    }

    void emitChildren(const Component& parent, const std::string& parentVar)
    {
        if (parent.children.empty() && parent.type != "tabs")
            return;
        LayoutKind kind = parent.layout.kind;
        const LayoutSpec& layout = parent.layout;

        if (parent.type == "tabs") {
            auto items = propertyStrings(parent, "items");
            for (std::size_t i = 0; i < parent.children.size(); ++i) {
                const Component& child = parent.children[i];
                std::string childVar = emitWidget(child, parentVar, &parent);
                std::string label = i < items.size() ? items[i] : child.name;
                w_.line(parentVar + "->addTab(" + childVar + ", " + qtString(label) + ");");
            }
            for (std::size_t i = parent.children.size(); i < items.size(); ++i)
                w_.line(parentVar + "->addTab(new QWidget(" + parentVar + "), " + qtString(items[i]) + ");");
            w_.line(parentVar + "->setCurrentIndex(" + intNumber(propertyNumber(parent, "selected", 0)) + ");");
            connectEvent(parent, parentVar, "onChange", "&QTabWidget::currentChanged", "int");
            return;
        }

        std::string host = parentVar;
        if (kind == LayoutKind::Scroll) {
            std::string content = (parentVar == "this" ? std::string("root") : parentVar) + "Content";
            w_.line("auto *" + content + " = new QWidget(" + parentVar + ");");
            w_.line(parentVar + "->setWidget(" + content + ");");
            w_.line(parentVar + "->setWidgetResizable(true);");
            w_.line(parentVar + "->setFrameShape(QFrame::NoFrame);");
            if (layout.scroll == ScrollAxis::Vertical)
                w_.line(parentVar + "->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);");
            else if (layout.scroll == ScrollAxis::Horizontal)
                w_.line(parentVar + "->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);");
            host = content;
        }

        if (kind == LayoutKind::Absolute) {
            for (const auto& child : parent.children) {
                std::string childVar = emitWidget(child, host, &parent);
                std::string width = child.width.mode == SizeMode::Fixed ? intNumber(child.width.value) : childVar + "->sizeHint().width()";
                std::string height = child.height.mode == SizeMode::Fixed ? intNumber(child.height.value) : childVar + "->sizeHint().height()";
                w_.bound(childVar + "->move(", intNumber(child.x) + ", " + intNumber(child.y), ");", BindingTarget{owner_, child.id, "@frame", ValueKind::String, LiteralStyle::Raw});
                if (child.width.mode != SizeMode::Fixed || child.height.mode != SizeMode::Fixed)
                    w_.line(childVar + "->resize(" + width + ", " + height + ");");
            }
            return;
        }

        std::string layoutVar = (parentVar == "this" ? std::string("root") : parentVar) + "Layout";
        bool horizontal = kind == LayoutKind::Horizontal || (kind == LayoutKind::Flex && !layout.wrap) || (kind == LayoutKind::Scroll && layout.scroll == ScrollAxis::Horizontal);

        if (kind == LayoutKind::Constraint) {
            includes_.insert("application/PinLayout.h");
            w_.line("auto *" + layoutVar + " = new PinLayout(" + host + ");");
            emitMargins(parent, layoutVar);
            for (const auto& child : parent.children) {
                std::string childVar = emitWidget(child, host, &parent);
                const Pins& pins = child.pins;
                auto pin = [](const std::optional<double>& value) { return value ? intNumber(*value) : std::string("PinLayout::None"); };
                w_.line(layoutVar + "->addPinned(" + childVar + ", {" + pin(pins.left) + ", " + pin(pins.right) + ", " + pin(pins.top) + ", " + pin(pins.bottom) + ", " + pin(pins.centerX) + ", " + pin(pins.centerY) + ", " + intNumber(child.x) + ", " + intNumber(child.y) + "});");
            }
            return;
        }

        if (kind == LayoutKind::Grid) {
            includes_.insert("QGridLayout");
            w_.line("auto *" + layoutVar + " = new QGridLayout(" + host + ");");
            emitMargins(parent, layoutVar);
            w_.bound(layoutVar + "->setHorizontalSpacing(", intNumber(layout.spacing), ");", target(parent, "layout.spacing", ValueKind::Number));
            w_.line(layoutVar + "->setVerticalSpacing(" + intNumber(layout.rowSpacing.value_or(layout.spacing)) + ");");
            auto tracks = [&](const std::vector<GridTrack>& list, const char* stretch, const char* minimum) {
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (list[i].kind == TrackKind::Fraction)
                        w_.line(layoutVar + "->" + stretch + "(" + std::to_string(i) + ", " + intNumber(list[i].value) + ");");
                    else if (list[i].kind == TrackKind::Fixed)
                        w_.line(layoutVar + "->" + minimum + "(" + std::to_string(i) + ", " + intNumber(list[i].value) + ");");
                }
            };
            tracks(layout.columns, "setColumnStretch", "setColumnMinimumWidth");
            tracks(layout.rows, "setRowStretch", "setRowMinimumHeight");
            int cursorColumn = 0;
            int cursorRow = 0;
            int columnCount = std::max(1, static_cast<int>(layout.columns.size()));
            for (const auto& child : parent.children) {
                std::string childVar = emitWidget(child, host, &parent);
                int column = child.grid.column;
                int row = child.grid.row;
                if (child.grid == GridPlacement{}) {
                    column = cursorColumn;
                    row = cursorRow;
                    if (++cursorColumn >= columnCount) {
                        cursorColumn = 0;
                        ++cursorRow;
                    }
                }
                w_.line(layoutVar + "->addWidget(" + childVar + ", " + std::to_string(row) + ", " + std::to_string(column) + ", " + std::to_string(child.grid.rowSpan) + ", " + std::to_string(child.grid.columnSpan) + ");");
            }
            return;
        }

        if (kind == LayoutKind::Flex && layout.wrap) {
            includes_.insert("application/FlowLayout.h");
            w_.line("auto *" + layoutVar + " = new FlowLayout(" + host + ");");
            emitMargins(parent, layoutVar);
            w_.bound(layoutVar + "->setHorizontalGap(", intNumber(layout.spacing), ");", target(parent, "layout.spacing", ValueKind::Number));
            w_.line(layoutVar + "->setVerticalGap(" + intNumber(layout.rowSpacing.value_or(layout.spacing)) + ");");
            for (const auto& child : parent.children) {
                std::string childVar = emitWidget(child, host, &parent);
                w_.line(layoutVar + "->addWidget(" + childVar + ");");
            }
            return;
        }

        includes_.insert(horizontal ? "QHBoxLayout" : "QVBoxLayout");
        w_.line("auto *" + layoutVar + " = new " + (horizontal ? "QHBoxLayout" : "QVBoxLayout") + "(" + host + ");");
        emitMargins(parent, layoutVar);
        w_.bound(layoutVar + "->setSpacing(", intNumber(layout.spacing), ");", target(parent, "layout.spacing", ValueKind::Number));
        if (parent.type == "dialog" && !propertyString(parent, "title").empty()) {
            std::string title = names_.name(parent.id + "_title");
            members.push_back({"QLabel", title});
            includes_.insert("QLabel");
            w_.line(title + " = new QLabel(" + host + ");");
            w_.line(title + "->setObjectName(" + qtString(parent.id + "_title") + ");");
            boundText(parent, title, "setText", "title");
            layout::ResolvedTextStyle resolved = layout::resolveTextStyle(parent, &model_);
            w_.line("applyStyle(" + title + ", {{\"font-size\", u\"" + number(resolved.size) + "px\"_s}, {\"font-weight\", u\"600\"_s}, {\"color\", Theme::color(u\"text\"_s)}});");
            w_.line(layoutVar + "->addWidget(" + title + ");");
        }

        bool anyFill = std::any_of(parent.children.begin(), parent.children.end(), [&](const Component& child) {
            const SizeSpec& main = horizontal ? child.width : child.height;
            return child.visible && (main.mode == SizeMode::Fill || child.grow > 0);
        });
        auto addStretch = [&](int amount = 1) { w_.line(layoutVar + "->addStretch(" + std::to_string(amount) + ");"); };
        if (!anyFill) {
            if (layout.justify == Justify::Center || layout.justify == Justify::End || layout.justify == Justify::SpaceEvenly)
                addStretch();
            else if (layout.justify == Justify::SpaceAround)
                addStretch();
        }
        for (std::size_t i = 0; i < parent.children.size(); ++i) {
            const Component& child = parent.children[i];
            std::string childVar = emitWidget(child, host, &parent);
            addToBox(parent, child, childVar, layoutVar, horizontal, host);
            if (!anyFill && i + 1 < parent.children.size()) {
                if (layout.justify == Justify::SpaceBetween || layout.justify == Justify::SpaceEvenly)
                    addStretch();
                else if (layout.justify == Justify::SpaceAround)
                    addStretch(2);
            }
        }
        if (!anyFill) {
            if (layout.justify == Justify::Start || layout.justify == Justify::Center || layout.justify == Justify::SpaceEvenly || layout.justify == Justify::SpaceAround)
                addStretch();
        }
    }

    void emitMargins(const Component& parent, const std::string& layoutVar)
    {
        const EdgeInsets& padding = parent.layout.padding;
        if (padding.isUniform()) {
            std::string prefix = layoutVar + "->setContentsMargins(";
            std::string value = intNumber(padding.top);
            w_.line(prefix + value + ", " + value + ", " + value + ", " + value + ");");
        } else {
            w_.line(layoutVar + "->setContentsMargins(" + intNumber(padding.left) + ", " + intNumber(padding.top) + ", " + intNumber(padding.right) + ", " + intNumber(padding.bottom) + ");");
        }
    }

    void addToBox(const Component& parent, const Component& child, const std::string& childVar, const std::string& layoutVar, bool horizontal, const std::string& host)
    {
        const SizeSpec& main = horizontal ? child.width : child.height;
        const SizeSpec& cross = horizontal ? child.height : child.width;
        int stretch = main.mode == SizeMode::Fill ? std::max(1, static_cast<int>(child.grow)) : static_cast<int>(child.grow);
        if (main.mode == SizeMode::Percent)
            stretch = static_cast<int>(main.value);
        Align align = child.alignSelf.value_or(parent.layout.align);
        std::string alignment;
        bool stretches = cross.mode == SizeMode::Fill || cross.mode == SizeMode::Percent || (align == Align::Stretch && cross.mode == SizeMode::Hug);
        if (!stretches) {
            if (horizontal) {
                alignment = align == Align::Center ? "Qt::AlignVCenter" : align == Align::End ? "Qt::AlignBottom" : "Qt::AlignTop";
            } else {
                alignment = align == Align::Center ? "Qt::AlignHCenter" : align == Align::End ? "Qt::AlignRight" : "Qt::AlignLeft";
            }
        }
        std::string widget = childVar;
        if (!child.margin.isZero()) {
            std::string wrapper = childVar + "Margin";
            w_.line("auto *" + wrapper + " = withMargins(" + childVar + ", " + host + ", " + intNumber(child.margin.left) + ", " + intNumber(child.margin.top) + ", " + intNumber(child.margin.right) + ", " + intNumber(child.margin.bottom) + ");");
            widget = wrapper;
        }
        std::string line = layoutVar + "->addWidget(" + widget + ", " + std::to_string(stretch);
        if (!alignment.empty())
            line += ", " + alignment;
        w_.line(line + ");");
    }

    std::string emitWidget(const Component& c, const std::string& parentVar, const Component* parent)
    {
        std::string var = names_.name(c.id);
        std::string cls = widgetClass(c);
        if (!c.instanceOf.empty()) {
            const ComponentDefinition* definition = model_.findDefinition(c.instanceOf);
            if (definition != nullptr) {
                cls = definitionClassName(*definition);
                includes_.insert("components/" + cls + ".h");
            }
        }
        members.push_back({cls, var});
        includeForWidget(c, cls);
        w_.beginAnchor(owner_, c.id);
        if (!c.instanceOf.empty())
            w_.line(var + " = new " + cls + "(*navigatorPointer(), *statePointer(), " + parentVar + ");");
        else
            w_.line(var + " = new " + cls + "(" + parentVar + ");");
        w_.line(var + "->setObjectName(" + qtString(c.id) + ");");
        if (!c.instanceOf.empty()) {
            emitOverrides(c, var);
        } else {
            emitStyle(c, var);
            emitLeafProperties(c, var);
        }
        emitEffects(c, var);
        emitSizeConstraints(c, var, false);
        emitSizePolicy(c, parent, var);
        if (!c.visible)
            w_.bound(var + "->setVisible(", "false", ");", target(c, "visible", ValueKind::Boolean));
        emitBindings(c, var);
        if (c.instanceOf.empty() && isContainerType(c))
            emitChildren(c, var);
        w_.endAnchor();
        return var;
    }

    void emitOverrides(const Component& c, const std::string& var)
    {
        for (auto item = c.overrides.begin(); item != c.overrides.end(); ++item) {
            if (!item->is_object())
                continue;
            const ComponentDefinition* definition = model_.findDefinition(c.instanceOf);
            if (definition == nullptr)
                continue;
            const Component* target = item.key().empty() || item.key() == definition->root.id ? &definition->root : findComponent(definition->root, item.key());
            if (target == nullptr)
                continue;
            NameTable childNames(NameTable::Style::Camel, cppReserved());
            std::function<void(const Component&)> assign = [&](const Component& node) {
                childNames.name(node.id);
                for (const auto& child : node.children)
                    assign(child);
            };
            for (const auto& child : definition->root.children)
                assign(child);
            std::string childVar = var + "->" + childNames.name(target->id);
            for (auto field = item->begin(); field != item->end(); ++field) {
                if (!field->is_string())
                    continue;
                std::string key = field.key();
                std::string value = qtString(field->get<std::string>());
                if (key == "properties.content" || key == "properties.label")
                    w_.line(childVar + "->setText(" + value + ");");
                else if (key == "properties.placeholder")
                    w_.line(childVar + "->setPlaceholderText(" + value + ");");
                else
                    diagnostics_.push_back({Severity::Warning, "generator.override", "The override of " + key + " on " + c.id + " is shown on the canvas but not generated.", owner_, c.id, "overrides"});
            }
        }
    }

    void includeForWidget(const Component& c, const std::string& cls)
    {
        if (cls == "SimpleChart") {
            includes_.insert("application/SimpleChart.h");
            return;
        }
        if (c.type == "custom") {
            std::string include = propertyString(c, "include");
            if (!include.empty() && cls != "QWidget") {
                includes_.insert(include);
                return;
            }
        }
        if (!c.instanceOf.empty())
            return;
        includes_.insert(cls);
        if (cls == "QTableWidget")
            includes_.insert("QHeaderView");
        if (c.type == "image" || c.type == "icon") {
            includes_.insert("QIcon");
            includes_.insert("QPixmap");
        }
    }

public:
    std::string className;

private:
    const ApplicationModel& model_;
    std::string owner_;
    CodeWriter& w_;
    std::set<std::string>& includes_;
    std::vector<Diagnostic>& diagnostics_;
    bool screenOwned_;
    NameTable names_;
};

class QtGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "qt-cpp";
        value.label = "Qt Widgets (C++)";
        value.language = "C++";
        value.framework = "Qt 6 Widgets";
        value.description = "A desktop application in C++20 using Qt Widgets, built with CMake.";
        value.platforms = {"windows", "macos", "linux"};
        value.requiredTools = {"cmake", "cxx", "qt"};
        value.buildSystem = "cmake";
        value.approach = "cross-platform";
        value.preview = "run";
        return value;
    }

    GeneratorCapabilities capabilities() const override
    {
        GeneratorCapabilities value;
        value.components = allComponentTypes();
        value.components.erase(std::remove(value.components.begin(), value.components.end(), "map"), value.components.end());
        value.components.erase(std::remove(value.components.begin(), value.components.end(), "native"), value.components.end());
        value.layouts = allLayoutKinds();
        value.events = {"onClick", "onChange", "onSubmit", "onSelect", "onBack"};
        value.features = {"navigation", "state", "bindings", "themes", "assets", "reusable-components", "source-map"};
        return value;
    }

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override
    {
        return checkCapabilities(model, capabilities(), "Qt Widgets");
    }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string appClass = projectClass(config.name.empty() ? model.meta.name : config.name);
        auto screens = screenNames(model);

        result.files.push_back(cmakeLists(model, config));
        result.files.push_back(mainFile(appClass));
        result.files.push_back(styleHelpers());
        result.files.push_back(themeHeader());
        result.files.push_back(themeSource(model));
        auto stateFiles = appState(model);
        result.files.insert(result.files.end(), stateFiles.begin(), stateFiles.end());
        auto navigationFiles = navigatorFiles(model);
        result.files.insert(result.files.end(), navigationFiles.begin(), navigationFiles.end());
        result.files.push_back(windowHeader());
        result.files.push_back(windowSource(model, screens, config));
        if (usesLayout(model, LayoutKind::Constraint)) {
            auto helper = pinLayout();
            result.files.insert(result.files.end(), helper.begin(), helper.end());
        }
        if (usesWrap(model)) {
            auto helper = flowLayout();
            result.files.insert(result.files.end(), helper.begin(), helper.end());
        }
        if (usesType(model, "chart")) {
            auto helper = simpleChart();
            result.files.insert(result.files.end(), helper.begin(), helper.end());
        }
        for (const auto& definition : model.components)
            emitDefinition(model, definition, result);
        for (const auto& item : screens)
            emitScreen(model, item, result);
        if (!model.assets.empty())
            result.files.push_back(resourceFile(model));
        result.files.push_back(gitIgnore());
        result.files.push_back(readme(model, config));
        return result;
    }

private:
    static GeneratedFile plain(std::string path, std::string content, FileCategory category = FileCategory::Generated)
    {
        GeneratedFile file;
        file.path = std::move(path);
        file.content = std::move(content);
        file.category = category;
        return file;
    }

    GeneratedFile cmakeLists(const ApplicationModel& model, const ProjectConfiguration& config) const
    {
        std::string name = projectClass(config.name.empty() ? model.meta.name : config.name);
        bool media = usesType(model, "media");
        CodeWriter w;
        w.line("cmake_minimum_required(VERSION 3.21)");
        w.line("project(" + name + " VERSION " + (model.meta.version.empty() ? std::string("0.1.0") : model.meta.version) + " LANGUAGES CXX)");
        w.line();
        w.line("set(CMAKE_CXX_STANDARD 20)");
        w.line("set(CMAKE_CXX_STANDARD_REQUIRED ON)");
        w.line("set(CMAKE_EXPORT_COMPILE_COMMANDS ON)");
        w.line();
        std::string components = "Widgets";
        if (media)
            components += " Multimedia MultimediaWidgets";
        w.line("find_package(Qt6 REQUIRED COMPONENTS " + components + ")");
        w.line("qt_standard_project_setup()");
        w.line();
        w.line("file(GLOB_RECURSE app_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)");
        w.line("qt_add_executable(" + name + " WIN32 MACOSX_BUNDLE ${app_sources})");
        if (!model.assets.empty())
            w.line("qt_add_resources(" + name + " app_resources FILES resources/app.qrc)");
        w.line("target_include_directories(" + name + " PRIVATE src)");
        std::string libs = "Qt6::Widgets";
        if (media)
            libs += " Qt6::Multimedia Qt6::MultimediaWidgets";
        w.line("target_link_libraries(" + name + " PRIVATE " + libs + ")");
        w.line("target_compile_definitions(" + name + " PRIVATE QT_NO_KEYWORDS)");
        w.line("if(MSVC)");
        w.line("    target_compile_options(" + name + " PRIVATE /utf-8)");
        w.line("endif()");
        w.line();
        w.line("install(TARGETS " + name + " BUNDLE DESTINATION . RUNTIME DESTINATION bin)");
        return w.finish("CMakeLists.txt", FileCategory::Protected);
    }

    GeneratedFile mainFile(const std::string&) const
    {
        CodeWriter w;
        w.line("#include <QApplication>");
        w.line();
        w.line("#include \"application/AppWindow.h\"");
        w.line();
        w.line("int main(int argc, char *argv[])");
        w.line("{");
        w.indent();
        w.line("QApplication app(argc, argv);");
        w.line("AppWindow window;");
        w.line("window.show();");
        w.line("return app.exec();");
        w.dedent();
        w.line("}");
        return w.finish("src/main.cpp", FileCategory::User);
    }

    GeneratedFile styleHelpers() const
    {
        CodeWriter w;
        w.line("// Generated by FormaIDE. Regenerating merges your edits into this file.");
        w.line("#pragma once");
        w.line();
        w.line("#include <QHBoxLayout>");
        w.line("#include <QString>");
        w.line("#include <QWidget>");
        w.line();
        w.line("#include <initializer_list>");
        w.line("#include <utility>");
        w.line();
        w.line("using namespace Qt::StringLiterals;");
        w.line();
        w.line("inline void applyStyle(QWidget *widget, std::initializer_list<std::pair<const char *, QString>> entries)");
        w.line("{");
        w.indent();
        w.line("QString body;");
        w.line("for (const auto &[name, value] : entries)");
        w.indent();
        w.line("body += QString::fromLatin1(name) + u\": \"_s + value + u\"; \"_s;");
        w.dedent();
        w.line("widget->setStyleSheet(u\"#\"_s + widget->objectName() + u\" { \"_s + body + u\"}\"_s);");
        w.dedent();
        w.line("}");
        w.line();
        w.line("inline QWidget *withMargins(QWidget *child, QWidget *parent, int left, int top, int right, int bottom)");
        w.line("{");
        w.indent();
        w.line("auto *wrapper = new QWidget(parent);");
        w.line("auto *layout = new QHBoxLayout(wrapper);");
        w.line("layout->setContentsMargins(left, top, right, bottom);");
        w.line("layout->addWidget(child);");
        w.line("return wrapper;");
        w.dedent();
        w.line("}");
        return w.finish("src/application/StyleHelpers.h");
    }

    GeneratedFile themeHeader() const
    {
        CodeWriter w;
        w.line("// Generated by FormaIDE from the project's design tokens.");
        w.line("#pragma once");
        w.line();
        w.line("#include <QString>");
        w.line("#include <QStringList>");
        w.line();
        w.line("class Theme");
        w.line("{");
        w.line("public:");
        w.indent();
        w.line("static QString color(const QString &name);");
        w.line("static double spacing(const QString &name);");
        w.line("static double radius(const QString &name);");
        w.line("static QStringList available();");
        w.line("static QString active();");
        w.line("static void setActive(const QString &id);");
        w.dedent();
        w.line("};");
        return w.finish("src/theme/Theme.h");
    }

    GeneratedFile themeSource(const ApplicationModel& model) const
    {
        CodeWriter w;
        w.line("// Generated by FormaIDE from the project's design tokens.");
        w.line("#include \"theme/Theme.h\"");
        w.line();
        w.line("#include <QHash>");
        w.line();
        w.line("using namespace Qt::StringLiterals;");
        w.line();
        w.line("namespace {");
        w.line();
        w.line("struct ThemeTokens {");
        w.indent();
        w.line("QHash<QString, QString> colors;");
        w.line("QHash<QString, double> spacing;");
        w.line("QHash<QString, double> radius;");
        w.dedent();
        w.line("};");
        w.line();
        w.line("const QHash<QString, ThemeTokens> &themes()");
        w.line("{");
        w.indent();
        w.line("static const QHash<QString, ThemeTokens> table = {");
        w.indent();
        for (const auto& theme : model.themes) {
            w.line("{" + qtString(theme.id) + ", {");
            w.indent();
            w.line("{");
            w.indent();
            for (const auto& [name, value] : theme.tokens.colors)
                w.line("{" + qtString(name) + ", " + qtString(value) + "},");
            w.dedent();
            w.line("},");
            w.line("{");
            w.indent();
            for (const auto& [name, value] : theme.tokens.spacing)
                w.line("{" + qtString(name) + ", " + number(value) + "},");
            w.dedent();
            w.line("},");
            w.line("{");
            w.indent();
            for (const auto& [name, value] : theme.tokens.radius)
                w.line("{" + qtString(name) + ", " + number(value) + "},");
            w.dedent();
            w.line("},");
            w.dedent();
            w.line("}},");
        }
        w.dedent();
        w.line("};");
        w.line("return table;");
        w.dedent();
        w.line("}");
        w.line();
        w.line("QString &activeId()");
        w.line("{");
        w.indent();
        w.line("static QString id = " + qtString(model.theme() != nullptr ? model.theme()->id : std::string("light")) + ";");
        w.line("return id;");
        w.dedent();
        w.line("}");
        w.line();
        w.line("}");
        w.line();
        w.line("QString Theme::color(const QString &name)");
        w.line("{");
        w.indent();
        w.line("return themes().value(activeId()).colors.value(name, u\"#000000\"_s);");
        w.dedent();
        w.line("}");
        w.line();
        w.line("double Theme::spacing(const QString &name)");
        w.line("{");
        w.indent();
        w.line("return themes().value(activeId()).spacing.value(name);");
        w.dedent();
        w.line("}");
        w.line();
        w.line("double Theme::radius(const QString &name)");
        w.line("{");
        w.indent();
        w.line("return themes().value(activeId()).radius.value(name);");
        w.dedent();
        w.line("}");
        w.line();
        w.line("QStringList Theme::available()");
        w.line("{");
        w.indent();
        w.line("return themes().keys();");
        w.dedent();
        w.line("}");
        w.line();
        w.line("QString Theme::active()");
        w.line("{");
        w.indent();
        w.line("return activeId();");
        w.dedent();
        w.line("}");
        w.line();
        w.line("void Theme::setActive(const QString &id)");
        w.line("{");
        w.indent();
        w.line("if (themes().contains(id))");
        w.indent();
        w.line("activeId() = id;");
        w.dedent();
        w.dedent();
        w.line("}");
        return w.finish("src/theme/Theme.cpp");
    }

    std::vector<GeneratedFile> appState(const ApplicationModel& model) const
    {
        CodeWriter h;
        h.line("// Generated by FormaIDE from the project's state model.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QObject>");
        h.line("#include <QString>");
        h.line("#include <QStringList>");
        h.line();
        h.line("class AppState : public QObject");
        h.line("{");
        h.indent();
        h.line("Q_OBJECT");
        h.dedent();
        h.line();
        h.line("public:");
        h.indent();
        h.line("explicit AppState(QObject *parent = nullptr);");
        h.line();
        for (const auto& variable : model.state) {
            std::string type = stateTypeName(variable);
            std::string argument = type == "QString" || type == "QStringList" ? "const " + type + " &" : type + " ";
            h.line(type + " " + getterName(variable.name) + "() const { return " + getterName(variable.name) + "_; }");
            h.line("void " + setterName(variable.name) + "(" + argument + "value);");
        }
        h.dedent();
        h.line();
        h.line("Q_SIGNALS:");
        h.indent();
        for (const auto& variable : model.state)
            h.line("void " + getterName(variable.name) + "Changed();");
        h.dedent();
        h.line();
        h.line("private:");
        h.indent();
        for (const auto& variable : model.state)
            h.line(stateTypeName(variable) + " " + getterName(variable.name) + "_ = " + stateInitialLiteralForHeader(variable) + ";");
        h.dedent();
        h.line("};");

        CodeWriter c;
        c.line("// Generated by FormaIDE from the project's state model.");
        c.line("#include \"application/AppState.h\"");
        c.line();
        c.line("AppState::AppState(QObject *parent) : QObject(parent) {}");
        for (const auto& variable : model.state) {
            std::string type = stateTypeName(variable);
            std::string argument = type == "QString" || type == "QStringList" ? "const " + type + " &" : type + " ";
            c.line();
            c.line("void AppState::" + setterName(variable.name) + "(" + argument + "value)");
            c.line("{");
            c.indent();
            c.line("if (" + getterName(variable.name) + "_ == value)");
            c.indent();
            c.line("return;");
            c.dedent();
            c.line(getterName(variable.name) + "_ = value;");
            c.line("Q_EMIT " + getterName(variable.name) + "Changed();");
            c.dedent();
            c.line("}");
        }
        return {h.finish("src/application/AppState.h"), c.finish("src/application/AppState.cpp")};
    }

    static std::string stateInitialLiteralForHeader(const StateVariable& variable)
    {
        std::string type = stateTypeName(variable);
        if (type == "QString")
            return "QStringLiteral(\"" + escapeLiteral(stateDefaultString(variable), LiteralStyle::CLike) + "\")";
        if (type == "QStringList") {
            std::string output = "QStringList{";
            bool first = true;
            for (const auto& item : stateDefaultList(variable)) {
                if (!first)
                    output += ", ";
                output += "QStringLiteral(\"" + escapeLiteral(item, LiteralStyle::CLike) + "\")";
                first = false;
            }
            return output + "}";
        }
        return stateInitialLiteral(variable);
    }

    std::vector<GeneratedFile> navigatorFiles(const ApplicationModel& model) const
    {
        CodeWriter h;
        h.line("// Generated by FormaIDE from the project's navigation model.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QHash>");
        h.line("#include <QStackedWidget>");
        h.line("#include <QStringList>");
        h.line();
        h.line("#include <functional>");
        h.line();
        h.line("class AppNavigator : public QStackedWidget");
        h.line("{");
        h.indent();
        h.line("Q_OBJECT");
        h.dedent();
        h.line();
        h.line("public:");
        h.indent();
        h.line("using Factory = std::function<QWidget *()>;");
        h.line();
        h.line("explicit AppNavigator(QWidget *parent = nullptr);");
        h.line("void registerScreen(const QString &id, Factory factory);");
        h.line("void navigate(const QString &id);");
        h.line("void back();");
        h.line("QString current() const;");
        h.dedent();
        h.line();
        h.line("Q_SIGNALS:");
        h.indent();
        h.line("void screenChanged(const QString &id);");
        h.dedent();
        h.line();
        h.line("private:");
        h.indent();
        h.line("void show(const QString &id);");
        h.line();
        h.line("QHash<QString, Factory> factories_;");
        h.line("QHash<QString, QWidget *> screens_;");
        h.line("QStringList history_;");
        h.dedent();
        h.line("};");

        CodeWriter c;
        c.line("// Generated by FormaIDE from the project's navigation model.");
        c.line("#include \"application/AppNavigator.h\"");
        c.line();
        c.line("AppNavigator::AppNavigator(QWidget *parent) : QStackedWidget(parent) {}");
        c.line();
        c.line("void AppNavigator::registerScreen(const QString &id, Factory factory)");
        c.line("{");
        c.indent();
        c.line("factories_.insert(id, std::move(factory));");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void AppNavigator::navigate(const QString &id)");
        c.line("{");
        c.indent();
        c.line("if (!factories_.contains(id))");
        c.indent();
        c.line("return;");
        c.dedent();
        c.line("history_.append(id);");
        c.line("show(id);");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void AppNavigator::back()");
        c.line("{");
        c.indent();
        c.line("if (history_.size() < 2)");
        c.indent();
        c.line("return;");
        c.dedent();
        c.line("history_.removeLast();");
        c.line("show(history_.last());");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QString AppNavigator::current() const");
        c.line("{");
        c.indent();
        c.line("return history_.isEmpty() ? QString() : history_.last();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void AppNavigator::show(const QString &id)");
        c.line("{");
        c.indent();
        c.line("QWidget *screen = screens_.value(id);");
        c.line("if (screen == nullptr) {");
        c.indent();
        c.line("screen = factories_.value(id)();");
        c.line("screens_.insert(id, screen);");
        c.line("addWidget(screen);");
        c.dedent();
        c.line("}");
        c.line("setCurrentWidget(screen);");
        c.line("Q_EMIT screenChanged(id);");
        c.dedent();
        c.line("}");
        (void)model;
        return {h.finish("src/application/AppNavigator.h"), c.finish("src/application/AppNavigator.cpp")};
    }

    GeneratedFile windowHeader() const
    {
        CodeWriter w;
        w.line("// Generated by FormaIDE.");
        w.line("#pragma once");
        w.line();
        w.line("#include <QMainWindow>");
        w.line();
        w.line("class AppNavigator;");
        w.line("class AppState;");
        w.line();
        w.line("class AppWindow : public QMainWindow");
        w.line("{");
        w.indent();
        w.line("Q_OBJECT");
        w.dedent();
        w.line();
        w.line("public:");
        w.indent();
        w.line("explicit AppWindow(QWidget *parent = nullptr);");
        w.line();
        w.line("AppNavigator *navigator() const { return navigator_; }");
        w.line("AppState *state() const { return state_; }");
        w.dedent();
        w.line();
        w.line("private:");
        w.indent();
        w.line("AppNavigator *navigator_ = nullptr;");
        w.line("AppState *state_ = nullptr;");
        w.dedent();
        w.line("};");
        return w.finish("src/application/AppWindow.h");
    }

    GeneratedFile windowSource(const ApplicationModel& model, const std::vector<ScreenNames>& screens, const ProjectConfiguration& config) const
    {
        CodeWriter w;
        w.line("// Generated by FormaIDE.");
        w.line("#include \"application/AppWindow.h\"");
        w.line();
        w.line("#include \"application/AppNavigator.h\"");
        w.line("#include \"application/AppState.h\"");
        w.line("#include \"application/StyleHelpers.h\"");
        for (const auto& screen : screens)
            w.line("#include \"screens/" + screen.className + ".h\"");
        w.line();
        w.line("AppWindow::AppWindow(QWidget *parent) : QMainWindow(parent)");
        w.line("{");
        w.indent();
        w.line("setWindowTitle(" + qtString(config.name.empty() ? model.meta.name : config.name) + ");");
        std::string start = startScreenId(model);
        if (const Screen* startScreen = model.findScreen(start)) {
            int width = static_cast<int>(startScreen->root.width.mode == SizeMode::Fixed ? startScreen->root.width.value : 800);
            int height = static_cast<int>(startScreen->root.height.mode == SizeMode::Fixed ? startScreen->root.height.value : 600);
            w.line("resize(" + std::to_string(width) + ", " + std::to_string(height) + ");");
        }
        w.line("state_ = new AppState(this);");
        w.line("navigator_ = new AppNavigator(this);");
        w.line("setCentralWidget(navigator_);");
        for (const auto& screen : screens) {
            w.line("navigator_->registerScreen(" + qtString(screen.screen->root.id) + ", [this] { return new " + screen.className + "(*navigator_, *state_, navigator_); });");
        }
        if (!start.empty())
            w.line("navigator_->navigate(" + qtString(start) + ");");
        w.dedent();
        w.line("}");
        return w.finish("src/application/AppWindow.cpp");
    }

    std::vector<GeneratedFile> pinLayout() const
    {
        CodeWriter h;
        h.line("// Generated by FormaIDE. Positions children by distances from the edges or the center.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QLayout>");
        h.line("#include <QList>");
        h.line();
        h.line("#include <limits>");
        h.line();
        h.line("class PinLayout : public QLayout");
        h.line("{");
        h.line("public:");
        h.indent();
        h.line("static constexpr int None = std::numeric_limits<int>::min();");
        h.line();
        h.line("struct Pins {");
        h.indent();
        h.line("int left = None;");
        h.line("int right = None;");
        h.line("int top = None;");
        h.line("int bottom = None;");
        h.line("int centerX = None;");
        h.line("int centerY = None;");
        h.line("int x = 0;");
        h.line("int y = 0;");
        h.dedent();
        h.line("};");
        h.line();
        h.line("explicit PinLayout(QWidget *parent = nullptr);");
        h.line("~PinLayout() override;");
        h.line("void addPinned(QWidget *widget, const Pins &pins);");
        h.line();
        h.line("void addItem(QLayoutItem *item) override;");
        h.line("QSize sizeHint() const override;");
        h.line("QSize minimumSize() const override;");
        h.line("void setGeometry(const QRect &rect) override;");
        h.line("QLayoutItem *itemAt(int index) const override;");
        h.line("QLayoutItem *takeAt(int index) override;");
        h.line("int count() const override;");
        h.dedent();
        h.line();
        h.line("private:");
        h.indent();
        h.line("struct Entry {");
        h.indent();
        h.line("QLayoutItem *item = nullptr;");
        h.line("Pins pins;");
        h.dedent();
        h.line("};");
        h.line("QList<Entry> entries_;");
        h.dedent();
        h.line("};");

        CodeWriter c;
        c.line("// Generated by FormaIDE.");
        c.line("#include \"application/PinLayout.h\"");
        c.line();
        c.line("#include <QWidget>");
        c.line();
        c.line("#include <algorithm>");
        c.line();
        c.line("PinLayout::PinLayout(QWidget *parent) : QLayout(parent) {}");
        c.line();
        c.line("PinLayout::~PinLayout()");
        c.line("{");
        c.indent();
        c.line("while (QLayoutItem *item = takeAt(0))");
        c.indent();
        c.line("delete item;");
        c.dedent();
        c.dedent();
        c.line("}");
        c.line();
        c.line("void PinLayout::addPinned(QWidget *widget, const Pins &pins)");
        c.line("{");
        c.indent();
        c.line("addChildWidget(widget);");
        c.line("entries_.append({new QWidgetItem(widget), pins});");
        c.line("invalidate();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void PinLayout::addItem(QLayoutItem *item)");
        c.line("{");
        c.indent();
        c.line("entries_.append({item, {}});");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QSize PinLayout::sizeHint() const");
        c.line("{");
        c.indent();
        c.line("return minimumSize();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QSize PinLayout::minimumSize() const");
        c.line("{");
        c.indent();
        c.line("QSize size(0, 0);");
        c.line("for (const Entry &entry : entries_) {");
        c.indent();
        c.line("QSize hint = entry.item->sizeHint();");
        c.line("int left = entry.pins.left != None ? entry.pins.left : entry.pins.x;");
        c.line("int top = entry.pins.top != None ? entry.pins.top : entry.pins.y;");
        c.line("size = size.expandedTo(QSize(left + hint.width(), top + hint.height()));");
        c.dedent();
        c.line("}");
        c.line("return size;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void PinLayout::setGeometry(const QRect &rect)");
        c.line("{");
        c.indent();
        c.line("QLayout::setGeometry(rect);");
        c.line("QRect area = contentsRect();");
        c.line("for (const Entry &entry : entries_) {");
        c.indent();
        c.line("const Pins &pins = entry.pins;");
        c.line("QSize hint = entry.item->sizeHint();");
        c.line("int width = hint.width();");
        c.line("int height = hint.height();");
        c.line("int x = pins.x;");
        c.line("int y = pins.y;");
        c.line("if (pins.left != None && pins.right != None) {");
        c.indent();
        c.line("x = pins.left;");
        c.line("width = std::max(0, area.width() - pins.left - pins.right);");
        c.dedent();
        c.line("} else if (pins.left != None) {");
        c.indent();
        c.line("x = pins.left;");
        c.dedent();
        c.line("} else if (pins.right != None) {");
        c.indent();
        c.line("x = area.width() - width - pins.right;");
        c.dedent();
        c.line("} else if (pins.centerX != None) {");
        c.indent();
        c.line("x = (area.width() - width) / 2 + pins.centerX;");
        c.dedent();
        c.line("}");
        c.line("if (pins.top != None && pins.bottom != None) {");
        c.indent();
        c.line("y = pins.top;");
        c.line("height = std::max(0, area.height() - pins.top - pins.bottom);");
        c.dedent();
        c.line("} else if (pins.top != None) {");
        c.indent();
        c.line("y = pins.top;");
        c.dedent();
        c.line("} else if (pins.bottom != None) {");
        c.indent();
        c.line("y = area.height() - height - pins.bottom;");
        c.dedent();
        c.line("} else if (pins.centerY != None) {");
        c.indent();
        c.line("y = (area.height() - height) / 2 + pins.centerY;");
        c.dedent();
        c.line("}");
        c.line("entry.item->setGeometry(QRect(area.x() + x, area.y() + y, width, height));");
        c.dedent();
        c.line("}");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QLayoutItem *PinLayout::itemAt(int index) const");
        c.line("{");
        c.indent();
        c.line("return index >= 0 && index < entries_.size() ? entries_.at(index).item : nullptr;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QLayoutItem *PinLayout::takeAt(int index)");
        c.line("{");
        c.indent();
        c.line("if (index < 0 || index >= entries_.size())");
        c.indent();
        c.line("return nullptr;");
        c.dedent();
        c.line("return entries_.takeAt(index).item;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("int PinLayout::count() const");
        c.line("{");
        c.indent();
        c.line("return static_cast<int>(entries_.size());");
        c.dedent();
        c.line("}");
        return {h.finish("src/application/PinLayout.h"), c.finish("src/application/PinLayout.cpp")};
    }

    std::vector<GeneratedFile> flowLayout() const
    {
        CodeWriter h;
        h.line("// Generated by FormaIDE. Lays children out in rows that wrap.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QLayout>");
        h.line("#include <QList>");
        h.line();
        h.line("class FlowLayout : public QLayout");
        h.line("{");
        h.line("public:");
        h.indent();
        h.line("explicit FlowLayout(QWidget *parent = nullptr);");
        h.line("~FlowLayout() override;");
        h.line("void setHorizontalGap(int gap) { horizontalGap_ = gap; }");
        h.line("void setVerticalGap(int gap) { verticalGap_ = gap; }");
        h.line();
        h.line("void addItem(QLayoutItem *item) override;");
        h.line("QSize sizeHint() const override;");
        h.line("QSize minimumSize() const override;");
        h.line("void setGeometry(const QRect &rect) override;");
        h.line("bool hasHeightForWidth() const override { return true; }");
        h.line("int heightForWidth(int width) const override;");
        h.line("QLayoutItem *itemAt(int index) const override;");
        h.line("QLayoutItem *takeAt(int index) override;");
        h.line("int count() const override;");
        h.dedent();
        h.line();
        h.line("private:");
        h.indent();
        h.line("int arrange(const QRect &rect, bool apply) const;");
        h.line();
        h.line("QList<QLayoutItem *> items_;");
        h.line("int horizontalGap_ = 0;");
        h.line("int verticalGap_ = 0;");
        h.dedent();
        h.line("};");

        CodeWriter c;
        c.line("// Generated by FormaIDE.");
        c.line("#include \"application/FlowLayout.h\"");
        c.line();
        c.line("#include <QWidget>");
        c.line();
        c.line("#include <algorithm>");
        c.line();
        c.line("FlowLayout::FlowLayout(QWidget *parent) : QLayout(parent) {}");
        c.line();
        c.line("FlowLayout::~FlowLayout()");
        c.line("{");
        c.indent();
        c.line("while (QLayoutItem *item = takeAt(0))");
        c.indent();
        c.line("delete item;");
        c.dedent();
        c.dedent();
        c.line("}");
        c.line();
        c.line("void FlowLayout::addItem(QLayoutItem *item)");
        c.line("{");
        c.indent();
        c.line("items_.append(item);");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QSize FlowLayout::sizeHint() const");
        c.line("{");
        c.indent();
        c.line("return minimumSize();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QSize FlowLayout::minimumSize() const");
        c.line("{");
        c.indent();
        c.line("QSize size(0, 0);");
        c.line("for (QLayoutItem *item : items_)");
        c.indent();
        c.line("size = size.expandedTo(item->minimumSize());");
        c.dedent();
        c.line("int left = 0, top = 0, right = 0, bottom = 0;");
        c.line("getContentsMargins(&left, &top, &right, &bottom);");
        c.line("return size + QSize(left + right, top + bottom);");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void FlowLayout::setGeometry(const QRect &rect)");
        c.line("{");
        c.indent();
        c.line("QLayout::setGeometry(rect);");
        c.line("arrange(rect, true);");
        c.dedent();
        c.line("}");
        c.line();
        c.line("int FlowLayout::heightForWidth(int width) const");
        c.line("{");
        c.indent();
        c.line("return arrange(QRect(0, 0, width, 0), false);");
        c.dedent();
        c.line("}");
        c.line();
        c.line("int FlowLayout::arrange(const QRect &rect, bool apply) const");
        c.line("{");
        c.indent();
        c.line("int left = 0, top = 0, right = 0, bottom = 0;");
        c.line("getContentsMargins(&left, &top, &right, &bottom);");
        c.line("QRect area = rect.adjusted(left, top, -right, -bottom);");
        c.line("int x = area.x();");
        c.line("int y = area.y();");
        c.line("int rowHeight = 0;");
        c.line("for (QLayoutItem *item : items_) {");
        c.indent();
        c.line("QSize hint = item->sizeHint();");
        c.line("if (x > area.x() && x + hint.width() > area.right() + 1) {");
        c.indent();
        c.line("x = area.x();");
        c.line("y += rowHeight + verticalGap_;");
        c.line("rowHeight = 0;");
        c.dedent();
        c.line("}");
        c.line("if (apply)");
        c.indent();
        c.line("item->setGeometry(QRect(QPoint(x, y), hint));");
        c.dedent();
        c.line("x += hint.width() + horizontalGap_;");
        c.line("rowHeight = std::max(rowHeight, hint.height());");
        c.dedent();
        c.line("}");
        c.line("return y + rowHeight - rect.y() + bottom;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QLayoutItem *FlowLayout::itemAt(int index) const");
        c.line("{");
        c.indent();
        c.line("return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("QLayoutItem *FlowLayout::takeAt(int index)");
        c.line("{");
        c.indent();
        c.line("return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;");
        c.dedent();
        c.line("}");
        c.line();
        c.line("int FlowLayout::count() const");
        c.line("{");
        c.indent();
        c.line("return static_cast<int>(items_.size());");
        c.dedent();
        c.line("}");
        return {h.finish("src/application/FlowLayout.h"), c.finish("src/application/FlowLayout.cpp")};
    }

    std::vector<GeneratedFile> simpleChart() const
    {
        CodeWriter h;
        h.line("// Generated by FormaIDE. A small chart with no extra dependencies. Replace it with Qt Charts if you need more.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QStringList>");
        h.line("#include <QWidget>");
        h.line();
        h.line("class SimpleChart : public QWidget");
        h.line("{");
        h.line("public:");
        h.indent();
        h.line("explicit SimpleChart(QWidget *parent = nullptr);");
        h.line("void setLabels(const QStringList &labels);");
        h.line("void setKind(const QString &kind);");
        h.line("void setValues(const QList<double> &values);");
        h.dedent();
        h.line();
        h.line("protected:");
        h.indent();
        h.line("void paintEvent(QPaintEvent *event) override;");
        h.dedent();
        h.line();
        h.line("private:");
        h.indent();
        h.line("QStringList labels_;");
        h.line("QString kind_ = QStringLiteral(\"bar\");");
        h.line("QList<double> values_;");
        h.dedent();
        h.line("};");

        CodeWriter c;
        c.line("// Generated by FormaIDE.");
        c.line("#include \"application/SimpleChart.h\"");
        c.line();
        c.line("#include <QPainter>");
        c.line("#include <QPainterPath>");
        c.line();
        c.line("#include <algorithm>");
        c.line();
        c.line("#include \"theme/Theme.h\"");
        c.line();
        c.line("using namespace Qt::StringLiterals;");
        c.line();
        c.line("SimpleChart::SimpleChart(QWidget *parent) : QWidget(parent) {}");
        c.line();
        c.line("void SimpleChart::setLabels(const QStringList &labels)");
        c.line("{");
        c.indent();
        c.line("labels_ = labels;");
        c.line("if (values_.size() < labels_.size()) {");
        c.indent();
        c.line("values_.clear();");
        c.line("for (qsizetype i = 0; i < labels_.size(); ++i)");
        c.indent();
        c.line("values_.append(static_cast<double>(((i * 37 + 40) % 70) + 25));");
        c.dedent();
        c.dedent();
        c.line("}");
        c.line("update();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void SimpleChart::setKind(const QString &kind)");
        c.line("{");
        c.indent();
        c.line("kind_ = kind;");
        c.line("update();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void SimpleChart::setValues(const QList<double> &values)");
        c.line("{");
        c.indent();
        c.line("values_ = values;");
        c.line("update();");
        c.dedent();
        c.line("}");
        c.line();
        c.line("void SimpleChart::paintEvent(QPaintEvent *)");
        c.line("{");
        c.indent();
        c.line("QPainter painter(this);");
        c.line("painter.setRenderHint(QPainter::Antialiasing);");
        c.line("QRectF area = QRectF(rect()).adjusted(12, 12, -12, -24);");
        c.line("QColor color(Theme::color(u\"primary\"_s));");
        c.line("double maximum = 1;");
        c.line("for (double value : values_)");
        c.indent();
        c.line("maximum = std::max(maximum, value);");
        c.dedent();
        c.line("if (values_.isEmpty())");
        c.indent();
        c.line("return;");
        c.dedent();
        c.line("double step = area.width() / values_.size();");
        c.line("if (kind_ == u\"line\"_s) {");
        c.indent();
        c.line("QPainterPath path;");
        c.line("for (qsizetype i = 0; i < values_.size(); ++i) {");
        c.indent();
        c.line("QPointF point(area.left() + step * (i + 0.5), area.bottom() - area.height() * values_.at(i) / maximum);");
        c.line("if (i == 0)");
        c.indent();
        c.line("path.moveTo(point);");
        c.dedent();
        c.line("else");
        c.indent();
        c.line("path.lineTo(point);");
        c.dedent();
        c.dedent();
        c.line("}");
        c.line("painter.setPen(QPen(color, 2));");
        c.line("painter.drawPath(path);");
        c.dedent();
        c.line("} else if (kind_ == u\"pie\"_s) {");
        c.indent();
        c.line("double total = 0;");
        c.line("for (double value : values_)");
        c.indent();
        c.line("total += value;");
        c.dedent();
        c.line("double angle = 90;");
        c.line("double size = std::min(area.width(), area.height());");
        c.line("QRectF circle(area.center().x() - size / 2, area.center().y() - size / 2, size, size);");
        c.line("for (qsizetype i = 0; i < values_.size(); ++i) {");
        c.indent();
        c.line("double span = total > 0 ? 360.0 * values_.at(i) / total : 0;");
        c.line("QColor slice = color;");
        c.line("slice.setAlphaF(1.0 - 0.6 * static_cast<double>(i) / values_.size());");
        c.line("painter.setPen(Qt::NoPen);");
        c.line("painter.setBrush(slice);");
        c.line("painter.drawPie(circle, static_cast<int>(angle * 16), static_cast<int>(span * 16));");
        c.line("angle += span;");
        c.dedent();
        c.line("}");
        c.dedent();
        c.line("} else {");
        c.indent();
        c.line("painter.setPen(Qt::NoPen);");
        c.line("painter.setBrush(color);");
        c.line("for (qsizetype i = 0; i < values_.size(); ++i) {");
        c.indent();
        c.line("double height = area.height() * values_.at(i) / maximum;");
        c.line("painter.drawRect(QRectF(area.left() + step * i + step * 0.15, area.bottom() - height, step * 0.7, height));");
        c.dedent();
        c.line("}");
        c.dedent();
        c.line("}");
        c.line("painter.setPen(QColor(Theme::color(u\"textMuted\"_s)));");
        c.line("for (qsizetype i = 0; i < labels_.size(); ++i)");
        c.indent();
        c.line("painter.drawText(QRectF(area.left() + step * i, area.bottom() + 4, step, 20), Qt::AlignHCenter, labels_.at(i));");
        c.dedent();
        c.dedent();
        c.line("}");
        return {h.finish("src/application/SimpleChart.h"), c.finish("src/application/SimpleChart.cpp")};
    }

    void emitActionBody(const ApplicationModel& model, const std::string& actionId, CodeWriter& w, std::set<std::string>& customHandlers, std::set<std::string>& includes, WidgetTreeEmitter& tree) const
    {
        const Action* action = model.findAction(actionId);
        if (action == nullptr)
            return;
        if (action->kind == "navigate") {
            w.line("navigator().navigate(" + qtString(action->target) + ");");
        } else if (action->kind == "back") {
            w.line("navigator().back();");
        } else if (action->kind == "setState") {
            const StateVariable* variable = model.findState(action->target);
            w.line("state()." + setterName(action->target) + "(" + jsonLiteralForState(variable, action->value) + ");");
        } else if (action->kind == "toggleState") {
            w.line("state()." + setterName(action->target) + "(!state()." + getterName(action->target) + "());");
        } else if (action->kind == "incrementState") {
            double amount = action->value.is_number() ? action->value.get<double>() : 1;
            w.line("state()." + setterName(action->target) + "(state()." + getterName(action->target) + "() + " + number(amount) + ");");
        } else if (action->kind == "openUrl") {
            includes.insert("QDesktopServices");
            includes.insert("QUrl");
            std::string url = action->target;
            if (url.empty() && action->value.is_string())
                url = action->value.get<std::string>();
            w.line("QDesktopServices::openUrl(QUrl(" + qtString(url) + "));");
        } else if (action->kind == "handler") {
            std::string handler = "handle" + text::toPascalCase(action->target.empty() ? action->id : action->target);
            customHandlers.insert(handler);
            w.line(handler + "();");
        } else if (action->kind == "sequence") {
            for (const auto& step : action->steps)
                emitActionBody(model, step, w, customHandlers, includes, tree);
        } else if (action->kind == "showDialog") {
            w.line(tree.memberName(action->target) + "->show();");
        } else if (action->kind == "dismissDialog") {
            w.line(tree.memberName(action->target) + "->hide();");
        }
    }

    void emitClass(const ApplicationModel& model, const Component& root, const std::string& owner, const std::string& baseClass, const std::string& includePath, const std::string& headerPath, const std::string& sourcePath, GenerationResult& result, bool isScreen) const
    {
        std::set<std::string> includes;
        CodeWriter body;
        body.indent();
        WidgetTreeEmitter tree(model, owner, body, includes, result.diagnostics, isScreen);
        tree.className = baseClass;
        tree.emitRoot(root, "this");
        for (const auto& line : tree.bindingLines)
            body.line(line);
        body.dedent();

        CodeWriter handlers;
        std::set<std::string> customHandlers;
        for (const auto& handler : tree.handlers) {
            std::string parameters;
            if (!handler.signature.empty()) {
                std::string list = handler.signature;
                parameters = list;
            }
            handlers.line();
            handlers.line("void " + baseClass + "::" + handler.name + "(" + parameters + ")");
            handlers.line("{");
            handlers.indent();
            emitActionBody(model, handler.action, handlers, customHandlers, includes, tree);
            handlers.dedent();
            handlers.line("}");
        }

        CodeWriter h;
        h.line("// Generated by FormaIDE. Put your own code in the class that derives from this one; hand edits here are merged on regeneration.");
        h.line("#pragma once");
        h.line();
        h.line("#include <QWidget>");
        h.line();
        std::vector<std::string> forward;
        for (const auto& member : tree.members) {
            if (std::find(forward.begin(), forward.end(), member.type) == forward.end())
                forward.push_back(member.type);
        }
        h.line("class AppNavigator;");
        h.line("class AppState;");
        for (const auto& type : forward)
            h.line("class " + type + ";");
        h.line();
        h.line("class " + baseClass + " : public QWidget");
        h.line("{");
        h.indent();
        h.line("Q_OBJECT");
        h.dedent();
        h.line();
        h.line("public:");
        h.indent();
        h.line("explicit " + baseClass + "(AppNavigator &navigator, AppState &state, QWidget *parent = nullptr);");
        for (const auto& member : tree.members)
            h.line(member.type + " *" + member.name + " = nullptr;");
        h.dedent();
        h.line();
        h.line("protected:");
        h.indent();
        h.line("AppNavigator &navigator() const { return navigator_; }");
        h.line("AppState &state() const { return state_; }");
        h.line("AppNavigator *navigatorPointer() const { return &navigator_; }");
        h.line("AppState *statePointer() const { return &state_; }");
        for (const auto& handler : customHandlers)
            h.line("virtual void " + handler + "() {}");
        h.dedent();
        h.line();
        if (!tree.handlers.empty()) {
            h.line("protected Q_SLOTS:");
            h.indent();
            for (const auto& handler : tree.handlers)
                h.line("virtual void " + handler.name + "(" + handler.signature + ");");
            h.dedent();
            h.line();
        }
        h.line("private:");
        h.indent();
        h.line("AppNavigator &navigator_;");
        h.line("AppState &state_;");
        h.dedent();
        h.line("};");
        result.files.push_back(h.finish(headerPath));

        CodeWriter c;
        c.line("// Generated by FormaIDE. Put your own code in the class that derives from this one; hand edits here are merged on regeneration.");
        c.line("#include \"" + includePath + "\"");
        c.line();
        std::vector<std::string> systemIncludes;
        std::vector<std::string> localIncludes;
        for (const auto& include : includes) {
            if (text::contains(include, "/") || text::endsWith(include, ".h"))
                localIncludes.push_back(include);
            else
                systemIncludes.push_back(include);
        }
        for (const auto& include : systemIncludes)
            c.line("#include <" + include + ">");
        c.line();
        c.line("#include \"application/AppNavigator.h\"");
        c.line("#include \"application/AppState.h\"");
        c.line("#include \"application/StyleHelpers.h\"");
        c.line("#include \"theme/Theme.h\"");
        for (const auto& include : localIncludes)
            c.line("#include \"" + include + "\"");
        c.line();
        c.line(baseClass + "::" + baseClass + "(AppNavigator &navigator, AppState &state, QWidget *parent)");
        c.indent();
        c.line(": QWidget(parent), navigator_(navigator), state_(state)");
        c.dedent();
        c.line("{");
        int bodyOffset = c.currentLine() - 1;
        c.raw(body.text());
        c.line("}");
        c.raw(handlers.text());
        GeneratedFile source = c.finish(sourcePath);
        GeneratedFile bodyFile = body.finish("body");
        for (auto binding : bodyFile.bindings) {
            binding.line += bodyOffset;
            source.bindings.push_back(std::move(binding));
        }
        for (auto anchor : bodyFile.anchors) {
            anchor.startLine += bodyOffset;
            anchor.endLine += bodyOffset;
            source.anchors.push_back(std::move(anchor));
        }
        result.files.push_back(std::move(source));
    }

    void emitScreen(const ApplicationModel& model, const ScreenNames& item, GenerationResult& result) const
    {
        std::string base = item.className + "Base";
        emitClass(model, item.screen->root, item.screen->root.id, base, "screens/generated/" + base + ".h", "src/screens/generated/" + base + ".h", "src/screens/generated/" + base + ".cpp", result, true);

        CodeWriter h;
        h.line("#pragma once");
        h.line();
        h.line("#include \"screens/generated/" + base + ".h\"");
        h.line();
        h.line("class " + item.className + " : public " + base);
        h.line("{");
        h.indent();
        h.line("Q_OBJECT");
        h.dedent();
        h.line();
        h.line("public:");
        h.indent();
        h.line("using " + base + "::" + base + ";");
        h.dedent();
        h.line("};");
        result.files.push_back(h.finish("src/screens/" + item.className + ".h", FileCategory::User));
    }

    void emitDefinition(const ApplicationModel& model, const ComponentDefinition& definition, GenerationResult& result) const
    {
        std::string name = definitionClassName(definition);
        std::string base = name + "Base";
        emitClass(model, definition.root, definition.root.id, base, "components/generated/" + base + ".h", "src/components/generated/" + base + ".h", "src/components/generated/" + base + ".cpp", result, false);
        CodeWriter h;
        h.line("#pragma once");
        h.line();
        h.line("#include \"components/generated/" + base + ".h\"");
        h.line();
        h.line("class " + name + " : public " + base);
        h.line("{");
        h.indent();
        h.line("Q_OBJECT");
        h.dedent();
        h.line();
        h.line("public:");
        h.indent();
        h.line("using " + base + "::" + base + ";");
        h.dedent();
        h.line("};");
        result.files.push_back(h.finish("src/components/" + name + ".h", FileCategory::User));
    }

    GeneratedFile resourceFile(const ApplicationModel& model) const
    {
        CodeWriter w;
        w.line("<RCC>");
        w.indent();
        w.line("<qresource prefix=\"/\">");
        w.indent();
        for (const auto& asset : model.assets)
            w.line("<file alias=\"" + asset.path + "\">../" + asset.path + "</file>");
        w.dedent();
        w.line("</qresource>");
        w.dedent();
        w.line("</RCC>");
        return w.finish("resources/app.qrc");
    }

    GeneratedFile gitIgnore() const
    {
        return plain(".gitignore", "build/\nCMakeUserPresets.json\ncompile_commands.json\n*.user\n", FileCategory::User);
    }

    GeneratedFile readme(const ApplicationModel& model, const ProjectConfiguration& config) const
    {
        std::string name = config.name.empty() ? model.meta.name : config.name;
        std::string content = "# " + name + "\n\nA Qt 6 Widgets application. Build it with any recent C++ compiler and CMake:\n\n"
            "```sh\ncmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/your_kit\ncmake --build build\n```\n\n"
            "The classes under `src/screens/generated` come from the design in `design/`. Your own logic goes in the classes in `src/screens`, which FormaIDE never overwrites.\n";
        return plain("README.md", std::move(content), FileCategory::User);
    }
};

}

std::unique_ptr<ICodeGenerator> createQtCppGenerator()
{
    return std::make_unique<QtGenerator>();
}

}
