#include "forma/codegen/code_writer.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "generator_support.h"
#include "ui_common.h"

namespace forma::generators {

using namespace ir;
using namespace codegen;

namespace {

std::string py(const std::string& value)
{
    return "\"" + escapeLiteral(value, LiteralStyle::Python) + "\"";
}

std::string lambdaFor(const ApplicationModel& model, const std::string& actionId)
{
    std::vector<std::string> calls;
    for (const auto& action : resolveAction(model, actionId)) {
        if (action.kind == "navigate")
            calls.push_back("app.navigate(" + py(action.target) + ")");
        else if (action.kind == "back")
            calls.push_back("app.back()");
        else if (action.kind == "openUrl")
            calls.push_back("webbrowser.open(" + py(action.target) + ")");
        else if (!action.handler.empty())
            calls.push_back("handlers." + text::toSnakeCase(action.handler) + "(app)");
    }
    if (calls.empty())
        return {};
    std::string joined;
    for (const auto& call : calls) {
        if (!joined.empty())
            joined += ", ";
        joined += call;
    }
    return "lambda *_: (" + joined + ")";
}

GeneratedFile pyproject(const std::string& slug, const std::string& version, const std::vector<std::string>& dependencies)
{
    std::string deps;
    for (const auto& item : dependencies)
        deps += (deps.empty() ? "\"" : ", \"") + item + "\"";
    return plainFile("pyproject.toml", "[project]\nname = \"" + slug + "\"\nversion = \"" + version + "\"\nrequires-python = \">=3.10\"\ndependencies = [" + deps + "]\n");
}

GeneratedFile handlersFile(const ApplicationModel& model)
{
    CodeWriter user;
    user.line("# Your code. FormaIDE won't overwrite this file.");
    user.line();
    for (const auto& handler : collectHandlers(model)) {
        user.line("def " + text::toSnakeCase(handler) + "(app):");
        user.indent();
        user.line("pass");
        user.dedent();
        user.line();
    }
    return user.finish("handlers.py", FileCategory::User);
}

class TkEmitter {
public:
    TkEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)), names_(NameTable::Style::Snake, {"self", "app", "parent", "tk", "ttk", "handlers", "webbrowser"}) {}

