#include "forma/codegen/code_writer.h"
#include "forma/core/text.h"
#include "forma/generators/registry.h"
#include "generator_support.h"
#include "ui_common.h"

namespace forma::generators {

using namespace ir;
using namespace codegen;

namespace {

std::string kt(const std::string& value)
{
    std::string out;
    for (char c : escapeLiteral(value, LiteralStyle::CLike)) {
        if (c == '$')
            out += "\\$";
        else
            out.push_back(c);
    }
    return "\"" + out + "\"";
}

std::string composeColor(const std::string& color)
{
    if (color.empty())
        return "Color.Unspecified";
    std::string hex = color[0] == '#' ? color.substr(1) : color;
    if (hex.size() == 6)
        hex = "FF" + hex;
    else if (hex.size() == 8)
        hex = hex.substr(6, 2) + hex.substr(0, 6);
    return "Color(0x" + hex + ")";
}

std::string actionCode(const ApplicationModel& model, const std::string& actionId)
{
    std::string code;
    for (const auto& action : resolveAction(model, actionId)) {
        if (action.kind == "navigate")
            code += "app.navigate(" + kt(action.target) + "); ";
        else if (action.kind == "back")
            code += "app.back(); ";
        else if (action.kind == "openUrl")
            code += "app.openUrl(" + kt(action.target) + "); ";
        else if (!action.handler.empty())
            code += "Handlers." + action.handler + "(app); ";
    }
    return code;
}

class ComposeEmitter {
public:
    ComposeEmitter(const ApplicationModel& model, CodeWriter& w, std::string screen) : model_(model), w_(w), screen_(std::move(screen)) {}

