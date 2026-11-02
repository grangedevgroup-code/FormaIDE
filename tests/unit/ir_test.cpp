#include <gtest/gtest.h>

#include "forma/core/filesystem_service.h"
#include "forma/ir/catalog.h"
#include "forma/ir/editor.h"
#include "forma/ir/model.h"
#include "forma/ir/serialization.h"
#include "forma/ir/validation.h"
#include "test_support.h"

using namespace forma;
using namespace forma::ir;
using forma::testing::TempDir;

namespace {

ApplicationModel sampleModel()
{
    ApplicationModel model = makeEmptyApplication("Sample", "com.example.sample");
    Screen home = makeScreen("screen_home", "Home", 390, 844);
    Component title = builtinCatalog().instantiate("text", "title");
    title.properties["content"] = "Welcome";
    title.typography.size = 28;
    title.typography.weight = 700;
    home.root.children.push_back(title);
    Component button = builtinCatalog().instantiate("button", "continue_button");
    button.properties["label"] = "Continue";
    button.events["onClick"] = "navigate_settings";
    home.root.children.push_back(button);
    model.screens.push_back(home);
    model.screens.push_back(makeScreen("screen_settings", "Settings", 390, 844));
    model.navigation.start = "screen_home";
    Action action;
    action.id = "navigate_settings";
    action.kind = "navigate";
    action.target = "screen_settings";
    model.actions.push_back(action);
    return model;
}

}

TEST(Catalog, HasEveryComponentTheProductPromises)
{
    for (const char* type : {"container", "frame", "group", "text", "image", "icon", "shape", "divider", "spacer", "vstack", "hstack", "grid",
                             "scroll", "flex", "absolute", "constraint", "button", "textinput", "checkbox", "radio", "dropdown", "slider",
                             "switch", "tabs", "navbar", "toolbar", "dialog", "modal", "card", "list", "table", "chart", "map", "media",
                             "datatable", "custom", "native"}) {
        EXPECT_NE(builtinCatalog().find(type), nullptr) << type;
    }
}

TEST(Catalog, InstantiateAppliesDefaults)
{
    Component stack = builtinCatalog().instantiate("hstack", "row");
    EXPECT_EQ(stack.layout.kind, LayoutKind::Horizontal);
    EXPECT_EQ(stack.layout.spacing, 8);
    EXPECT_EQ(stack.layout.align, Align::Center);
    EXPECT_EQ(stack.width.mode, SizeMode::Fill);
    Component button = builtinCatalog().instantiate("button", "b");
    EXPECT_EQ(propertyString(button, "label"), "Button");
    EXPECT_EQ(propertyString(button, "variant"), "primary");
}

TEST(Serialization, ComponentRoundTrip)
{
    Component component = builtinCatalog().instantiate("card", "card_1");
    component.width = SizeSpec{SizeMode::Percent, 50, 100.0, 400.0};
    component.margin = EdgeInsets{1, 2, 3, 4};
    component.pins.left = 8;
    component.pins.centerY = 0;
    component.style.shadow = Shadow{0, 2, 8, 0, "#00000040"};
    component.typography.ref = "body";
    component.events["onClick"] = "go";
    component.bindings["properties.label"] = Binding{"user.name", "twoWay"};
    component.tokens["style.radius"] = "radius.md";
    component.variants["wide"] = Json{{"width", 400}};
    component.platform["ios"] = Json{{"style.radius", 12}};
    component.children.push_back(builtinCatalog().instantiate("text", "child"));

    Json json = componentToJson(component);
    auto parsed = componentFromJson(json);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    EXPECT_EQ(*parsed, component);
}

TEST(Serialization, PreservesUnknownFields)
{
    auto parsed = componentFromJson(Json{{"id", "a"}, {"type", "text"}, {"futureField", {{"x", 1}}}});
    ASSERT_TRUE(parsed.ok());
    Json output = componentToJson(*parsed);
    ASSERT_TRUE(output.contains("futureField"));
    EXPECT_EQ(output["futureField"]["x"], 1);
}

TEST(Serialization, RejectsWrongTypesWithAPath)
{
    auto parsed = componentFromJson(Json{{"id", "a"}, {"type", "text"}, {"x", "not a number"}});
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.error().message.find("a.x"), std::string::npos);

    auto layout = componentFromJson(Json{{"id", "a"}, {"type", "text"}, {"layout", {{"type", "spiral"}}}});
    ASSERT_FALSE(layout.ok());
    EXPECT_NE(layout.error().message.find("vertical"), std::string::npos);
}

TEST(Serialization, ReadsTheShorthandFromTheSpecExample)
{
    auto parsed = screenFromJson(Json::parse(R"({
      "id": "screen_home", "type": "screen", "name": "Home",
      "layout": {"type": "vertical", "spacing": 16, "padding": 24},
      "children": [
        {"id": "title", "type": "text", "properties": {"content": "Welcome", "fontSize": 28}},
        {"id": "continue_button", "type": "button", "properties": {"label": "Continue"}, "events": {"onClick": "navigate_settings"}}
      ]
    })"));
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    EXPECT_EQ(parsed->root.layout.kind, LayoutKind::Vertical);
    EXPECT_EQ(parsed->root.layout.padding.left, 24);
    ASSERT_EQ(parsed->root.children.size(), 2u);
    EXPECT_EQ(parsed->root.children[1].events["onClick"], "navigate_settings");
}

