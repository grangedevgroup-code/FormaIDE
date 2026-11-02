#include <map>

#include "forma/codegen/code_writer.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "generator_support.h"
#include "ui_common.h"

namespace forma::generators {

using namespace ir;
using namespace codegen;

namespace {

std::string cssId(const std::string& id)
{
    std::string out;
    for (char c : id)
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '-');
    return "c-" + out;
}

std::string cssSize(const SizeSpec& size)
{
    switch (size.mode) {
    case SizeMode::Fixed:
        return numberText(size.value) + "px";
    case SizeMode::Percent:
        return numberText(size.value) + "%";
    case SizeMode::Fill:
        return "100%";
    case SizeMode::Hug:
        return "";
    }
    return "";
}

const char* cssAlign(Align align)
{
    switch (align) {
    case Align::Center:
        return "center";
    case Align::End:
        return "flex-end";
    case Align::Stretch:
        return "stretch";
    case Align::Start:
        return "flex-start";
    }
    return "flex-start";
}

const char* cssJustify(Justify justify)
{
    switch (justify) {
    case Justify::Center:
        return "center";
    case Justify::End:
        return "flex-end";
    case Justify::SpaceBetween:
        return "space-between";
    case Justify::SpaceAround:
        return "space-around";
    case Justify::SpaceEvenly:
        return "space-evenly";
    case Justify::Start:
        return "flex-start";
    }
    return "flex-start";
}

std::string gridTracks(const std::vector<GridTrack>& tracks)
{
    std::string out;
    for (const auto& track : tracks) {
        if (!out.empty())
            out += " ";
        if (track.kind == TrackKind::Fixed)
            out += numberText(track.value) + "px";
        else if (track.kind == TrackKind::Fraction)
            out += numberText(track.value) + "fr";
        else
            out += "auto";
    }
    return out;
}

void writeRule(CodeWriter& w, const ApplicationModel& model, const Component& c, const Component* parent)
{
    BoxStyle box = resolveBox(model, c);
    std::vector<std::string> d;
    auto add = [&](const std::string& key, const std::string& value) {
        if (!value.empty())
            d.push_back(key + ": " + value + ";");
    };
    std::string width = cssSize(c.width);
    std::string height = cssSize(c.height);
    bool parentRow = parent != nullptr && (parent->layout.kind == LayoutKind::Horizontal || parent->layout.kind == LayoutKind::Flex);
    bool parentColumn = parent != nullptr && parent->layout.kind == LayoutKind::Vertical;
    if (c.width.mode == SizeMode::Fill && parentRow) {
        add("flex", "1 1 0");
    } else {
        add("width", width);
    }
    if (c.height.mode == SizeMode::Fill && parentColumn)
        add("flex", "1 1 0");
    else
        add("height", height);
    if (c.width.min)
        add("min-width", numberText(*c.width.min) + "px");
    if (c.width.max)
        add("max-width", numberText(*c.width.max) + "px");
    if (c.height.min)
        add("min-height", numberText(*c.height.min) + "px");
    if (c.height.max)
        add("max-height", numberText(*c.height.max) + "px");
    if (c.grow > 0)
        add("flex-grow", numberText(c.grow));
    if (!c.margin.isZero())
        add("margin", numberText(c.margin.top) + "px " + numberText(c.margin.right) + "px " + numberText(c.margin.bottom) + "px " + numberText(c.margin.left) + "px");
    if (c.alignSelf)
        add("align-self", cssAlign(*c.alignSelf));
    if (parent != nullptr && parent->layout.kind == LayoutKind::Absolute) {
        add("position", "absolute");
        add("left", numberText(c.x) + "px");
        add("top", numberText(c.y) + "px");
    }
    if (parent != nullptr && parent->layout.kind == LayoutKind::Constraint) {
        add("position", "absolute");
        if (c.pins.left)
            add("left", numberText(*c.pins.left) + "px");
        if (c.pins.right)
            add("right", numberText(*c.pins.right) + "px");
        if (c.pins.top)
            add("top", numberText(*c.pins.top) + "px");
        if (c.pins.bottom)
            add("bottom", numberText(*c.pins.bottom) + "px");
    }
    if (parent != nullptr && parent->layout.kind == LayoutKind::Grid) {
        add("grid-column", std::to_string(c.grid.column + 1) + " / span " + std::to_string(c.grid.columnSpan));
        add("grid-row", std::to_string(c.grid.row + 1) + " / span " + std::to_string(c.grid.rowSpan));
    }
    if (isContainerType(c)) {
        const LayoutSpec& l = c.layout;
        switch (l.kind) {
        case LayoutKind::Vertical:
        case LayoutKind::Scroll:
            add("display", "flex");
            add("flex-direction", "column");
            if (l.kind == LayoutKind::Scroll)
                add("overflow", l.scroll == ScrollAxis::Horizontal ? "auto hidden" : l.scroll == ScrollAxis::Both ? "auto" : "hidden auto");
            break;
        case LayoutKind::Horizontal:
            add("display", "flex");
            add("flex-direction", "row");
            break;
        case LayoutKind::Flex:
            add("display", "flex");
            add("flex-direction", "row");
            add("flex-wrap", l.wrap ? "wrap" : "nowrap");
            break;
        case LayoutKind::Grid:
            add("display", "grid");
            add("grid-template-columns", gridTracks(l.columns));
            add("grid-template-rows", gridTracks(l.rows));
            break;
        case LayoutKind::Absolute:
        case LayoutKind::Constraint:
            add("position", "relative");
            break;
        }
        if (l.kind != LayoutKind::Absolute && l.kind != LayoutKind::Constraint) {
            add("align-items", cssAlign(l.align));
            add("justify-content", cssJustify(l.justify));
            if (l.spacing > 0)
                add("gap", numberText(l.rowSpacing.value_or(l.spacing)) + "px " + numberText(l.spacing) + "px");
        }
        if (!l.padding.isZero())
            add("padding", numberText(l.padding.top) + "px " + numberText(l.padding.right) + "px " + numberText(l.padding.bottom) + "px " + numberText(l.padding.left) + "px");
    } else if (box.paddingX > 0 || box.paddingY > 0) {
        add("padding", numberText(box.paddingY) + "px " + numberText(box.paddingX) + "px");
    }
    add("background", box.background);
    if (box.borderWidth > 0 && !box.borderColor.empty())
        add("border", numberText(box.borderWidth) + "px " + (c.style.borderStyle.empty() ? std::string("solid") : c.style.borderStyle) + " " + box.borderColor);
    else if (c.type == "button")
        add("border", "none");
    if (box.radius > 0)
        add("border-radius", numberText(box.radius) + "px");
    if (box.opacity < 1)
        add("opacity", numberText(box.opacity));
    if (c.style.clip)
        add("overflow", "hidden");
    if (c.style.shadow) {
        const Shadow& s = *c.style.shadow;
        add("box-shadow", numberText(s.x) + "px " + numberText(s.y) + "px " + numberText(s.blur) + "px " + numberText(s.spread) + "px " + resolveColor(model, s.color));
    }
    add("color", box.color);
    add("font-size", numberText(box.fontSize) + "px");
    if (box.fontWeight != 400)
        add("font-weight", std::to_string(box.fontWeight));
    if (box.italic)
        add("font-style", "italic");
    if (box.underline)
        add("text-decoration", "underline");
    if (!box.fontFamily.empty())
        add("font-family", "\"" + box.fontFamily + "\", sans-serif");
    add("text-align", box.textAlign);
    if (c.type == "divider") {
        add("border", "none");
        add("border-top", "1px solid " + resolveColor(model, "$color.border"));
        if (c.width.mode == SizeMode::Hug)
            add("width", "100%");
    }
    if (c.type == "spacer" && c.grow <= 0 && c.height.mode == SizeMode::Hug && c.width.mode == SizeMode::Hug)
        add("flex", "1 1 0");
    if (c.type == "button")
        add("cursor", "pointer");
    if (c.visible == false)
        add("display", "none");
    w.line("#" + cssId(c.id) + " {");
    w.indent();
    for (const auto& declaration : d)
        w.line(declaration);
    w.dedent();
    w.line("}");
    for (const auto& child : c.children)
        writeRule(w, model, child, &c);
}

GeneratedFile stylesFile(const ApplicationModel& model, const std::string& path)
{
    CodeWriter w;
    w.line("* { box-sizing: border-box; }");
    w.line("html, body, #root { height: 100%; margin: 0; }");
    w.line("body { font-family: \"Segoe UI\", system-ui, -apple-system, sans-serif; background: " + resolveColor(model, "$color.background", "#FFFFFF") + "; }");
    w.line(".screen { min-height: 100%; }");
    w.line(".screen[hidden] { display: none; }");
    w.line("button, input, select { font: inherit; }");
    w.line();
    for (const auto& screen : model.screens) {
        w.line("#" + cssId(screen.root.id) + " { background: " + rootBackground(model, screen.root) + "; }");
        writeRule(w, model, screen.root, nullptr);
    }
    return w.finish(path);
}

std::string startScreen(const ApplicationModel& model)
{
    std::string start = startScreenId(model);
    return start.empty() && !model.screens.empty() ? model.screens.front().root.id : start;
}

std::string jsString(const std::string& value)
{
    return "\"" + escapeLiteral(value, LiteralStyle::CLike) + "\"";
}

std::string actionCall(const ApplicationModel& model, const std::string& actionId, bool react)
{
    std::string code;
    for (const auto& action : resolveAction(model, actionId)) {
        if (action.kind == "navigate")
            code += react ? "app.navigate(" + jsString(action.target) + "); " : "navigate(" + jsString(action.target) + "); ";
        else if (action.kind == "back")
            code += react ? "app.back(); " : "back(); ";
        else if (action.kind == "openUrl")
            code += "window.open(" + jsString(action.target) + ", \"_blank\", \"noopener\"); ";
        else if (action.kind == "setState")
            code += "app.setState(" + jsString(action.target) + ", " + dumpJson(action.value) + "); ";
        else if (action.kind == "toggleState")
            code += "app.setState(" + jsString(action.target) + ", !app.state[" + jsString(action.target) + "]); ";
        else if (action.kind == "incrementState")
            code += "app.setState(" + jsString(action.target) + ", Number(app.state[" + jsString(action.target) + "]) + " + (action.value.is_number() ? numberText(action.value.get<double>()) : std::string("1")) + "); ";
        else if (!action.handler.empty())
            code += "handlers." + action.handler + "(app); ";
    }
    return code;
}

class HtmlEmitter {
public:
    HtmlEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)) {}

    void emit(const Component& c)
    {
        w_.beginAnchor(screen_, c.id);
        std::string id = " id=\"" + cssId(c.id) + "\"";
        std::string click;
        auto event = c.events.find("onClick");
        if (event != c.events.end() && !actionCall(model_, event->second, false).empty())
            click = " data-action=\"" + escapeLiteral(event->second, LiteralStyle::Html) + "\"";
        auto label = primaryText(c);
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::Html};
        std::string value = label ? label->value : std::string();
        if (c.type == "text") {
            w_.bound("<p" + id + ">", escapeLiteral(value, LiteralStyle::Html), "</p>", target);
        } else if (c.type == "button") {
            w_.bound("<button type=\"button\"" + id + click + ">", escapeLiteral(value, LiteralStyle::Html), "</button>", target);
        } else if (c.type == "textinput") {
            w_.line("<input" + id + " type=\"text\" placeholder=\"" + escapeLiteral(propertyString(c, "placeholder"), LiteralStyle::Html) + "\" value=\"" + escapeLiteral(propertyString(c, "value"), LiteralStyle::Html) + "\">");
        } else if (c.type == "checkbox" || c.type == "radio" || c.type == "switch") {
            std::string kind = c.type == "radio" ? "radio" : "checkbox";
            w_.line("<label" + id + "><input type=\"" + kind + "\"" + (propertyBool(c, "checked") ? " checked" : "") + "> " + escapeLiteral(value, LiteralStyle::Html) + "</label>");
        } else if (c.type == "dropdown") {
            w_.line("<select" + id + ">");
            w_.indent();
            for (const auto& option : propertyStrings(c, "options"))
                w_.line("<option>" + escapeLiteral(option, LiteralStyle::Html) + "</option>");
            w_.dedent();
            w_.line("</select>");
        } else if (c.type == "slider") {
            w_.line("<input" + id + " type=\"range\" min=\"" + numberText(propertyNumber(c, "min", 0)) + "\" max=\"" + numberText(propertyNumber(c, "max", 100)) + "\" value=\"" + numberText(propertyNumber(c, "value", 0)) + "\">");
        } else if (c.type == "image") {
            w_.line("<img" + id + " src=\"" + escapeLiteral(propertyString(c, "source"), LiteralStyle::Html) + "\" alt=\"" + escapeLiteral(propertyString(c, "alt"), LiteralStyle::Html) + "\">");
        } else if (c.type == "divider") {
            w_.line("<hr" + id + ">");
        } else if (c.type == "list") {
            w_.line("<ul" + id + ">");
            w_.indent();
            for (const auto& item : propertyStrings(c, "items"))
                w_.line("<li>" + escapeLiteral(item, LiteralStyle::Html) + "</li>");
            w_.dedent();
            w_.line("</ul>");
        } else {
            std::string classes = c.type == "screen" ? " class=\"screen\"" : std::string();
            if (c.type == "screen")
                w_.line("<section" + id + classes + " hidden>");
            else
                w_.line("<div" + id + click + ">");
            w_.indent();
            for (const auto& child : c.children)
                emit(child);
            w_.dedent();
            w_.line(c.type == "screen" ? "</section>" : "</div>");
        }
        w_.endAnchor();
    }

