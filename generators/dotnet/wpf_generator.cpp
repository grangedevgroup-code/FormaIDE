#include "forma/codegen/code_writer.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "generator_support.h"
#include "ui_common.h"

namespace forma::generators {

using namespace ir;
using namespace codegen;

namespace {

std::string attr(const std::string& value)
{
    return escapeLiteral(value, LiteralStyle::Xml);
}

std::string csString(const std::string& value)
{
    return "\"" + escapeLiteral(value, LiteralStyle::CLike) + "\"";
}

std::string thickness(const EdgeInsets& e)
{
    return numberText(e.left) + "," + numberText(e.top) + "," + numberText(e.right) + "," + numberText(e.bottom);
}

class XamlEmitter {
public:
    XamlEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen, NameTable& names, std::vector<std::pair<std::string, std::string>>& clicks)
        : model_(model), w_(w), screen_(std::move(screen)), names_(names), clicks_(clicks) {}

    void emit(const Component& c, const Component* parent, int index)
    {
        w_.beginAnchor(screen_, c.id);
        BoxStyle box = resolveBox(model_, c);
        std::string common = commonAttributes(c, parent, index);
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::Xml};
        std::string font = fontAttributes(box);
        if (c.type == "text") {
            w_.bound("<TextBlock" + common + font + " TextWrapping=\"Wrap\" Text=\"", attr(value), "\"/>", target);
        } else if (c.type == "button") {
            std::string name = names_.name(c.id);
            auto event = c.events.find("onClick");
            std::string click;
            if (event != c.events.end()) {
                click = " Click=\"" + name + "_Click\"";
                clicks_.push_back({name, event->second});
            }
            w_.line("<Button" + common + font + click + " Padding=\"" + numberText(box.paddingX) + "," + numberText(box.paddingY) + "\"" + brush("Background", box.background.empty() ? "#00FFFFFF" : box.background) + brush("BorderBrush", box.borderColor.empty() ? "#00FFFFFF" : box.borderColor) + " BorderThickness=\"" + numberText(box.borderWidth) + "\">");
            w_.indent();
            w_.line("<Button.Resources><Style TargetType=\"Border\"><Setter Property=\"CornerRadius\" Value=\"" + numberText(box.radius) + "\"/></Style></Button.Resources>");
            w_.bound("<TextBlock Text=\"", attr(value), "\"/>", target);
            w_.dedent();
            w_.line("</Button>");
        } else if (c.type == "textinput") {
            w_.line("<TextBox" + common + font + " Text=\"" + attr(propertyString(c, "value")) + "\" Padding=\"" + numberText(box.paddingX) + "," + numberText(box.paddingY) + "\"/>");
        } else if (c.type == "checkbox" || c.type == "switch") {
            w_.line("<CheckBox" + common + font + " Content=\"" + attr(value) + "\" IsChecked=\"" + (propertyBool(c, "checked") ? "True" : "False") + "\"/>");
        } else if (c.type == "radio") {
            w_.line("<RadioButton" + common + font + " Content=\"" + attr(value) + "\" IsChecked=\"" + (propertyBool(c, "checked") ? "True" : "False") + "\"/>");
        } else if (c.type == "dropdown") {
            w_.line("<ComboBox" + common + font + " SelectedIndex=\"0\">");
            w_.indent();
            for (const auto& option : propertyStrings(c, "options"))
                w_.line("<ComboBoxItem Content=\"" + attr(option) + "\"/>");
            w_.dedent();
            w_.line("</ComboBox>");
        } else if (c.type == "slider") {
            w_.line("<Slider" + common + " Minimum=\"" + numberText(propertyNumber(c, "min", 0)) + "\" Maximum=\"" + numberText(propertyNumber(c, "max", 100)) + "\" Value=\"" + numberText(propertyNumber(c, "value", 0)) + "\"/>");
        } else if (c.type == "image") {
            std::string source = propertyString(c, "source");
            if (source.empty())
                w_.line("<Border" + common + brush("Background", resolveColor(model_, "$color.border")) + "/>");
            else
                w_.line("<Image" + common + " Source=\"" + attr(source) + "\"/>");
        } else if (c.type == "divider") {
            w_.line("<Border" + common + " Height=\"1\"" + brush("Background", resolveColor(model_, "$color.border")) + "/>");
        } else if (c.type == "spacer") {
            w_.line("<Border" + common + "/>");
        } else if (c.type == "list") {
            w_.line("<ListBox" + common + font + ">");
            w_.indent();
            for (const auto& item : propertyStrings(c, "items"))
                w_.line("<ListBoxItem Content=\"" + attr(item) + "\"/>");
            w_.dedent();
            w_.line("</ListBox>");
        } else {
            emitContainer(c, parent, index, common, box);
        }
        w_.endAnchor();
    }