    void emit(const Component& c, const Component* parent, const std::string& parentVar, int index)
    {
        w_.beginAnchor(screen_, c.id);
        std::string var = names_.name(c.id);
        BoxStyle box = resolveBox(model_, c);
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::Python};
        std::string bg = parentBackground_.empty() ? rootBackground(model_, *rootOf_) : parentBackground_;
        std::string fontText = "(" + py(box.fontFamily.empty() ? std::string("Segoe UI") : box.fontFamily) + ", " + numberText(-box.fontSize) + (box.fontWeight >= 600 ? ", \"bold\"" : "") + ")";
        if (c.type == "text") {
            w_.bound(var + " = tk.Label(" + parentVar + ", text=\"", escapeLiteral(value, LiteralStyle::Python), "\", font=" + fontText + ", fg=" + py(colorRgbHex(box.color)) + ", bg=" + py(colorRgbHex(bg)) + ", anchor=\"w\", justify=\"left\")", target);
        } else if (c.type == "button") {
            std::string command;
            auto event = c.events.find("onClick");
            if (event != c.events.end()) {
                std::string handler = lambdaFor(model_, event->second);
                if (!handler.empty())
                    command = ", command=" + handler;
            }
            std::string background = box.background.empty() ? bg : box.background;
            w_.bound(var + " = tk.Button(" + parentVar + ", text=\"", escapeLiteral(value, LiteralStyle::Python), "\", font=" + fontText + ", fg=" + py(colorRgbHex(box.color)) + ", bg=" + py(colorRgbHex(background)) + ", activebackground=" + py(colorRgbHex(background)) + ", activeforeground=" + py(colorRgbHex(box.color)) + ", relief=\"flat\", bd=0, padx=" + numberText(box.paddingX) + ", pady=" + numberText(box.paddingY) + command + ")", target);
        } else if (c.type == "textinput") {
            w_.line(var + " = tk.Entry(" + parentVar + ", font=" + fontText + ")");
            std::string initial = propertyString(c, "value");
            if (!initial.empty())
                w_.line(var + ".insert(0, " + py(initial) + ")");
        } else if (c.type == "checkbox" || c.type == "switch") {
            w_.line(var + " = ttk.Checkbutton(" + parentVar + ", text=" + py(value) + ")");
        } else if (c.type == "radio") {
            w_.line(var + " = ttk.Radiobutton(" + parentVar + ", text=" + py(value) + ")");
        } else if (c.type == "dropdown") {
            std::string options;
            for (const auto& option : propertyStrings(c, "options"))
                options += py(option) + ", ";
            w_.line(var + " = ttk.Combobox(" + parentVar + ", state=\"readonly\", values=[" + options + "])");
        } else if (c.type == "slider") {
            w_.line(var + " = ttk.Scale(" + parentVar + ", from_=" + numberText(propertyNumber(c, "min", 0)) + ", to=" + numberText(propertyNumber(c, "max", 100)) + ", value=" + numberText(propertyNumber(c, "value", 0)) + ")");
        } else if (c.type == "divider") {
            w_.line(var + " = ttk.Separator(" + parentVar + ", orient=\"horizontal\")");
        } else if (c.type == "list") {
            w_.line(var + " = tk.Listbox(" + parentVar + ", font=" + fontText + ")");
            for (const auto& item : propertyStrings(c, "items"))
                w_.line(var + ".insert(\"end\", " + py(item) + ")");
        } else {
            std::string background = c.type == "screen" ? rootBackground(model_, c) : (box.background.empty() ? bg : box.background);
            w_.line(var + " = tk.Frame(" + parentVar + ", bg=" + py(colorRgbHex(background)) + (box.borderWidth > 0 && !box.borderColor.empty() ? ", highlightthickness=" + numberText(box.borderWidth) + ", highlightbackground=" + py(colorRgbHex(box.borderColor)) : std::string()) + ")");
        }
        place(c, parent, var, index);
        if (isContainerType(c) && !c.children.empty()) {
            std::string saved = parentBackground_;
            parentBackground_ = c.type == "screen" ? rootBackground(model_, c) : (box.background.empty() ? bg : box.background);
            int position = 0;
            const LayoutSpec& l = c.layout;
            if (l.kind == LayoutKind::Grid) {
                for (std::size_t i = 0; i < l.columns.size(); ++i)
                    w_.line(var + ".columnconfigure(" + std::to_string(i) + ", weight=" + (l.columns[i].kind == TrackKind::Fraction ? numberText(l.columns[i].value) : std::string("0")) + ")");
            }
            for (const auto& child : c.children)
                emit(child, &c, var, position++);
            parentBackground_ = saved;
        }
        w_.endAnchor();
    }

    void setRoot(const Component& root) { rootOf_ = &root; }

private:
    void place(const Component& c, const Component* parent, const std::string& var, int index)
    {
        if (parent == nullptr) {
            w_.line(var + ".pack(fill=\"both\", expand=True)");
            return;
        }
        const LayoutSpec& l = parent->layout;
        double gap = index > 0 ? l.spacing : 0;
        double padX = (l.padding.left + l.padding.right) / 2;
        double padY = (l.padding.top + l.padding.bottom) / 2;
        switch (l.kind) {
        case LayoutKind::Vertical:
        case LayoutKind::Scroll:
        case LayoutKind::Flex: {
            bool isColumn = l.kind != LayoutKind::Flex;
            if (isColumn) {
                std::string anchor = l.align == Align::Center ? "n" : l.align == Align::End ? "ne" : "nw";
                double top = index == 0 ? l.padding.top : gap;
                std::string fill = c.width.mode == SizeMode::Fill || l.align == Align::Stretch ? ", fill=\"x\"" : "";
                std::string expand = c.height.mode == SizeMode::Fill || c.grow > 0 ? ", expand=True, fill=\"both\"" : fill;
                w_.line(var + ".pack(side=\"top\", anchor=\"" + anchor + "\", padx=" + numberText(padX) + ", pady=(" + numberText(top) + ", 0)" + expand + ")");
            } else {
                w_.line(var + ".pack(side=\"left\", padx=(" + numberText(index == 0 ? l.padding.left : gap) + ", 0), pady=" + numberText(padY) + ")");
            }
            break;
        }
        case LayoutKind::Horizontal: {
            std::string expand = c.width.mode == SizeMode::Fill || c.grow > 0 ? ", expand=True, fill=\"both\"" : "";
            w_.line(var + ".pack(side=\"left\", padx=(" + numberText(index == 0 ? l.padding.left : gap) + ", 0), pady=" + numberText(padY) + expand + ")");
            break;
        }
        case LayoutKind::Grid:
            w_.line(var + ".grid(column=" + std::to_string(c.grid.column) + ", row=" + std::to_string(c.grid.row) + ", columnspan=" + std::to_string(c.grid.columnSpan) + ", rowspan=" + std::to_string(c.grid.rowSpan) + ", sticky=\"nsew\", padx=" + numberText(l.spacing / 2) + ", pady=" + numberText(l.rowSpacing.value_or(l.spacing) / 2) + ")");
            break;
        case LayoutKind::Absolute:
        case LayoutKind::Constraint: {
            double x = l.kind == LayoutKind::Absolute ? c.x : c.pins.left.value_or(0);
            double y = l.kind == LayoutKind::Absolute ? c.y : c.pins.top.value_or(0);
            std::string size;
            if (c.width.mode == SizeMode::Fixed)
                size += ", width=" + numberText(c.width.value);
            if (c.height.mode == SizeMode::Fixed)
                size += ", height=" + numberText(c.height.value);
            w_.line(var + ".place(x=" + numberText(x) + ", y=" + numberText(y) + size + ")");
            break;
        }
        }
        if (c.height.mode == SizeMode::Fixed && (c.type == "spacer" || c.type == "container") && l.kind != LayoutKind::Absolute && l.kind != LayoutKind::Constraint)
            w_.line(var + ".configure(height=" + numberText(c.height.value) + ")");
    }

    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
    NameTable names_;
    std::string parentBackground_;
    const Component* rootOf_ = nullptr;
};

class TkGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "python-tk";
        value.label = "Python (Tkinter)";
        value.language = "Python";
        value.framework = "Tkinter";
        value.description = "A desktop app in Python using Tkinter, with no extra packages.";
        value.platforms = {"windows", "macos", "linux"};
        value.requiredTools = {"python"};
        value.buildSystem = "python";
        value.approach = "cross-platform";
        value.preview = "run";
        return value;
    }

    GeneratorCapabilities capabilities() const override
    {
        GeneratorCapabilities value;
        value.components = coreComponentTypes();
        value.layouts = coreLayoutKinds();
        value.events = {"onClick"};
        value.features = {"navigation", "source-map"};
        return value;
    }

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "Tkinter"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        result.files.push_back(pyproject(projectSlug(name), model.meta.version.empty() ? "0.1.0" : model.meta.version, {}));
        CodeWriter main;
        main.line("import tkinter as tk");
        main.line();
        main.line("import screens");
        main.line();
        main.line();
        main.line("class App(tk.Tk):");
        main.indent();
        main.line("def __init__(self):");
        main.indent();
        main.line("super().__init__()");
        main.line("self.title(" + py(name) + ")");
        double width = 960;
        double height = 640;
        if (!model.screens.empty()) {
            const Component& first = model.screens.front().root;
            if (first.width.mode == SizeMode::Fixed)
                width = first.width.value;
            if (first.height.mode == SizeMode::Fixed)
                height = first.height.value;
        }
        main.line("self.geometry(\"" + numberText(width) + "x" + numberText(height) + "\")");
        main.line("self.history = []");
        main.line("self.current = None");
        std::string start = startScreenId(model);
        main.line("self.navigate(" + py(start.empty() && !model.screens.empty() ? model.screens.front().root.id : start) + ")");
        main.dedent();
        main.line();
        main.line("def navigate(self, screen):");
        main.indent();
        main.line("self.history.append(screen)");
        main.line("self.show(screen)");
        main.dedent();
        main.line();
        main.line("def back(self):");
        main.indent();
        main.line("if len(self.history) > 1:");
        main.indent();
        main.line("self.history.pop()");
        main.line("self.show(self.history[-1])");
        main.dedent();
        main.dedent();
        main.line();
        main.line("def show(self, screen):");
        main.indent();
        main.line("if self.current is not None:");
        main.indent();
        main.line("self.current.destroy()");
        main.dedent();
        main.line("self.current = screens.SCREENS[screen](self, self)");
        main.dedent();
        main.dedent();
        main.line();
        main.line();
        main.line("if __name__ == \"__main__\":");
        main.indent();
        main.line("App().mainloop()");
        main.dedent();
        result.files.push_back(main.finish("main.py"));

        CodeWriter w;
        w.line("import tkinter as tk");
        w.line("from tkinter import ttk");
        w.line("import webbrowser");
        w.line();
        w.line("import handlers");
        w.line();
        auto screens = screenNames(model);
        for (const auto& item : screens) {
            w.line();
            w.line("def " + text::toSnakeCase(item.className) + "(parent, app):");
            w.indent();
            TkEmitter emitter(model, w, item.screen->root.id);
            emitter.setRoot(item.screen->root);
            emitter.emit(item.screen->root, nullptr, "parent", 0);
            w.line("return " + NameTable(NameTable::Style::Snake, {"self", "app", "parent", "tk", "ttk", "handlers", "webbrowser"}).name(item.screen->root.id));
            w.dedent();
            w.line();
        }
        w.line();
        w.line("SCREENS = {");
        w.indent();
        for (const auto& item : screens)
            w.line(py(item.screen->root.id) + ": " + text::toSnakeCase(item.className) + ",");
        w.dedent();
        w.line("}");
        result.files.push_back(w.finish("screens.py"));
        result.files.push_back(handlersFile(model));
        result.files.push_back(plainFile(".gitignore", "__pycache__/\n"));
        result.files.push_back(standardReadme(model, config, "Python and Tkinter", "Run `python main.py`.", "handlers.py"));
        return result;
    }
};

