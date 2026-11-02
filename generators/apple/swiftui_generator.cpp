#include "forma/codegen/code_writer.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "generator_support.h"
#include "ui_common.h"

namespace forma::generators {

using namespace ir;
using namespace codegen;

namespace {

std::string swift(const std::string& value)
{
    return "\"" + escapeLiteral(value, LiteralStyle::CLike) + "\"";
}

std::string swiftColor(const std::string& color)
{
    return "Color(hex: " + swift(colorRgbHex(color)) + ", alpha: " + numberText(colorAlpha(color) / 255.0) + ")";
}

std::string actionCode(const ApplicationModel& model, const std::string& actionId)
{
    std::string code;
    for (const auto& action : resolveAction(model, actionId)) {
        if (action.kind == "navigate")
            code += "app.navigate(" + swift(action.target) + "); ";
        else if (action.kind == "back")
            code += "app.back(); ";
        else if (action.kind == "openUrl")
            code += "app.openUrl(" + swift(action.target) + "); ";
        else if (!action.handler.empty())
            code += "Handlers." + action.handler + "(app); ";
    }
    return code;
}

class SwiftEmitter {
public:
    SwiftEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)) {}

    std::vector<std::string> states;

    void collect(const Component& c)
    {
        std::string name = "value" + text::toPascalCase(c.id);
        if (c.type == "textinput")
            states.push_back("@State private var " + name + " = " + swift(propertyString(c, "value")));
        else if (c.type == "checkbox" || c.type == "switch" || c.type == "radio")
            states.push_back("@State private var " + name + " = " + (propertyBool(c, "checked") ? "true" : "false"));
        else if (c.type == "dropdown") {
            auto options = propertyStrings(c, "options");
            states.push_back("@State private var " + name + " = " + swift(options.empty() ? std::string() : options.front()));
        } else if (c.type == "slider")
            states.push_back("@State private var " + name + ": Double = " + numberText(propertyNumber(c, "value", propertyNumber(c, "min", 0))));
        for (const auto& child : c.children)
            collect(child);
    }

    void emit(const Component& c, const Component* parent)
    {
        w_.beginAnchor(screen_, c.id);
        BoxStyle box = resolveBox(model_, c);
        std::string name = "value" + text::toPascalCase(c.id);
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::CLike};
        std::string font = ".font(.system(size: " + numberText(box.fontSize) + ", weight: " + weightName(box.fontWeight) + "))";
        if (c.type == "text") {
            w_.bound("Text(\"", escapeLiteral(value, LiteralStyle::CLike), "\")", target);
            w_.indent();
            w_.line(font);
            w_.line(".foregroundStyle(" + swiftColor(box.color) + ")");
            w_.dedent();
        } else if (c.type == "button") {
            std::string click;
            auto event = c.events.find("onClick");
            if (event != c.events.end())
                click = actionCode(model_, event->second);
            w_.line("Button(action: { " + click + "}) {");
            w_.indent();
            w_.bound("Text(\"", escapeLiteral(value, LiteralStyle::CLike), "\")", target);
            w_.indent();
            w_.line(font);
            w_.line(".foregroundStyle(" + swiftColor(box.color) + ")");
            w_.line(".padding(.horizontal, " + numberText(box.paddingX) + ")");
            w_.line(".padding(.vertical, " + numberText(box.paddingY) + ")");
            if (!box.background.empty())
                w_.line(".background(" + swiftColor(box.background) + ")");
            w_.line(".clipShape(RoundedRectangle(cornerRadius: " + numberText(box.radius) + "))");
            if (box.borderWidth > 0 && !box.borderColor.empty())
                w_.line(".overlay(RoundedRectangle(cornerRadius: " + numberText(box.radius) + ").stroke(" + swiftColor(box.borderColor) + ", lineWidth: " + numberText(box.borderWidth) + "))");
            w_.dedent();
            w_.dedent();
            w_.line("}");
            w_.line(".buttonStyle(.plain)");
        } else if (c.type == "textinput") {
            w_.line("TextField(" + swift(propertyString(c, "placeholder")) + ", text: $" + name + ")");
            w_.indent();
            w_.line(".textFieldStyle(.roundedBorder)");
            w_.dedent();
        } else if (c.type == "checkbox" || c.type == "switch" || c.type == "radio") {
            w_.line("Toggle(" + swift(value) + ", isOn: $" + name + ")");
        } else if (c.type == "dropdown") {
            w_.line("Picker(\"\", selection: $" + name + ") {");
            w_.indent();
            for (const auto& option : propertyStrings(c, "options"))
                w_.line("Text(" + swift(option) + ").tag(" + swift(option) + ")");
            w_.dedent();
            w_.line("}");
        } else if (c.type == "slider") {
            w_.line("Slider(value: $" + name + ", in: " + numberText(propertyNumber(c, "min", 0)) + "..." + numberText(propertyNumber(c, "max", 100)) + ")");
        } else if (c.type == "divider") {
            w_.line("Divider()");
        } else if (c.type == "spacer") {
            w_.line("Spacer()");
        } else if (c.type == "list") {
            w_.line("VStack(alignment: .leading) {");
            w_.indent();
            for (const auto& item : propertyStrings(c, "items"))
                w_.line("Text(" + swift(item) + ")" + font);
            w_.dedent();
            w_.line("}");
        } else if (isContainerType(c)) {
            emitContainer(c);
        } else {
            w_.line("Color.clear");
        }
        modifiers(c, parent, box);
        w_.endAnchor();
    }