    std::string brush(const char* key, const std::string& color) const
    {
        return color.empty() ? std::string() : std::string(" ") + key + "=\"" + argb(color) + "\"";
    }

private:
    static std::string argb(const std::string& color)
    {
        if (color.size() == 9 && color[0] == '#')
            return "#" + color.substr(7, 2) + color.substr(1, 6);
        return color;
    }

    std::string fontAttributes(const BoxStyle& box) const
    {
        std::string out = " FontSize=\"" + numberText(box.fontSize) + "\"" + brush("Foreground", box.color);
        if (box.fontWeight != 400)
            out += " FontWeight=\"" + std::to_string(box.fontWeight) + "\"";
        if (box.italic)
            out += " FontStyle=\"Italic\"";
        if (!box.fontFamily.empty())
            out += " FontFamily=\"" + attr(box.fontFamily) + "\"";
        return out;
    }

    std::string commonAttributes(const Component& c, const Component* parent, int index) const
    {
        std::string out;
        if (c.width.mode == SizeMode::Fixed)
            out += " Width=\"" + numberText(c.width.value) + "\"";
        if (c.height.mode == SizeMode::Fixed)
            out += " Height=\"" + numberText(c.height.value) + "\"";
        if (c.width.min)
            out += " MinWidth=\"" + numberText(*c.width.min) + "\"";
        if (c.width.max)
            out += " MaxWidth=\"" + numberText(*c.width.max) + "\"";
        if (c.height.min)
            out += " MinHeight=\"" + numberText(*c.height.min) + "\"";
        if (c.height.max)
            out += " MaxHeight=\"" + numberText(*c.height.max) + "\"";
        EdgeInsets margin = c.margin;
        if (parent != nullptr && index > 0 && parent->layout.spacing > 0) {
            if (parent->layout.kind == LayoutKind::Vertical || parent->layout.kind == LayoutKind::Scroll)
                margin.top += parent->layout.spacing;
            else if (parent->layout.kind == LayoutKind::Horizontal || parent->layout.kind == LayoutKind::Flex)
                margin.left += parent->layout.spacing;
        }
        if (!margin.isZero())
            out += " Margin=\"" + thickness(margin) + "\"";
        if (parent != nullptr) {
            const LayoutSpec& l = parent->layout;
            if (l.kind == LayoutKind::Absolute)
                out += " Canvas.Left=\"" + numberText(c.x) + "\" Canvas.Top=\"" + numberText(c.y) + "\"";
            if (l.kind == LayoutKind::Constraint) {
                if (c.pins.left)
                    out += " Canvas.Left=\"" + numberText(*c.pins.left) + "\"";
                if (c.pins.top)
                    out += " Canvas.Top=\"" + numberText(*c.pins.top) + "\"";
                if (c.pins.right)
                    out += " Canvas.Right=\"" + numberText(*c.pins.right) + "\"";
                if (c.pins.bottom)
                    out += " Canvas.Bottom=\"" + numberText(*c.pins.bottom) + "\"";
            }
            if (l.kind == LayoutKind::Grid) {
                out += " Grid.Column=\"" + std::to_string(c.grid.column) + "\" Grid.Row=\"" + std::to_string(c.grid.row) + "\"";
                if (c.grid.columnSpan > 1)
                    out += " Grid.ColumnSpan=\"" + std::to_string(c.grid.columnSpan) + "\"";
                if (c.grid.rowSpan > 1)
                    out += " Grid.RowSpan=\"" + std::to_string(c.grid.rowSpan) + "\"";
            }
            Align align = c.alignSelf.value_or(l.align);
            bool fills = (l.kind == LayoutKind::Vertical || l.kind == LayoutKind::Scroll) ? c.width.mode == SizeMode::Fill : c.height.mode == SizeMode::Fill;
            const char* value = fills || align == Align::Stretch ? "Stretch" : align == Align::Center ? "Center" : align == Align::End ? "Right" : "Left";
            if (l.kind == LayoutKind::Vertical || l.kind == LayoutKind::Scroll)
                out += std::string(" HorizontalAlignment=\"") + value + "\"";
            else if (l.kind == LayoutKind::Horizontal || l.kind == LayoutKind::Flex) {
                const char* vertical = fills || align == Align::Stretch ? "Stretch" : align == Align::Center ? "Center" : align == Align::End ? "Bottom" : "Top";
                out += std::string(" VerticalAlignment=\"") + vertical + "\"";
            }
        }
        if (c.style.opacity < 1)
            out += " Opacity=\"" + numberText(c.style.opacity) + "\"";
        if (!c.visible)
            out += " Visibility=\"Collapsed\"";
        return out;
    }