class PySideEmitter {
public:
    PySideEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)), names_(NameTable::Style::Snake, {"self", "app", "handlers", "webbrowser"}) {}

    std::string emit(const Component& c)
    {
        w_.beginAnchor(screen_, c.id);
        std::string var = names_.name(c.id);
        BoxStyle box = resolveBox(model_, c);
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::Python};
        if (c.type == "text") {
            w_.bound(var + " = QLabel(\"", escapeLiteral(value, LiteralStyle::Python), "\")", target);
            w_.line(var + ".setWordWrap(True)");
        } else if (c.type == "button") {
            w_.bound(var + " = QPushButton(\"", escapeLiteral(value, LiteralStyle::Python), "\")", target);
            auto event = c.events.find("onClick");
            if (event != c.events.end()) {
                std::string handler = lambdaFor(model_, event->second);
                if (!handler.empty())
                    w_.line(var + ".clicked.connect(" + handler + ")");
            }
        } else if (c.type == "textinput") {
            w_.line(var + " = QLineEdit(" + py(propertyString(c, "value")) + ")");
            w_.line(var + ".setPlaceholderText(" + py(propertyString(c, "placeholder")) + ")");
        } else if (c.type == "checkbox" || c.type == "switch") {
            w_.line(var + " = QCheckBox(" + py(value) + ")");
        } else if (c.type == "radio") {
            w_.line(var + " = QRadioButton(" + py(value) + ")");
        } else if (c.type == "dropdown") {
            w_.line(var + " = QComboBox()");
            for (const auto& option : propertyStrings(c, "options"))
                w_.line(var + ".addItem(" + py(option) + ")");
        } else if (c.type == "slider") {
            w_.line(var + " = QSlider(Qt.Orientation.Horizontal)");
            w_.line(var + ".setRange(" + numberText(propertyNumber(c, "min", 0)) + ", " + numberText(propertyNumber(c, "max", 100)) + ")");
        } else if (c.type == "list") {
            w_.line(var + " = QListWidget()");
            for (const auto& item : propertyStrings(c, "items"))
                w_.line(var + ".addItem(" + py(item) + ")");
        } else if (c.type == "divider") {
            w_.line(var + " = QFrame()");
            w_.line(var + ".setFixedHeight(1)");
            box.background = resolveColor(model_, "$color.border");
        } else {
            w_.line(var + " = QWidget()");
            w_.line(var + ".setObjectName(" + py(var) + ")");
            w_.line(var + ".setAttribute(Qt.WidgetAttribute.WA_StyledBackground)");
            if (c.type == "screen")
                box.background = rootBackground(model_, c);
        }
        std::string css;
        if (!box.background.empty())
            css += "background: " + box.background + "; ";
        if (!box.color.empty() && !isContainerType(c))
            css += "color: " + box.color + "; ";
        if (box.borderWidth > 0 && !box.borderColor.empty())
            css += "border: " + numberText(box.borderWidth) + "px solid " + box.borderColor + "; ";
        else if (c.type == "button")
            css += "border: none; ";
        if (box.radius > 0)
            css += "border-radius: " + numberText(box.radius) + "px; ";
        if (box.paddingX > 0 || box.paddingY > 0)
            css += "padding: " + numberText(box.paddingY) + "px " + numberText(box.paddingX) + "px; ";
        if (!isContainerType(c)) {
            css += "font-size: " + numberText(box.fontSize) + "px; ";
            if (box.fontWeight != 400)
                css += "font-weight: " + std::to_string(box.fontWeight) + "; ";
        }
        if (!css.empty())
            w_.line(var + ".setStyleSheet(" + py(css) + ")");
        if (c.width.mode == SizeMode::Fixed)
            w_.line(var + ".setFixedWidth(" + numberText(c.width.value) + ")");
        if (c.height.mode == SizeMode::Fixed)
            w_.line(var + ".setFixedHeight(" + numberText(c.height.value) + ")");
        if (isContainerType(c)) {
            const LayoutSpec& l = c.layout;
            std::string layoutVar = var + "_layout";
            bool absolute = l.kind == LayoutKind::Absolute || l.kind == LayoutKind::Constraint;
            if (!absolute) {
                const char* cls = l.kind == LayoutKind::Grid ? "QGridLayout" : l.kind == LayoutKind::Horizontal || l.kind == LayoutKind::Flex ? "QHBoxLayout" : "QVBoxLayout";
                w_.line(layoutVar + " = " + cls + "(" + var + ")");
                w_.line(layoutVar + ".setContentsMargins(" + numberText(l.padding.left) + ", " + numberText(l.padding.top) + ", " + numberText(l.padding.right) + ", " + numberText(l.padding.bottom) + ")");
                w_.line(layoutVar + ".setSpacing(" + numberText(l.spacing) + ")");
            }
            for (const auto& child : c.children) {
                std::string childVar = emit(child);
                if (absolute) {
                    double x = l.kind == LayoutKind::Absolute ? child.x : child.pins.left.value_or(0);
                    double y = l.kind == LayoutKind::Absolute ? child.y : child.pins.top.value_or(0);
                    w_.line(childVar + ".setParent(" + var + ")");
                    w_.line(childVar + ".move(" + numberText(x) + ", " + numberText(y) + ")");
                } else if (l.kind == LayoutKind::Grid) {
                    w_.line(layoutVar + ".addWidget(" + childVar + ", " + std::to_string(child.grid.row) + ", " + std::to_string(child.grid.column) + ", " + std::to_string(child.grid.rowSpan) + ", " + std::to_string(child.grid.columnSpan) + ")");
                } else {
                    bool column = l.kind != LayoutKind::Horizontal && l.kind != LayoutKind::Flex;
                    bool fills = column ? child.height.mode == SizeMode::Fill : child.width.mode == SizeMode::Fill;
                    int stretch = fills || child.grow > 0 ? 1 : 0;
                    bool crossFill = column ? child.width.mode == SizeMode::Fill : child.height.mode == SizeMode::Fill;
                    Align align = child.alignSelf.value_or(l.align);
                    std::string alignment;
                    if (!crossFill && align != Align::Stretch && child.type != "text") {
                        if (column)
                            alignment = align == Align::Center ? "Qt.AlignmentFlag.AlignHCenter" : align == Align::End ? "Qt.AlignmentFlag.AlignRight" : "Qt.AlignmentFlag.AlignLeft";
                        else
                            alignment = align == Align::Center ? "Qt.AlignmentFlag.AlignVCenter" : align == Align::End ? "Qt.AlignmentFlag.AlignBottom" : "Qt.AlignmentFlag.AlignTop";
                    }
                    w_.line(layoutVar + ".addWidget(" + childVar + ", " + std::to_string(stretch) + (alignment.empty() ? std::string() : ", " + alignment) + ")");
                }
            }
            if (!absolute && l.kind != LayoutKind::Grid) {
                bool anyFill = false;
                for (const auto& child : c.children) {
                    bool column = l.kind != LayoutKind::Horizontal && l.kind != LayoutKind::Flex;
                    anyFill = anyFill || child.grow > 0 || (column ? child.height.mode == SizeMode::Fill : child.width.mode == SizeMode::Fill);
                }
                if (!anyFill && l.justify == Justify::Start)
                    w_.line(layoutVar + ".addStretch(1)");
            }
        }
        w_.endAnchor();
        return var;
    }