private:
    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
};

class HtmlCssGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "html-css";
        value.label = "HTML and CSS";
        value.language = "HTML";
        value.framework = "Plain HTML, CSS and JavaScript";
        value.description = "A static site with no build step and no dependencies.";
        value.platforms = {"web"};
        value.requiredTools = {};
        value.buildSystem = "static";
        value.approach = "web";
        value.preview = "run";
        return value;
    }

    GeneratorCapabilities capabilities() const override
    {
        GeneratorCapabilities value;
        value.components = coreComponentTypes();
        value.layouts = coreLayoutKinds();
        value.events = {"onClick"};
        value.features = {"navigation", "state", "source-map"};
        return value;
    }

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "HTML and CSS"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        CodeWriter html("  ");
        html.line("<!doctype html>");
        html.line("<html lang=\"en\">");
        html.line("<head>");
        html.indent();
        html.line("<meta charset=\"utf-8\">");
        html.line("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
        html.line("<title>" + escapeLiteral(name, LiteralStyle::Html) + "</title>");
        html.line("<link rel=\"stylesheet\" href=\"styles.css\">");
        html.dedent();
        html.line("</head>");
        html.line("<body>");
        html.indent();
        for (const auto& screen : model.screens)
            HtmlEmitter(model, html, screen.root.id).emit(screen.root);
        html.line("<script type=\"module\" src=\"app.js\"></script>");
        html.dedent();
        html.line("</body>");
        html.line("</html>");
        result.files.push_back(html.finish("index.html"));
        result.files.push_back(stylesFile(model, "styles.css"));

        CodeWriter js("  ");
        js.line("import * as handlers from \"./handlers.js\";");
        js.line();
        js.line("const history = [];");
        js.line("const app = { state: " + stateObject(model) + ", setState(name, value) { this.state[name] = value; }, navigate, back };");
        js.line();
        js.line("function show(id) {");
        js.indent();
        js.line("document.querySelectorAll(\".screen\").forEach((screen) => { screen.hidden = screen.id !== \"" + std::string("c-") + "\" + id; });");
        js.dedent();
        js.line("}");
        js.line();
        js.line("function navigate(id) {");
        js.indent();
        js.line("history.push(id);");
        js.line("show(id);");
        js.dedent();
        js.line("}");
        js.line();
        js.line("function back() {");
        js.indent();
        js.line("if (history.length > 1) {");
        js.indent();
        js.line("history.pop();");
        js.line("show(history[history.length - 1]);");
        js.dedent();
        js.line("}");
        js.dedent();
        js.line("}");
        js.line();
        js.line("const actions = {");
        js.indent();
        for (const auto& action : model.actions) {
            std::string body = actionCall(model, action.id, false);
            if (!body.empty())
                js.line(jsString(action.id) + ": () => { " + body + "},");
        }
        js.dedent();
        js.line("};");
        js.line();
        js.line("document.addEventListener(\"click\", (event) => {");
        js.indent();
        js.line("const element = event.target.closest(\"[data-action]\");");
        js.line("if (element && actions[element.dataset.action]) actions[element.dataset.action]();");
        js.dedent();
        js.line("});");
        js.line();
        js.line("navigate(" + jsString(cssTrim(startScreen(model))) + ");");
        result.files.push_back(js.finish("app.js"));

        CodeWriter user("  ");
        user.line("// Your code. FormaIDE won't overwrite this file.");
        for (const auto& handler : collectHandlers(model))
            user.line("export function " + handler + "(app) {}");
        result.files.push_back(user.finish("handlers.js", FileCategory::User));
        result.files.push_back(standardReadme(model, config, "plain HTML and CSS", "Open it from FormaIDE with Run, or serve this folder with any web server.", "handlers.js"));
        return result;
    }