    void emit(const Component& c, const Component* parent)
    {
        w_.beginAnchor(screen_, c.id);
        BoxStyle box = resolveBox(model_, c);
        std::string modifier = modifierFor(c, parent, box);
        auto label = primaryText(c);
        std::string value = label ? label->value : std::string();
        BindingTarget target{screen_, c.id, label ? label->key : "content", ValueKind::String, LiteralStyle::CLike};
        std::string fontArgs = "fontSize = " + numberText(box.fontSize) + ".sp, fontWeight = FontWeight(" + std::to_string(box.fontWeight) + "), color = " + composeColor(box.color);
        if (c.type == "text") {
            w_.bound("Text(text = \"", ktEscape(value), "\", modifier = " + modifier + ", " + fontArgs + ")", target);
        } else if (c.type == "button") {
            std::string click;
            auto event = c.events.find("onClick");
            if (event != c.events.end())
                click = actionCode(model_, event->second);
            w_.line("Button(onClick = { " + click + "}, modifier = " + modifier + ", shape = RoundedCornerShape(" + numberText(box.radius) + ".dp), colors = ButtonDefaults.buttonColors(containerColor = " + composeColor(box.background.empty() ? std::string("#00000000") : box.background) + ", contentColor = " + composeColor(box.color) + "), contentPadding = PaddingValues(horizontal = " + numberText(box.paddingX) + ".dp, vertical = " + numberText(box.paddingY) + ".dp)" + (box.borderWidth > 0 && !box.borderColor.empty() ? ", border = BorderStroke(" + numberText(box.borderWidth) + ".dp, " + composeColor(box.borderColor) + ")" : std::string()) + ") {");
            w_.indent();
            w_.bound("Text(text = \"", ktEscape(value), "\", fontSize = " + numberText(box.fontSize) + ".sp, fontWeight = FontWeight(" + std::to_string(box.fontWeight) + "))", target);
            w_.dedent();
            w_.line("}");
        } else if (c.type == "textinput") {
            w_.line("var " + varName(c) + " by remember { mutableStateOf(" + kt(propertyString(c, "value")) + ") }");
            w_.line("OutlinedTextField(value = " + varName(c) + ", onValueChange = { " + varName(c) + " = it }, modifier = " + modifier + ", placeholder = { Text(" + kt(propertyString(c, "placeholder")) + ") }, singleLine = true)");
        } else if (c.type == "checkbox" || c.type == "switch" || c.type == "radio") {
            w_.line("var " + varName(c) + " by remember { mutableStateOf(" + (propertyBool(c, "checked") ? "true" : "false") + ") }");
            w_.line("Row(modifier = " + modifier + ", verticalAlignment = Alignment.CenterVertically) {");
            w_.indent();
            if (c.type == "checkbox")
                w_.line("Checkbox(checked = " + varName(c) + ", onCheckedChange = { " + varName(c) + " = it })");
            else if (c.type == "switch")
                w_.line("Switch(checked = " + varName(c) + ", onCheckedChange = { " + varName(c) + " = it })");
            else
                w_.line("RadioButton(selected = " + varName(c) + ", onClick = { " + varName(c) + " = true })");
            w_.line("Text(text = " + kt(value) + ", " + fontArgs + ")");
            w_.dedent();
            w_.line("}");
        } else if (c.type == "dropdown") {
            auto options = propertyStrings(c, "options");
            std::string list;
            for (const auto& option : options)
                list += (list.empty() ? "" : ", ") + kt(option);
            w_.line("var " + varName(c) + "Open by remember { mutableStateOf(false) }");
            w_.line("var " + varName(c) + " by remember { mutableStateOf(" + kt(options.empty() ? std::string() : options.front()) + ") }");
            w_.line("Box(modifier = " + modifier + ") {");
            w_.indent();
            w_.line("OutlinedButton(onClick = { " + varName(c) + "Open = true }) { Text(" + varName(c) + ") }");
            w_.line("DropdownMenu(expanded = " + varName(c) + "Open, onDismissRequest = { " + varName(c) + "Open = false }) {");
            w_.indent();
            w_.line("listOf<String>(" + list + ").forEach { option ->");
            w_.indent();
            w_.line("DropdownMenuItem(text = { Text(option) }, onClick = { " + varName(c) + " = option; " + varName(c) + "Open = false })");
            w_.dedent();
            w_.line("}");
            w_.dedent();
            w_.line("}");
            w_.dedent();
            w_.line("}");
        } else if (c.type == "slider") {
            double lo = propertyNumber(c, "min", 0);
            double hi = propertyNumber(c, "max", 100);
            w_.line("var " + varName(c) + " by remember { mutableFloatStateOf(" + numberText(propertyNumber(c, "value", lo)) + "f) }");
            w_.line("Slider(value = " + varName(c) + ", onValueChange = { " + varName(c) + " = it }, valueRange = " + numberText(lo) + "f.." + numberText(hi) + "f, modifier = " + modifier + ")");
        } else if (c.type == "divider") {
            w_.line("HorizontalDivider(modifier = " + modifier + ", color = " + composeColor(resolveColor(model_, "$color.border")) + ")");
        } else if (c.type == "spacer") {
            w_.line("Spacer(modifier = " + modifier + ")");
        } else if (c.type == "list") {
            w_.line("Column(modifier = " + modifier + ") {");
            w_.indent();
            for (const auto& item : propertyStrings(c, "items"))
                w_.line("Text(text = " + kt(item) + ", " + fontArgs + ")");
            w_.dedent();
            w_.line("}");
        } else if (isContainerType(c)) {
            emitContainer(c, modifier);
        } else {
            w_.line("Box(modifier = " + modifier + ")");
        }
        w_.endAnchor();
    }

private:
    static std::string ktEscape(const std::string& value)
    {
        std::string quoted = kt(value);
        return quoted.substr(1, quoted.size() - 2);
    }

    std::string varName(const Component& c)
    {
        return "value" + text::toPascalCase(c.id);
    }