private:
    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
    NameTable names_;
};

class PySideGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "pyside6";
        value.label = "Python (PySide6)";
        value.language = "Python";
        value.framework = "Qt for Python";
        value.description = "A desktop app in Python using PySide6.";
        value.platforms = {"windows", "macos", "linux"};
        value.requiredTools = {"python"};
        value.buildSystem = "python";
        value.approach = "cross-platform";
        value.preview = "run";
        return value;
    }

    GeneratorCapabilities capabilities() const override
    {
        GeneratorCapabilities value;
        value.components = coreComponentTypes();
        value.layouts = coreLayoutKinds();
        value.events = {"onClick"};
        value.features = {"navigation", "source-map"};
        return value;
    }

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "PySide6"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        result.files.push_back(pyproject(projectSlug(name), model.meta.version.empty() ? "0.1.0" : model.meta.version, {"PySide6>=6.10"}));
        result.files.push_back(plainFile("requirements.txt", "PySide6>=6.10\n"));
        double width = 960;
        double height = 640;
        if (!model.screens.empty()) {
            const Component& first = model.screens.front().root;
            if (first.width.mode == SizeMode::Fixed)
                width = first.width.value;
            if (first.height.mode == SizeMode::Fixed)
                height = first.height.value;
        }
        std::string start = startScreenId(model);
        CodeWriter main;
        main.line("import sys");
        main.line();
        main.line("from PySide6.QtWidgets import QApplication, QMainWindow, QStackedWidget");
        main.line();
        main.line("import screens");
        main.line();
        main.line();
        main.line("class App(QMainWindow):");
        main.indent();
        main.line("def __init__(self):");
        main.indent();
        main.line("super().__init__()");
        main.line("self.setWindowTitle(" + py(name) + ")");
        main.line("self.resize(" + numberText(width) + ", " + numberText(height) + ")");
        main.line("self.history = []");
        main.line("self.navigate(" + py(start.empty() && !model.screens.empty() ? model.screens.front().root.id : start) + ")");
        main.dedent();
        main.line();
        main.line("def navigate(self, screen):");
        main.indent();
        main.line("self.history.append(screen)");
        main.line("self.setCentralWidget(screens.SCREENS[screen](self))");
        main.dedent();
        main.line();
        main.line("def back(self):");
        main.indent();
        main.line("if len(self.history) > 1:");
        main.indent();
        main.line("self.history.pop()");
        main.line("self.setCentralWidget(screens.SCREENS[self.history[-1]](self))");
        main.dedent();
        main.dedent();
        main.dedent();
        main.line();
        main.line();
        main.line("if __name__ == \"__main__\":");
        main.indent();
        main.line("application = QApplication(sys.argv)");
        main.line("window = App()");
        main.line("window.show()");
        main.line("sys.exit(application.exec())");
        main.dedent();
        result.files.push_back(main.finish("main.py"));

        CodeWriter w;
        w.line("import webbrowser");
        w.line();
        w.line("from PySide6.QtCore import Qt");
        w.line("from PySide6.QtWidgets import (QCheckBox, QComboBox, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QListWidget, QPushButton, QRadioButton, QSlider, QVBoxLayout, QWidget)");
        w.line();
        w.line("import handlers");
        w.line();
        auto screens = screenNames(model);
        for (const auto& item : screens) {
            w.line();
            w.line("def " + text::toSnakeCase(item.className) + "(app):");
            w.indent();
            PySideEmitter emitter(model, w, item.screen->root.id);
            std::string root = emitter.emit(item.screen->root);
            w.line("return " + root);
            w.dedent();
            w.line();
        }
        w.line();
        w.line("SCREENS = {");
        w.indent();
        for (const auto& item : screens)
            w.line(py(item.screen->root.id) + ": " + text::toSnakeCase(item.className) + ",");
        w.dedent();
        w.line("}");
        result.files.push_back(w.finish("screens.py"));
        result.files.push_back(handlersFile(model));
        result.files.push_back(plainFile(".gitignore", "__pycache__/\n.venv/\n"));
        result.files.push_back(standardReadme(model, config, "Python and PySide6", "Run `pip install -r requirements.txt`, then `python main.py`.", "handlers.py"));
        return result;
    }
};

}

std::unique_ptr<ICodeGenerator> createTkGenerator()
{
    return std::make_unique<TkGenerator>();
}

std::unique_ptr<ICodeGenerator> createPySideGenerator()
{
    return std::make_unique<PySideGenerator>();
}

}