private:
    static std::string cssTrim(const std::string& id)
    {
        return cssId(id).substr(2);
    }

    static std::string stateObject(const ApplicationModel& model)
    {
        Json object = Json::object();
        for (const auto& variable : model.state)
            object[variable.name] = variable.initial;
        return dumpJson(object);
    }
};

class JsxEmitter {
public:
    JsxEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)) {}

    void emit(const Component& c)
    {
        w_.beginAnchor(screen_, c.id);
        std::string id = " id=\"" + cssId(c.id) + "\"";
        std::string click;
        auto event = c.events.find("onClick");
        if (event != c.events.end()) {
            std::string body = actionCall(model_, event->second, true);
            if (!body.empty())
                click = " onClick={() => { " + body + "}}";
        }
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::CLike};
        if (c.type == "text") {
            w_.bound("<p" + id + ">{\"", escapeLiteral(value, LiteralStyle::CLike), "\"}</p>", target);
        } else if (c.type == "button") {
            w_.bound("<button type=\"button\"" + id + click + ">{\"", escapeLiteral(value, LiteralStyle::CLike), "\"}</button>", target);
        } else if (c.type == "textinput") {
            w_.line("<input" + id + " type=\"text\" placeholder={" + jsString(propertyString(c, "placeholder")) + "} defaultValue={" + jsString(propertyString(c, "value")) + "} />");
        } else if (c.type == "checkbox" || c.type == "radio" || c.type == "switch") {
            std::string kind = c.type == "radio" ? "radio" : "checkbox";
            w_.line("<label" + id + "><input type=\"" + kind + "\" defaultChecked={" + (propertyBool(c, "checked") ? "true" : "false") + "} /> {" + jsString(value) + "}</label>");
        } else if (c.type == "dropdown") {
            w_.line("<select" + id + ">");
            w_.indent();
            for (const auto& option : propertyStrings(c, "options"))
                w_.line("<option>{" + jsString(option) + "}</option>");
            w_.dedent();
            w_.line("</select>");
        } else if (c.type == "slider") {
            w_.line("<input" + id + " type=\"range\" min={" + numberText(propertyNumber(c, "min", 0)) + "} max={" + numberText(propertyNumber(c, "max", 100)) + "} defaultValue={" + numberText(propertyNumber(c, "value", 0)) + "} />");
        } else if (c.type == "image") {
            w_.line("<img" + id + " src={" + jsString(propertyString(c, "source")) + "} alt={" + jsString(propertyString(c, "alt")) + "} />");
        } else if (c.type == "divider") {
            w_.line("<hr" + id + " />");
        } else if (c.type == "list") {
            w_.line("<ul" + id + ">");
            w_.indent();
            for (const auto& item : propertyStrings(c, "items"))
                w_.line("<li>{" + jsString(item) + "}</li>");
            w_.dedent();
            w_.line("</ul>");
        } else {
            std::string classes = c.type == "screen" ? " className=\"screen\"" : std::string();
            w_.line("<div" + id + classes + click + ">");
            w_.indent();
            for (const auto& child : c.children)
                emit(child);
            w_.dedent();
            w_.line("</div>");
        }
        w_.endAnchor();
    }