private:
    static std::string weightName(int weight)
    {
        if (weight >= 700)
            return ".bold";
        if (weight >= 600)
            return ".semibold";
        if (weight >= 500)
            return ".medium";
        return ".regular";
    }

    void modifiers(const Component& c, const Component* parent, const BoxStyle& box)
    {
        w_.indent();
        if (isContainerType(c) && !c.layout.padding.isZero()) {
            const EdgeInsets& p = c.layout.padding;
            w_.line(".padding(EdgeInsets(top: " + numberText(p.top) + ", leading: " + numberText(p.left) + ", bottom: " + numberText(p.bottom) + ", trailing: " + numberText(p.right) + "))");
        }
        std::string width = c.width.mode == SizeMode::Fixed ? numberText(c.width.value) : "";
        std::string height = c.height.mode == SizeMode::Fixed ? numberText(c.height.value) : "";
        if (!width.empty() || !height.empty()) {
            std::string args;
            if (!width.empty())
                args += "width: " + width;
            if (!height.empty())
                args += std::string(args.empty() ? "" : ", ") + "height: " + height;
            w_.line(".frame(" + args + ")");
        }
        if (c.width.mode == SizeMode::Fill || c.height.mode == SizeMode::Fill || c.type == "screen") {
            std::string args;
            if (c.width.mode == SizeMode::Fill || c.type == "screen")
                args += "maxWidth: .infinity";
            if (c.height.mode == SizeMode::Fill || c.type == "screen")
                args += std::string(args.empty() ? "" : ", ") + "maxHeight: .infinity";
            w_.line(".frame(" + args + ", alignment: .topLeading)");
        }
        if (c.type != "button") {
            std::string background = c.type == "screen" ? rootBackground(model_, c) : box.background;
            if (!background.empty())
                w_.line(".background(" + swiftColor(background) + ")");
            if (box.radius > 0)
                w_.line(".clipShape(RoundedRectangle(cornerRadius: " + numberText(box.radius) + "))");
            if (box.borderWidth > 0 && !box.borderColor.empty())
                w_.line(".overlay(RoundedRectangle(cornerRadius: " + numberText(box.radius) + ").stroke(" + swiftColor(box.borderColor) + ", lineWidth: " + numberText(box.borderWidth) + "))");
        }
        if (parent != nullptr && parent->layout.kind == LayoutKind::Absolute)
            w_.line(".offset(x: " + numberText(c.x) + ", y: " + numberText(c.y) + ")");
        if (parent != nullptr && parent->layout.kind == LayoutKind::Constraint)
            w_.line(".offset(x: " + numberText(c.pins.left.value_or(0)) + ", y: " + numberText(c.pins.top.value_or(0)) + ")");
        if (c.style.opacity < 1)
            w_.line(".opacity(" + numberText(c.style.opacity) + ")");
        w_.dedent();
    }

    void emitContainer(const Component& c)
    {
        const LayoutSpec& l = c.layout;
        std::string open;
        bool scroll = l.kind == LayoutKind::Scroll;
        std::string spacing = numberText(l.spacing);
        switch (l.kind) {
        case LayoutKind::Vertical:
        case LayoutKind::Scroll: {
            const char* align = l.align == Align::Center ? ".center" : l.align == Align::End ? ".trailing" : ".leading";
            open = std::string("VStack(alignment: ") + align + ", spacing: " + spacing + ")";
            break;
        }
        case LayoutKind::Horizontal:
        case LayoutKind::Flex:
        case LayoutKind::Grid: {
            const char* align = l.align == Align::Center ? ".center" : l.align == Align::End ? ".bottom" : ".top";
            open = std::string("HStack(alignment: ") + align + ", spacing: " + spacing + ")";
            break;
        }
        case LayoutKind::Absolute:
        case LayoutKind::Constraint:
            open = "ZStack(alignment: .topLeading)";
            break;
        }
        if (scroll) {
            w_.line("ScrollView {");
            w_.indent();
        }
        w_.line(open + " {");
        w_.indent();
        for (const auto& child : c.children)
            emit(child, &c);
        w_.dedent();
        w_.line("}");
        if (scroll) {
            w_.dedent();
            w_.line("}");
        }
    }

    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
};

class SwiftUiGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "swiftui";
        value.label = "SwiftUI";
        value.language = "Swift";
        value.framework = "SwiftUI";
        value.description = "A macOS app in Swift with SwiftUI, built with Swift Package Manager.";
        value.platforms = {"macos"};
        value.requiredTools = {"swift"};
        value.buildSystem = "swiftpm";
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

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "SwiftUI"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        std::string product = projectClass(name);
        result.files.push_back(plainFile("Package.swift", "// swift-tools-version: 6.0\nimport PackageDescription\n\nlet package = Package(\n    name: " + swift(product) + ",\n    platforms: [.macOS(.v14)],\n    targets: [\n        .executableTarget(name: " + swift(product) + ", path: \"Sources\")\n    ]\n)\n"));
        std::string start = startScreenId(model);
        if (start.empty() && !model.screens.empty())
            start = model.screens.front().root.id;
        auto screens = screenNames(model);
        CodeWriter app;
        app.line("import AppKit");
        app.line("import SwiftUI");
        app.line();
        app.line("extension Color {");
        app.indent();
        app.line("init(hex: String, alpha: Double = 1) {");
        app.indent();
        app.line("var value: UInt64 = 0");
        app.line("Scanner(string: String(hex.dropFirst())).scanHexInt64(&value)");
        app.line("self.init(.sRGB, red: Double((value >> 16) & 255) / 255, green: Double((value >> 8) & 255) / 255, blue: Double(value & 255) / 255, opacity: alpha)");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        app.line();
        app.line("@MainActor");
        app.line("final class AppModel: ObservableObject {");
        app.indent();
        app.line("@Published var stack: [String] = [" + swift(start) + "]");
        app.line("func navigate(_ screen: String) { stack.append(screen) }");
        app.line("func back() { if stack.count > 1 { stack.removeLast() } }");
        app.line("func openUrl(_ url: String) { if let target = URL(string: url) { NSWorkspace.shared.open(target) } }");
        app.dedent();
        app.line("}");
        app.line();
        app.line("struct RootView: View {");
        app.indent();
        app.line("@StateObject private var app = AppModel()");
        app.line();
        app.line("var body: some View {");
        app.indent();
        app.line("switch app.stack.last {");
        for (const auto& item : screens) {
            app.line("case " + swift(item.screen->root.id) + ":");
            app.indent();
            app.line(item.className + "(app: app)");
            app.dedent();
        }
        app.line("default:");
        app.indent();
        app.line("EmptyView()");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        app.line();
        app.line("@main");
        app.line("struct " + product + "App: App {");
        app.indent();
        app.line("var body: some Scene {");
        app.indent();
        app.line("WindowGroup(" + swift(name) + ") {");
        app.indent();
        app.line("RootView()");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        app.dedent();
        app.line("}");
        result.files.push_back(app.finish("Sources/App.swift"));
        for (const auto& item : screens) {
            CodeWriter w;
            w.line("import SwiftUI");
            w.line();
            w.line("struct " + item.className + ": View {");
            w.indent();
            w.line("@ObservedObject var app: AppModel");
            SwiftEmitter emitter(model, w, item.screen->root.id);
            emitter.collect(item.screen->root);
            for (const auto& state : emitter.states)
                w.line(state);
            w.line();
            w.line("var body: some View {");
            w.indent();
            Component root = item.screen->root;
            root.width.mode = SizeMode::Fill;
            root.height.mode = SizeMode::Fill;
            emitter.emit(root, nullptr);
            w.dedent();
            w.line("}");
            w.dedent();
            w.line("}");
            result.files.push_back(w.finish("Sources/Screens/" + item.className + ".swift"));
        }
        CodeWriter user;
        user.line("// Your code. FormaIDE won't overwrite this file.");
        user.line("@MainActor");
        user.line("enum Handlers {");
        user.indent();
        for (const auto& handler : collectHandlers(model))
            user.line("static func " + handler + "(_ app: AppModel) {}");
        user.dedent();
        user.line("}");
        result.files.push_back(user.finish("Sources/Handlers.swift", FileCategory::User));
        result.files.push_back(plainFile(".gitignore", ".build/\n.swiftpm/\n"));
        result.files.push_back(standardReadme(model, config, "SwiftUI", "Run `swift run`.", "Sources/Handlers.swift"));
        return result;
    }
};

}

std::unique_ptr<ICodeGenerator> createSwiftUiGenerator()
{
    return std::make_unique<SwiftUiGenerator>();
}

}