    std::string modifierFor(const Component& c, const Component* parent, const BoxStyle& box) const
    {
        std::string m = "Modifier";
        if (c.width.mode == SizeMode::Fixed)
            m += ".width(" + numberText(c.width.value) + ".dp)";
        if (c.height.mode == SizeMode::Fixed)
            m += ".height(" + numberText(c.height.value) + ".dp)";
        bool parentColumn = parent != nullptr && (parent->layout.kind == LayoutKind::Vertical || parent->layout.kind == LayoutKind::Scroll);
        bool parentRow = parent != nullptr && (parent->layout.kind == LayoutKind::Horizontal || parent->layout.kind == LayoutKind::Flex);
        if (c.width.mode == SizeMode::Fill)
            m += parentRow ? ".weight(1f)" : ".fillMaxWidth()";
        if (c.height.mode == SizeMode::Fill)
            m += parentColumn ? ".weight(1f)" : ".fillMaxHeight()";
        if (c.width.mode == SizeMode::Percent)
            m += ".fillMaxWidth(" + numberText(c.width.value / 100) + "f)";
        if (c.height.mode == SizeMode::Percent)
            m += ".fillMaxHeight(" + numberText(c.height.value / 100) + "f)";
        if (c.grow > 0 && (parentColumn || parentRow) && c.width.mode != SizeMode::Fill && c.height.mode != SizeMode::Fill)
            m += ".weight(" + numberText(c.grow) + "f)";
        if (parent != nullptr && (parent->layout.kind == LayoutKind::Absolute))
            m += ".offset(x = " + numberText(c.x) + ".dp, y = " + numberText(c.y) + ".dp)";
        if (parent != nullptr && parent->layout.kind == LayoutKind::Constraint)
            m += ".offset(x = " + numberText(c.pins.left.value_or(0)) + ".dp, y = " + numberText(c.pins.top.value_or(0)) + ".dp)";
        if (!c.margin.isZero())
            m += ".padding(start = " + numberText(c.margin.left) + ".dp, top = " + numberText(c.margin.top) + ".dp, end = " + numberText(c.margin.right) + ".dp, bottom = " + numberText(c.margin.bottom) + ".dp)";
        if (c.style.opacity < 1)
            m += ".alpha(" + numberText(c.style.opacity) + "f)";
        if (c.type != "button") {
            std::string background = c.type == "screen" ? rootBackground(model_, c) : box.background;
            std::string shape = box.radius > 0 ? ", RoundedCornerShape(" + numberText(box.radius) + ".dp)" : std::string();
            if (!background.empty())
                m += ".background(" + composeColor(background) + shape + ")";
            if (box.borderWidth > 0 && !box.borderColor.empty())
                m += ".border(" + numberText(box.borderWidth) + ".dp, " + composeColor(box.borderColor) + (shape.empty() ? std::string() : shape) + ")";
        }
        if (isContainerType(c) && !c.layout.padding.isZero()) {
            const EdgeInsets& p = c.layout.padding;
            m += ".padding(start = " + numberText(p.left) + ".dp, top = " + numberText(p.top) + ".dp, end = " + numberText(p.right) + ".dp, bottom = " + numberText(p.bottom) + ".dp)";
        }
        return m;
    }

    static std::string justifyText(Justify justify, double spacing, bool vertical)
    {
        const char* axis = vertical ? "Arrangement.Top" : "Arrangement.Start";
        switch (justify) {
        case Justify::Center:
            return "Arrangement.Center";
        case Justify::End:
            return vertical ? "Arrangement.Bottom" : "Arrangement.End";
        case Justify::SpaceBetween:
            return "Arrangement.SpaceBetween";
        case Justify::SpaceAround:
            return "Arrangement.SpaceAround";
        case Justify::SpaceEvenly:
            return "Arrangement.SpaceEvenly";
        case Justify::Start:
            break;
        }
        if (spacing > 0)
            return "Arrangement.spacedBy(" + numberText(spacing) + ".dp)";
        return axis;
    }

    void emitContainer(const Component& c, const std::string& modifier)
    {
        const LayoutSpec& l = c.layout;
        std::string open;
        switch (l.kind) {
        case LayoutKind::Vertical:
        case LayoutKind::Scroll: {
            std::string scroll = l.kind == LayoutKind::Scroll ? ".verticalScroll(rememberScrollState())" : "";
            const char* align = l.align == Align::Center ? "Alignment.CenterHorizontally" : l.align == Align::End ? "Alignment.End" : "Alignment.Start";
            open = "Column(modifier = " + modifier + scroll + ", verticalArrangement = " + justifyText(l.justify, l.spacing, true) + ", horizontalAlignment = " + align + ")";
            break;
        }
        case LayoutKind::Horizontal: {
            const char* align = l.align == Align::Center ? "Alignment.CenterVertically" : l.align == Align::End ? "Alignment.Bottom" : "Alignment.Top";
            open = "Row(modifier = " + modifier + ", horizontalArrangement = " + justifyText(l.justify, l.spacing, false) + ", verticalAlignment = " + align + ")";
            break;
        }
        case LayoutKind::Flex:
            open = "FlowRow(modifier = " + modifier + ", horizontalArrangement = " + justifyText(l.justify, l.spacing, false) + ")";
            break;
        case LayoutKind::Grid: {
            std::size_t columns = std::max<std::size_t>(l.columns.size(), 1);
            open = "FlowRow(modifier = " + modifier + ", maxItemsInEachRow = " + std::to_string(columns) + ", horizontalArrangement = " + justifyText(Justify::Start, l.spacing, false) + ")";
            break;
        }
        case LayoutKind::Absolute:
        case LayoutKind::Constraint:
            open = "Box(modifier = " + modifier + ")";
            break;
        }
        w_.line(open + " {");
        w_.indent();
        for (const auto& child : c.children)
            emit(child, &c);
        w_.dedent();
        w_.line("}");
    }