TEST(Serialization, SaveAndLoadThroughFiles)
{
    TempDir dir;
    auto filesystem = createLocalFilesystemService(dir.path());
    ApplicationModel model = sampleModel();
    ASSERT_TRUE(saveApplication(*filesystem, model).ok());
    EXPECT_TRUE(filesystem->exists("design/application.ir"));
    EXPECT_TRUE(filesystem->exists("design/screens/screen_home.json"));
    EXPECT_TRUE(filesystem->exists("design/themes/light.json"));
    auto loaded = loadApplication(*filesystem);
    ASSERT_TRUE(loaded.ok()) << loaded.error().message;
    EXPECT_EQ(*loaded, model);

    model.screens.pop_back();
    model.actions.clear();
    model.screens[0].root.children[1].events.clear();
    ASSERT_TRUE(saveApplication(*filesystem, model).ok());
    EXPECT_FALSE(filesystem->exists("design/screens/screen_settings.json"));
}

TEST(Serialization, RefusesNewerFormatVersions)
{
    Json json = applicationToJson(sampleModel());
    json["formaIr"] = kFormatVersion + 1;
    auto parsed = applicationFromJson(json);
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.error().code, "ir.version");
}

TEST(Validation, SampleIsClean)
{
    auto diagnostics = validateModel(sampleModel());
    EXPECT_FALSE(hasErrors(diagnostics));
}

TEST(Validation, CatchesBrokenReferences)
{
    ApplicationModel model = sampleModel();
    model.screens[0].root.children[1].events["onClick"] = "missing_action";
    model.navigation.start = "nowhere";
    Component duplicate = builtinCatalog().instantiate("text", "title");
    model.screens[0].root.children.push_back(duplicate);
    model.screens[0].root.children[0].style.background = "red";
    model.screens[0].root.children[0].style.opacity = 2;
    Component badChild = builtinCatalog().instantiate("button", "b2");
    badChild.children.push_back(builtinCatalog().instantiate("text", "inner"));
    model.screens[0].root.children.push_back(badChild);
    auto diagnostics = validateModel(model);
    auto has = [&](const char* code) {
        return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) { return d.code == code; });
    };
    EXPECT_TRUE(has("event.action"));
    EXPECT_TRUE(has("navigation.start"));
    EXPECT_TRUE(has("component.duplicate"));
    EXPECT_TRUE(has("style.color"));
    EXPECT_TRUE(has("style.opacity"));
    EXPECT_TRUE(has("component.children"));
    EXPECT_TRUE(hasErrors(diagnostics));
}

TEST(Validation, RequiresRequiredProperties)
{
    ApplicationModel model = sampleModel();
    model.screens[0].root.children[1].properties.erase("label");
    auto diagnostics = validateModel(model);
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.code == "property.required"; }));
}

TEST(Editor, SetFieldUndoRedo)
{
    ModelEditor editor(sampleModel());
    ASSERT_TRUE(editor.setField("screen_home", "title", "properties.content", "Hello").ok());
    const auto* title = findComponent(editor.model().findScreen("screen_home")->root, "title");
    EXPECT_EQ(propertyString(*title, "content"), "Hello");
    EXPECT_TRUE(editor.canUndo());
    ASSERT_TRUE(editor.undo().ok());
    title = findComponent(editor.model().findScreen("screen_home")->root, "title");
    EXPECT_EQ(propertyString(*title, "content"), "Welcome");
    ASSERT_TRUE(editor.redo().ok());
    title = findComponent(editor.model().findScreen("screen_home")->root, "title");
    EXPECT_EQ(propertyString(*title, "content"), "Hello");
}

TEST(Editor, SetFieldRejectsInvalidValuesWithoutChangingAnything)
{
    ModelEditor editor(sampleModel());
    auto before = editor.model();
    EXPECT_FALSE(editor.setField("screen_home", "title", "layout.type", "spiral").ok());
    EXPECT_FALSE(editor.setField("screen_home", "title", "style.radius", "big").ok());
    EXPECT_FALSE(editor.setField("screen_home", "nobody", "x", 1).ok());
    EXPECT_FALSE(editor.setField("screen_home", "title", "id", "renamed").ok());
    EXPECT_EQ(editor.model(), before);
    EXPECT_FALSE(editor.canUndo());
}