private:
    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
};

class ReactGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "react-ts";
        value.label = "React (TypeScript)";
        value.language = "TypeScript";
        value.framework = "React with Vite";
        value.description = "A React web app in TypeScript, built with Vite.";
        value.platforms = {"web"};
        value.requiredTools = {"node", "npm"};
        value.buildSystem = "node";
        value.approach = "web";
        value.preview = "run";
        return value;
    }

    GeneratorCapabilities capabilities() const override
    {
        GeneratorCapabilities value;
        value.components = coreComponentTypes();
        value.layouts = coreLayoutKinds();
        value.events = {"onClick"};
        value.features = {"navigation", "state", "source-map"};
        return value;
    }

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "React"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        std::string slug = projectSlug(name);
        Json package = {
            {"name", slug},
            {"private", true},
            {"version", model.meta.version.empty() ? "0.1.0" : model.meta.version},
            {"type", "module"},
            {"scripts", {{"dev", "vite"}, {"build", "tsc --noEmit && vite build"}, {"preview", "vite preview"}}},
            {"dependencies", {{"react", "^19.3.1"}, {"react-dom", "^19.3.1"}}},
            {"devDependencies", {{"@types/react", "^19.3.0"}, {"@types/react-dom", "^19.3.0"}, {"@vitejs/plugin-react", "^6.2.1"}, {"typescript", "^7.0.2"}, {"vite", "^8.3.5"}}},
        };
        result.files.push_back(plainFile("package.json", dumpJson(package, 2) + "\n"));
        Json tsconfig = {{"compilerOptions", {{"target", "ES2022"}, {"lib", {"ES2022", "DOM", "DOM.Iterable"}}, {"module", "ESNext"}, {"moduleResolution", "bundler"}, {"jsx", "react-jsx"}, {"strict", true}, {"noUnusedLocals", false}, {"skipLibCheck", true}, {"isolatedModules", true}, {"noEmit", true}}}, {"include", {"src"}}};
        result.files.push_back(plainFile("tsconfig.json", dumpJson(tsconfig, 2) + "\n"));
        result.files.push_back(plainFile("vite.config.ts", "import { defineConfig } from \"vite\";\nimport react from \"@vitejs/plugin-react\";\n\nexport default defineConfig({ plugins: [react()] });\n"));
        result.files.push_back(plainFile("index.html", "<!doctype html>\n<html lang=\"en\">\n<head>\n  <meta charset=\"utf-8\">\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>" + escapeLiteral(name, LiteralStyle::Html) + "</title>\n</head>\n<body>\n  <div id=\"root\"></div>\n  <script type=\"module\" src=\"/src/main.tsx\"></script>\n</body>\n</html>\n"));
        result.files.push_back(plainFile("src/main.tsx", "import { StrictMode } from \"react\";\nimport { createRoot } from \"react-dom/client\";\nimport { App } from \"./App\";\nimport \"./styles.css\";\n\ncreateRoot(document.getElementById(\"root\")!).render(\n  <StrictMode>\n    <App />\n  </StrictMode>,\n);\n"));
        result.files.push_back(stylesFile(model, "src/styles.css"));
        result.files.push_back(plainFile("src/vite-env.d.ts", "/// <reference types=\"vite/client\" />\n"));

        CodeWriter context("  ");
        context.line("export interface AppApi {");
        context.indent();
        context.line("state: Record<string, unknown>;");
        context.line("setState(name: string, value: unknown): void;");
        context.line("navigate(screen: string): void;");
        context.line("back(): void;");
        context.dedent();
        context.line("}");
        result.files.push_back(context.finish("src/appApi.ts"));

        auto screens = screenNames(model);
        CodeWriter app("  ");
        app.line("import { useCallback, useMemo, useState } from \"react\";");
        app.line("import type { AppApi } from \"./appApi\";");
        for (const auto& item : screens)
            app.line("import { " + item.className + " } from \"./screens/" + item.className + "\";");
        app.line();
        Json initial = Json::object();
        for (const auto& variable : model.state)
            initial[variable.name] = variable.initial;
        app.line("export function App() {");
        app.indent();
        app.line("const [stack, setStack] = useState<string[]>([" + jsString(startScreen(model)) + "]);");
        app.line("const [state, setValues] = useState<Record<string, unknown>>(" + dumpJson(initial) + ");");
        app.line("const navigate = useCallback((screen: string) => setStack((s) => [...s, screen]), []);");
        app.line("const back = useCallback(() => setStack((s) => (s.length > 1 ? s.slice(0, -1) : s)), []);");
        app.line("const setState = useCallback((name: string, value: unknown) => setValues((v) => ({ ...v, [name]: value })), []);");
        app.line("const api: AppApi = useMemo(() => ({ state, setState, navigate, back }), [state, setState, navigate, back]);");
        app.line("switch (stack[stack.length - 1]) {");
        app.indent();
        for (const auto& item : screens)
            app.line("case " + jsString(item.screen->root.id) + ": return <" + item.className + " app={api} />;");
        app.line("default: return null;");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        result.files.push_back(app.finish("src/App.tsx"));

        for (const auto& item : screens) {
            CodeWriter w("  ");
            w.line("import type { AppApi } from \"../appApi\";");
            w.line("import * as handlers from \"../handlers\";");
            w.line();
            w.line("void handlers;");
            w.line();
            w.line("export function " + item.className + "({ app }: { app: AppApi }) {");
            w.indent();
            w.line("void app;");
            w.line("return (");
            w.indent();
            JsxEmitter(model, w, item.screen->root.id).emit(item.screen->root);
            w.dedent();
            w.line(");");
            w.dedent();
            w.line("}");
            result.files.push_back(w.finish("src/screens/" + item.className + ".tsx"));
        }

        CodeWriter user("  ");
        user.line("import type { AppApi } from \"./appApi\";");
        user.line();
        user.line("// Your code. FormaIDE won't overwrite this file.");
        user.line("export type { AppApi };");
        for (const auto& handler : collectHandlers(model))
            user.line("export function " + handler + "(_app: AppApi): void {}");
        result.files.push_back(user.finish("src/handlers.ts", FileCategory::User));
        result.files.push_back(plainFile(".gitignore", "node_modules/\ndist/\n"));
        result.files.push_back(standardReadme(model, config, "React and TypeScript", "Run `npm install`, then `npm run dev`.", "src/handlers.ts"));
        return result;
    }
};

}

std::unique_ptr<ICodeGenerator> createHtmlCssGenerator()
{
    return std::make_unique<HtmlCssGenerator>();
}

std::unique_ptr<ICodeGenerator> createReactGenerator()
{
    return std::make_unique<ReactGenerator>();
}

}