    void emitContainer(const Component& c, const Component* parent, int index, const std::string& common, const BoxStyle& box)
    {
        (void)parent;
        (void)index;
        std::string background = c.type == "screen" ? rootBackground(model_, c) : box.background;
        w_.line("<Border" + common + brush("Background", background) + brush("BorderBrush", box.borderColor) + " BorderThickness=\"" + numberText(box.borderWidth) + "\" CornerRadius=\"" + numberText(box.radius) + "\" Padding=\"" + thickness(c.layout.padding) + "\">");
        w_.indent();
        const LayoutSpec& l = c.layout;
        bool scroll = l.kind == LayoutKind::Scroll;
        if (scroll)
            w_.line(std::string("<ScrollViewer VerticalScrollBarVisibility=\"Auto\"") + (l.scroll != ScrollAxis::Vertical ? " HorizontalScrollBarVisibility=\"Auto\"" : "") + ">");
        std::string panel;
        std::string attributes;
        switch (l.kind) {
        case LayoutKind::Vertical:
        case LayoutKind::Scroll:
            panel = "StackPanel";
            break;
        case LayoutKind::Horizontal:
            panel = "StackPanel";
            attributes = " Orientation=\"Horizontal\"";
            break;
        case LayoutKind::Flex:
            panel = l.wrap ? "WrapPanel" : "StackPanel";
            attributes = l.wrap ? "" : " Orientation=\"Horizontal\"";
            break;
        case LayoutKind::Grid:
            panel = "Grid";
            break;
        case LayoutKind::Absolute:
        case LayoutKind::Constraint:
            panel = "Canvas";
            break;
        }
        w_.line("<" + panel + attributes + ">");
        w_.indent();
        if (l.kind == LayoutKind::Grid) {
            w_.line("<Grid.ColumnDefinitions>");
            w_.indent();
            for (const auto& track : l.columns)
                w_.line("<ColumnDefinition Width=\"" + trackText(track) + "\"/>");
            w_.dedent();
            w_.line("</Grid.ColumnDefinitions>");
            w_.line("<Grid.RowDefinitions>");
            w_.indent();
            for (const auto& track : l.rows)
                w_.line("<RowDefinition Height=\"" + trackText(track) + "\"/>");
            w_.dedent();
            w_.line("</Grid.RowDefinitions>");
        }
        int position = 0;
        for (const auto& child : c.children)
            emit(child, &c, position++);
        w_.dedent();
        w_.line("</" + panel + ">");
        if (scroll)
            w_.line("</ScrollViewer>");
        w_.dedent();
        w_.line("</Border>");
    }

    static std::string trackText(const GridTrack& track)
    {
        if (track.kind == TrackKind::Fixed)
            return numberText(track.value);
        if (track.kind == TrackKind::Fraction)
            return numberText(track.value) + "*";
        return "Auto";
    }

    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
    NameTable& names_;
    std::vector<std::pair<std::string, std::string>>& clicks_;
};

class WpfGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "wpf-csharp";
        value.label = "WPF (C#)";
        value.language = "C#";
        value.framework = "WPF on .NET";
        value.description = "A Windows desktop app in C# with XAML, built with the .NET SDK.";
        value.platforms = {"windows"};
        value.requiredTools = {"dotnet"};
        value.buildSystem = "dotnet";
        value.approach = "native";
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

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "WPF"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        std::string ns = projectClass(name);
        CodeWriter project("  ");
        project.line("<Project Sdk=\"Microsoft.NET.Sdk\">");
        project.indent();
        project.line("<PropertyGroup>");
        project.indent();
        project.line("<OutputType>WinExe</OutputType>");
        project.line("<TargetFramework>net10.0-windows</TargetFramework>");
        project.line("<UseWPF>true</UseWPF>");
        project.line("<Nullable>enable</Nullable>");
        project.line("<RootNamespace>" + ns + "</RootNamespace>");
        project.line("<AssemblyName>" + ns + "</AssemblyName>");
        project.dedent();
        project.line("</PropertyGroup>");
        project.dedent();
        project.line("</Project>");
        result.files.push_back(project.finish(ns + ".csproj"));
        result.files.push_back(plainFile("App.xaml", "<Application x:Class=\"" + ns + ".App\" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\" StartupUri=\"MainWindow.xaml\"/>\n"));
        result.files.push_back(plainFile("App.xaml.cs", "namespace " + ns + ";\n\npublic partial class App : System.Windows.Application\n{\n}\n"));
        auto screens = screenNames(model);
        std::string start = startScreenId(model);
        double width = 960;
        double height = 640;
        if (!screens.empty()) {
            const Component& first = screens.front().screen->root;
            if (first.width.mode == SizeMode::Fixed)
                width = first.width.value;
            if (first.height.mode == SizeMode::Fixed)
                height = first.height.value;
        }
        result.files.push_back(plainFile("MainWindow.xaml", "<Window x:Class=\"" + ns + ".MainWindow\" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\" Title=\"" + attr(name) + "\" Width=\"" + numberText(width) + "\" Height=\"" + numberText(height) + "\">\n    <ContentControl x:Name=\"Host\"/>\n</Window>\n"));
        CodeWriter window;
        window.line("using System.Collections.Generic;");
        window.line("using System.Windows;");
        window.line("using System.Windows.Controls;");
        window.line();
        window.line("namespace " + ns + ";");
        window.line();
        window.line("public partial class MainWindow : Window");
        window.line("{");
        window.indent();
        window.line("private readonly Stack<string> history = new();");
        window.line();
        window.line("public MainWindow()");
        window.line("{");
        window.indent();
        window.line("InitializeComponent();");
        window.line("Navigate(" + csString(start.empty() && !screens.empty() ? screens.front().screen->root.id : start) + ");");
        window.dedent();
        window.line("}");
        window.line();
        window.line("public void Navigate(string screen)");
        window.line("{");
        window.indent();
        window.line("history.Push(screen);");
        window.line("Host.Content = Create(screen);");
        window.dedent();
        window.line("}");
        window.line();
        window.line("public void Back()");
        window.line("{");
        window.indent();
        window.line("if (history.Count > 1)");
        window.line("{");
        window.indent();
        window.line("history.Pop();");
        window.line("Host.Content = Create(history.Peek());");
        window.dedent();
        window.line("}");
        window.dedent();
        window.line("}");
        window.line();
        window.line("private UserControl? Create(string screen) => screen switch");
        window.line("{");
        window.indent();
        for (const auto& item : screens)
            window.line(csString(item.screen->root.id) + " => new " + item.className + "(this),");
        window.line("_ => null,");
        window.dedent();
        window.line("};");
        window.dedent();
        window.line("}");
        result.files.push_back(window.finish("MainWindow.xaml.cs"));

        std::set<std::string> handlers;
        for (const auto& item : screens) {
            CodeWriter xaml;
            NameTable names(NameTable::Style::Pascal, {"Host", "Window", "Content"});
            std::vector<std::pair<std::string, std::string>> clicks;
            xaml.line("<UserControl x:Class=\"" + ns + "." + item.className + "\" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\">");
            xaml.indent();
            Component root = item.screen->root;
            root.width.mode = SizeMode::Fill;
            root.height.mode = SizeMode::Fill;
            XamlEmitter(model, xaml, item.screen->root.id, names, clicks).emit(root, nullptr, 0);
            xaml.dedent();
            xaml.line("</UserControl>");
            result.files.push_back(xaml.finish("Screens/" + item.className + ".xaml"));

            CodeWriter code;
            code.line("using System.Windows;");
            code.line("using System.Windows.Controls;");
            code.line();
            code.line("namespace " + ns + ";");
            code.line();
            code.line("public partial class " + item.className + " : UserControl");
            code.line("{");
            code.indent();
            code.line("private readonly MainWindow window;");
            code.line();
            code.line("public " + item.className + "(MainWindow window)");
            code.line("{");
            code.indent();
            code.line("this.window = window;");
            code.line("InitializeComponent();");
            code.dedent();
            code.line("}");
            for (const auto& [control, actionId] : clicks) {
                code.line();
                code.line("private void " + control + "_Click(object sender, RoutedEventArgs e)");
                code.line("{");
                code.indent();
                for (const auto& action : resolveAction(model, actionId)) {
                    if (action.kind == "navigate")
                        code.line("window.Navigate(" + csString(action.target) + ");");
                    else if (action.kind == "back")
                        code.line("window.Back();");
                    else if (action.kind == "openUrl")
                        code.line("System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(" + csString(action.target) + ") { UseShellExecute = true });");
                    else if (!action.handler.empty()) {
                        handlers.insert(action.handler);
                        code.line("Handlers." + text::toPascalCase(action.handler) + "(window);");
                    }
                }
                code.dedent();
                code.line("}");
            }
            code.dedent();
            code.line("}");
            result.files.push_back(code.finish("Screens/" + item.className + ".xaml.cs"));
        }
        CodeWriter user;
        user.line("namespace " + ns + ";");
        user.line();
        user.line("// Your code. FormaIDE won't overwrite this file.");
        user.line("public static class Handlers");
        user.line("{");
        user.indent();
        for (const auto& handler : collectHandlers(model))
            user.line("public static void " + text::toPascalCase(handler) + "(MainWindow window) { }");
        user.dedent();
        user.line("}");
        result.files.push_back(user.finish("Handlers.cs", FileCategory::User));
        result.files.push_back(plainFile(".gitignore", "bin/\nobj/\n"));
        result.files.push_back(standardReadme(model, config, "WPF", "Run `dotnet run`.", "Handlers.cs"));
        return result;
    }
};

}

std::unique_ptr<ICodeGenerator> createWpfGenerator()
{
    return std::make_unique<WpfGenerator>();
}

}