TEST(Editor, InsertMoveRemoveUndoAllTheWayBack)
{
    ModelEditor editor(sampleModel());
    auto original = editor.model();
    auto stack = editor.insertComponent("screen_home", "screen_home", 0, builtinCatalog().instantiate("hstack", "row"));
    ASSERT_TRUE(stack.ok());
    EXPECT_EQ(*stack, "row");
    ASSERT_TRUE(editor.moveComponent("screen_home", "title", "row", 0).ok());
    ASSERT_TRUE(editor.removeComponent("screen_home", "continue_button").ok());
    const auto& root = editor.model().findScreen("screen_home")->root;
    ASSERT_EQ(root.children.size(), 1u);
    EXPECT_EQ(root.children[0].children[0].id, "title");
    ASSERT_TRUE(editor.undo().ok());
    ASSERT_TRUE(editor.undo().ok());
    ASSERT_TRUE(editor.undo().ok());
    EXPECT_EQ(editor.model(), original);
    ASSERT_TRUE(editor.redo().ok());
    ASSERT_TRUE(editor.redo().ok());
    ASSERT_TRUE(editor.redo().ok());
    EXPECT_EQ(editor.model().findScreen("screen_home")->root.children.size(), 1u);
}

TEST(Editor, RefusesMovingIntoItselfAndDeletingTheRoot)
{
    ModelEditor editor(sampleModel());
    auto outer = editor.insertComponent("screen_home", "screen_home", -1, builtinCatalog().instantiate("vstack", "outer"));
    ASSERT_TRUE(outer.ok());
    auto inner = editor.insertComponent("screen_home", "outer", -1, builtinCatalog().instantiate("vstack", "inner"));
    ASSERT_TRUE(inner.ok());
    EXPECT_FALSE(editor.moveComponent("screen_home", "outer", "inner", 0).ok());
    EXPECT_FALSE(editor.moveComponent("screen_home", "outer", "outer", 0).ok());
    EXPECT_FALSE(editor.removeComponent("screen_home", "screen_home").ok());
}

TEST(Editor, InsertRenamesCollidingIds)
{
    ModelEditor editor(sampleModel());
    auto copy = builtinCatalog().instantiate("text", "title");
    auto id = editor.insertComponent("screen_home", "screen_home", -1, copy);
    ASSERT_TRUE(id.ok());
    EXPECT_NE(*id, "title");
    EXPECT_NE(findComponent(editor.model().findScreen("screen_home")->root, *id), nullptr);
}

TEST(Editor, TransactionsUndoAsOneStep)
{
    ModelEditor editor(sampleModel());
    auto original = editor.model();
    {
        TransactionScope scope(editor, "Align");
        ASSERT_TRUE(editor.setField("screen_home", "title", "x", 10).ok());
        ASSERT_TRUE(editor.setField("screen_home", "title", "y", 20).ok());
    }
    EXPECT_EQ(editor.undoLabel(), "Align");
    ASSERT_TRUE(editor.undo().ok());
    EXPECT_EQ(editor.model(), original);
    EXPECT_FALSE(editor.canUndo());
}

TEST(Editor, RollbackRestoresTheModel)
{
    ModelEditor editor(sampleModel());
    auto original = editor.model();
    editor.beginTransaction("Failed work");
    ASSERT_TRUE(editor.setField("screen_home", "title", "x", 10).ok());
    ASSERT_TRUE(editor.removeComponent("screen_home", "continue_button").ok());
    editor.rollbackTransaction();
    EXPECT_EQ(editor.model(), original);
    EXPECT_FALSE(editor.canUndo());
}

TEST(Editor, ScreensAndSectionsUndo)
{
    ModelEditor editor(sampleModel());
    auto original = editor.model();
    auto id = editor.addScreen(makeScreen("screen_extra", "Extra", 390, 844));
    ASSERT_TRUE(id.ok());
    ASSERT_TRUE(editor.removeScreen("screen_settings").ok());
    ASSERT_TRUE(editor.setSection("navigation", Json{{"kind", "tabs"}, {"start", "screen_home"}, {"routes", Json::array()}}).ok());
    EXPECT_EQ(editor.model().navigation.kind, "tabs");
    ASSERT_TRUE(editor.undo().ok());
    ASSERT_TRUE(editor.undo().ok());
    ASSERT_TRUE(editor.undo().ok());
    EXPECT_EQ(editor.model(), original);
}

TEST(Editor, ListenersSeeEveryChange)
{
    ModelEditor editor(sampleModel());
    std::vector<ChangeKind> kinds;
    int id = editor.addListener([&](const ChangeEvent& event) { kinds.push_back(event.kind); });
    ASSERT_TRUE(editor.setField("screen_home", "title", "x", 5).ok());
    ASSERT_TRUE(editor.undo().ok());
    ASSERT_EQ(kinds.size(), 2u);
    EXPECT_EQ(kinds[0], ChangeKind::ComponentChanged);
    editor.removeListener(id);
    ASSERT_TRUE(editor.setField("screen_home", "title", "x", 6).ok());
    EXPECT_EQ(kinds.size(), 2u);
}

TEST(Editor, DirtyTracking)
{
    ModelEditor editor(sampleModel());
    EXPECT_FALSE(editor.isDirty());
    ASSERT_TRUE(editor.setField("screen_home", "title", "x", 5).ok());
    EXPECT_TRUE(editor.isDirty());
    editor.markClean();
    EXPECT_FALSE(editor.isDirty());
}