    const ApplicationModel& model_;
    CodeWriter& w_;
    std::string screen_;
};

class ComposeGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "compose-kotlin";
        value.label = "Jetpack Compose (Kotlin)";
        value.language = "Kotlin";
        value.framework = "Jetpack Compose";
        value.description = "An Android app in Kotlin with Jetpack Compose, built with Gradle.";
        value.platforms = {"android"};
        value.requiredTools = {"jdk", "android-sdk", "gradle"};
        value.buildSystem = "gradle";
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

    std::vector<Diagnostic> validate(const ApplicationModel& model) const override { return checkCapabilities(model, capabilities(), "Jetpack Compose"); }

    GenerationResult generate(const ApplicationModel& model, const ProjectConfiguration& config) const override
    {
        GenerationResult result;
        std::string name = config.name.empty() ? model.meta.name : config.name;
        std::string package = packageName(config.bundleId.empty() ? model.meta.bundleId : config.bundleId);
        if (package.empty())
            package = "com.example." + projectSlug(name);
        std::string folder = text::replaceAll(package, ".", "/");
        result.files.push_back(plainFile("settings.gradle.kts", "pluginManagement {\n    repositories {\n        google()\n        mavenCentral()\n        gradlePluginPortal()\n    }\n}\n\ndependencyResolutionManagement {\n    repositories {\n        google()\n        mavenCentral()\n    }\n}\n\nrootProject.name = " + kt(projectClass(name)) + "\ninclude(\":app\")\n"));
        result.files.push_back(plainFile("build.gradle.kts", "plugins {\n    id(\"com.android.application\") version \"9.5.0\" apply false\n    id(\"org.jetbrains.kotlin.plugin.compose\") version \"2.4.21\" apply false\n}\n"));
        result.files.push_back(plainFile("gradle.properties", "android.useAndroidX=true\norg.gradle.jvmargs=-Xmx2g\n"));
        result.files.push_back(plainFile("app/build.gradle.kts", "plugins {\n    id(\"com.android.application\")\n    id(\"org.jetbrains.kotlin.plugin.compose\")\n}\n\nandroid {\n    namespace = " + kt(package) + "\n    compileSdk = 37\n\n    defaultConfig {\n        applicationId = " + kt(package) + "\n        minSdk = 26\n        targetSdk = 37\n        versionCode = 1\n        versionName = " + kt(model.meta.version.empty() ? "0.1.0" : model.meta.version) + "\n    }\n\n    buildFeatures {\n        compose = true\n    }\n}\n\ndependencies {\n    implementation(platform(\"androidx.compose:compose-bom:2026.09.00\"))\n    implementation(\"androidx.compose.material3:material3\")\n    implementation(\"androidx.compose.foundation:foundation\")\n    implementation(\"androidx.activity:activity-compose:1.13.0\")\n}\n"));
        result.files.push_back(plainFile("app/src/main/AndroidManifest.xml", "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">\n    <application android:label=\"" + escapeLiteral(name, LiteralStyle::Xml) + "\" android:theme=\"@android:style/Theme.Material.Light.NoActionBar\">\n        <activity android:name=\".MainActivity\" android:exported=\"true\">\n            <intent-filter>\n                <action android:name=\"android.intent.action.MAIN\"/>\n                <category android:name=\"android.intent.category.LAUNCHER\"/>\n            </intent-filter>\n        </activity>\n    </application>\n</manifest>\n"));
        std::string start = startScreenId(model);
        if (start.empty() && !model.screens.empty())
            start = model.screens.front().root.id;
        auto screens = screenNames(model);
        CodeWriter main;
        main.line("package " + package);
        main.line();
        main.line("import android.content.Intent");
        main.line("import android.net.Uri");
        main.line("import android.os.Bundle");
        main.line("import androidx.activity.ComponentActivity");
        main.line("import androidx.activity.compose.BackHandler");
        main.line("import androidx.activity.compose.setContent");
        main.line("import androidx.compose.runtime.Composable");
        main.line("import androidx.compose.runtime.mutableStateListOf");
        main.line("import androidx.compose.runtime.remember");
        main.line();
        main.line("class AppApi(private val stack: MutableList<String>, private val activity: ComponentActivity) {");
        main.indent();
        main.line("fun navigate(screen: String) { stack.add(screen) }");
        main.line("fun back() { if (stack.size > 1) stack.removeAt(stack.lastIndex) }");
        main.line("fun openUrl(url: String) { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }");
        main.dedent();
        main.line("}");
        main.line();
        main.line("class MainActivity : ComponentActivity() {");
        main.indent();
        main.line("override fun onCreate(savedInstanceState: Bundle?) {");
        main.indent();
        main.line("super.onCreate(savedInstanceState)");
        main.line("setContent {");
        main.indent();
        main.line("val stack = remember { mutableStateListOf(" + kt(start) + ") }");
        main.line("val app = remember { AppApi(stack, this) }");
        main.line("BackHandler(enabled = stack.size > 1) { app.back() }");
        main.line("when (stack.last()) {");
        main.indent();
        for (const auto& item : screens)
            main.line(kt(item.screen->root.id) + " -> " + item.className + "(app)");
        main.dedent();
        main.line("}");
        main.dedent();
        main.line("}");
        main.dedent();
        main.line("}");
        main.dedent();
        main.line("}");
        result.files.push_back(main.finish("app/src/main/java/" + folder + "/MainActivity.kt"));
        for (const auto& item : screens) {
            CodeWriter w;
            w.line("@file:OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)");
            w.line();
            w.line("package " + package);
            w.line();
            for (const char* import : {"androidx.compose.foundation.BorderStroke", "androidx.compose.foundation.background", "androidx.compose.foundation.border", "androidx.compose.foundation.layout.*", "androidx.compose.foundation.rememberScrollState", "androidx.compose.foundation.shape.RoundedCornerShape", "androidx.compose.foundation.verticalScroll", "androidx.compose.material3.*", "androidx.compose.runtime.*", "androidx.compose.ui.Alignment", "androidx.compose.ui.Modifier", "androidx.compose.ui.draw.alpha", "androidx.compose.ui.graphics.Color", "androidx.compose.ui.text.font.FontWeight", "androidx.compose.ui.unit.dp", "androidx.compose.ui.unit.sp"})
                w.line(std::string("import ") + import);
            w.line();
            w.line("@Composable");
            w.line("fun " + item.className + "(app: AppApi) {");
            w.indent();
            Component root = item.screen->root;
            root.width.mode = SizeMode::Fill;
            root.height.mode = SizeMode::Fill;
            w.line("Box(modifier = Modifier.fillMaxSize().systemBarsPadding()) {");
            w.indent();
            ComposeEmitter(model, w, item.screen->root.id).emit(root, nullptr);
            w.dedent();
            w.line("}");
            w.dedent();
            w.line("}");
            result.files.push_back(w.finish("app/src/main/java/" + folder + "/" + item.className + ".kt"));
        }
        CodeWriter user;
        user.line("package " + package);
        user.line();
        user.line("// Your code. FormaIDE won't overwrite this file.");
        user.line("object Handlers {");
        user.indent();
        for (const auto& handler : collectHandlers(model))
            user.line("fun " + handler + "(app: AppApi) {}");
        user.dedent();
        user.line("}");
        result.files.push_back(user.finish("app/src/main/java/" + folder + "/Handlers.kt", FileCategory::User));
        result.files.push_back(plainFile(".gitignore", ".gradle/\nbuild/\nlocal.properties\n"));
        result.files.push_back(standardReadme(model, config, "Jetpack Compose", "Build it from FormaIDE, or run `gradlew assembleDebug`.", "Handlers.kt"));
        return result;
    }
};

}

std::unique_ptr<ICodeGenerator> createComposeGenerator()
{
    return std::make_unique<ComposeGenerator>();
}

}
