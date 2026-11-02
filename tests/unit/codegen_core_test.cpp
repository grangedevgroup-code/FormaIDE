#include <gtest/gtest.h>

#include "forma/codegen/code_writer.h"
#include "forma/codegen/pipeline.h"
#include "forma/codegen/template_engine.h"
#include "forma/codegen/text_diff.h"
#include "forma/core/filesystem_service.h"
#include "forma/ir/catalog.h"
#include "test_support.h"

using namespace forma;
using namespace forma::codegen;
using forma::testing::TempDir;

TEST(TextDiff, FindsMinimalHunks)
{
    std::vector<std::string> a = {"one", "two", "three", "four"};
    std::vector<std::string> b = {"one", "2", "three", "four", "five"};
    auto hunks = diffLines(a, b);
    ASSERT_EQ(hunks.size(), 2u);
    EXPECT_EQ(hunks[0].baseStart, 1);
    EXPECT_EQ(hunks[0].baseCount, 1);
    EXPECT_EQ(hunks[0].otherCount, 1);
    EXPECT_EQ(hunks[1].baseStart, 4);
    EXPECT_EQ(hunks[1].otherCount, 1);
    EXPECT_TRUE(diffLines(a, a).empty());
}

TEST(TextDiff, HandlesEmptySides)
{
    std::vector<std::string> a;
    std::vector<std::string> b = {"x", "y"};
    auto insert = diffLines(a, b);
    ASSERT_EQ(insert.size(), 1u);
    EXPECT_EQ(insert[0].otherCount, 2);
    auto remove = diffLines(b, a);
    ASSERT_EQ(remove.size(), 1u);
    EXPECT_EQ(remove[0].baseCount, 2);
}

TEST(Merge, CombinesEditsThatDoNotOverlap)
{
    std::string base = "a\nb\nc\nd\ne\n";
    std::string mine = "a\nB\nc\nd\ne\n";
    std::string theirs = "a\nb\nc\nd\nE\n";
    auto merged = mergeThreeWay(base, mine, theirs);
    EXPECT_TRUE(merged.clean());
    EXPECT_EQ(merged.text, "a\nB\nc\nd\nE\n");
}

TEST(Merge, IdenticalChangesAreNotConflicts)
{
    auto merged = mergeThreeWay("a\nb\nc\n", "a\nX\nc\n", "a\nX\nc\n");
    EXPECT_TRUE(merged.clean());
    EXPECT_EQ(merged.text, "a\nX\nc\n");
}

TEST(Merge, OverlappingEditsConflictWithMarkers)
{
    auto merged = mergeThreeWay("a\nb\nc\n", "a\nmine\nc\n", "a\ntheirs\nc\n");
    EXPECT_EQ(merged.conflicts, 1);
    EXPECT_NE(merged.text.find("<<<<<<< your edits"), std::string::npos);
    EXPECT_NE(merged.text.find("mine"), std::string::npos);
    EXPECT_NE(merged.text.find("theirs"), std::string::npos);
    EXPECT_NE(merged.text.find(">>>>>>> generated"), std::string::npos);
}

TEST(Merge, KeepsUserAddedLinesWhenTheGeneratorChangesOtherLines)
{
    std::string base = "void f()\n{\n    a();\n    b();\n}\n";
    std::string mine = "void f()\n{\n    a();\n    myCustomCall();\n    b();\n}\n";
    std::string theirs = "void f()\n{\n    a2();\n    b();\n    c();\n}\n";
    auto merged = mergeThreeWay(base, mine, theirs);
    EXPECT_TRUE(merged.clean());
    EXPECT_EQ(merged.text, "void f()\n{\n    a2();\n    myCustomCall();\n    b();\n    c();\n}\n");
}

TEST(Merge, InsertionsAtTheSamePlaceConflict)
{
    auto merged = mergeThreeWay("a\nb\n", "a\nmine\nb\n", "a\ntheirs\nb\n");
    EXPECT_EQ(merged.conflicts, 1);
}

TEST(Merge, EverySideUnchanged)
{
    auto merged = mergeThreeWay("a\nb\n", "a\nb\n", "a\nb\n");
    EXPECT_TRUE(merged.clean());
    EXPECT_EQ(merged.text, "a\nb\n");
}

TEST(UnifiedDiff, ShowsContext)
{
    std::string diff = unifiedDiff("a\nb\nc\nd\n", "a\nB\nc\nd\n", "before", "after", 1);
    EXPECT_NE(diff.find("--- before"), std::string::npos);
    EXPECT_NE(diff.find("-b"), std::string::npos);
    EXPECT_NE(diff.find("+B"), std::string::npos);
    EXPECT_EQ(unifiedDiff("same\n", "same\n", "x", "y"), "");
}

TEST(TemplateEngine, RendersValuesConditionsAndLoops)
{
    Json context = {{"name", "Forma"}, {"on", true}, {"off", false}, {"items", {{{"id", "a"}}, {{"id", "b"}}}}, {"plain", {"x", "y"}}, {"nested", {{"value", 7}}}};
    auto rendered = renderTemplate("Hi {{name}} {{nested.value}}\n{{#if on}}\nyes\n{{/if}}\n{{#if off}}\nno\n{{/if}}\n{{#unless off}}\nnot off\n{{/unless}}\n{{#each items}}\n- {{id}}\n{{/each}}\n{{#each plain}}{{this}},{{/each}}\n", context);
    ASSERT_TRUE(rendered.ok()) << rendered.error().message;
    EXPECT_EQ(*rendered, "Hi Forma 7\nyes\nnot off\n- a\n- b\nx,y,\n");
}

TEST(TemplateEngine, ReportsUnbalancedTags)
{
    EXPECT_FALSE(renderTemplate("{{#if a}}never closed", Json::object()).ok());
    EXPECT_FALSE(renderTemplate("{{/if}}", Json::object()).ok());
    EXPECT_FALSE(renderTemplate("{{ open", Json::object()).ok());
}

TEST(Literals, EscapeAndUnescapeAreInverse)
{
    std::string value = "He said \"hi\"\n\tC:\\path";
    for (auto style : {LiteralStyle::CLike, LiteralStyle::Python}) {
        auto decoded = unescapeLiteral(escapeLiteral(value, style), style);
        ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(*decoded, value);
    }
    std::string markup = "a < b & \"c\" > d";
    for (auto style : {LiteralStyle::Xml, LiteralStyle::Html}) {
        auto decoded = unescapeLiteral(escapeLiteral(markup, style), style);
        ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(*decoded, markup);
    }
    EXPECT_FALSE(unescapeLiteral("two\" + \"parts", LiteralStyle::CLike).ok());
    EXPECT_FALSE(unescapeLiteral("<b>bold</b>", LiteralStyle::Html).ok());
}

TEST(CodeWriter, TracksBindingsAndAnchors)
{
    CodeWriter writer;
    writer.line("first");
    writer.beginAnchor("home", "title");
    writer.indent();
    writer.bound("title->setText(u\"", "Hello", "\"_s);", {"home", "title", "properties.content"});
    writer.dedent();
    writer.endAnchor();
    GeneratedFile file = writer.finish("a.cpp");
    EXPECT_EQ(file.content, "first\n    title->setText(u\"Hello\"_s);\n");
    ASSERT_EQ(file.bindings.size(), 1u);
    EXPECT_EQ(file.bindings[0].line, 2);
    ASSERT_EQ(file.anchors.size(), 1u);
    EXPECT_EQ(file.anchors[0].startLine, 2);
    EXPECT_EQ(file.anchors[0].endLine, 2);
}

namespace {

class TestGenerator final : public ICodeGenerator {
public:
    GeneratorInfo info() const override
    {
        GeneratorInfo value;
        value.id = "test";
        value.label = "Test";
        return value;
    }
    GeneratorCapabilities capabilities() const override { return {}; }
    std::vector<ir::Diagnostic> validate(const ir::ApplicationModel&) const override { return {}; }
    GenerationResult generate(const ir::ApplicationModel& model, const ProjectConfiguration&) const override
    {
        GenerationResult result;
        for (const auto& screen : model.screens) {
            CodeWriter writer;
            writer.line("// generated header");
            writer.line("Screen " + screen.root.id);
            for (const auto& child : screen.root.children) {
                writer.beginAnchor(screen.root.id, child.id);
                writer.line("begin " + child.id);
                writer.bound("text = \"", escapeLiteral(ir::propertyString(child, "content"), LiteralStyle::CLike), "\";", {screen.root.id, child.id, "properties.content"});
                writer.bound("size = ", text::formatNumber(child.typography.size.value_or(15)), ";", {screen.root.id, child.id, "typography.size", ValueKind::Number});
                writer.line("end " + child.id);
                writer.endAnchor();
            }
            writer.line("// footer");
            result.files.push_back(writer.finish("src/" + screen.root.id + ".gen"));
        }
        CodeWriter user;
        user.line("user seed");
        result.files.push_back(user.finish("src/user.txt", FileCategory::User));
        CodeWriter generatedOnce;
        generatedOnce.line("build " + model.meta.name);
        result.files.push_back(generatedOnce.finish("build.cfg", FileCategory::Protected));
        return result;
    }
};

ir::ApplicationModel makeModel()
{
    ir::ApplicationModel model = ir::makeEmptyApplication("Sample", "com.example.sample");
    ir::Screen screen = ir::makeScreen("home", "Home", 390, 844);
    ir::Component title = ir::builtinCatalog().instantiate("text", "title");
    title.properties["content"] = "Welcome";
    title.typography.size = 28;
    screen.root.children.push_back(title);
    ir::Component body = ir::builtinCatalog().instantiate("text", "body");
    body.properties["content"] = "Body text";
    screen.root.children.push_back(body);
    model.screens.push_back(screen);
    model.navigation.start = "home";
    return model;
}

const FileReport* findReport(const GenerationReport& report, const std::string& path)
{
    for (const auto& file : report.files) {
        if (file.path == path)
            return &file;
    }
    return nullptr;
}

struct Fixture {
    TempDir dir;
    std::unique_ptr<IFilesystemService> filesystem = createLocalFilesystemService(dir.path());
    TestGenerator generator;
    ProjectConfiguration config;
    GenerationOptions options;
    ir::ApplicationModel model = makeModel();

    GenerationReport generate() { return runGeneration(*filesystem, generator, model, config, options); }
};

}

TEST(Pipeline, FirstRunCreatesFilesAndManifest)
{
    Fixture f;
    auto report = f.generate();
    EXPECT_FALSE(report.blockedByErrors);
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Created);
    EXPECT_EQ(findReport(report, "src/user.txt")->outcome, FileOutcome::Created);
    EXPECT_TRUE(f.filesystem->exists("src/home.gen"));
    EXPECT_TRUE(f.filesystem->exists(".forma/generated/main/manifest.json"));
    EXPECT_TRUE(f.filesystem->exists(".forma/generated/main/base/src/home.gen"));
}

TEST(Pipeline, SecondRunChangesNothing)
{
    Fixture f;
    f.generate();
    auto report = f.generate();
    EXPECT_FALSE(report.filesChanged());
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Unchanged);
}

TEST(Pipeline, UpdatesUneditedFilesWhenTheDesignChanges)
{
    Fixture f;
    f.generate();
    f.model.screens[0].root.children[0].properties["content"] = "Changed";
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Updated);
    EXPECT_NE(f.filesystem->readFile("src/home.gen")->find("Changed"), std::string::npos);
}

TEST(Pipeline, NeverTouchesUserFiles)
{
    Fixture f;
    f.generate();
    ASSERT_TRUE(f.filesystem->writeFile("src/user.txt", "my own code\n").ok());
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/user.txt")->outcome, FileOutcome::KeptUserFile);
    EXPECT_EQ(*f.filesystem->readFile("src/user.txt"), "my own code\n");
}

TEST(Pipeline, DeletedUserFilesStayDeleted)
{
    Fixture f;
    f.generate();
    ASSERT_TRUE(f.filesystem->remove("src/user.txt").ok());
    f.generate();
    EXPECT_FALSE(f.filesystem->exists("src/user.txt"));
}

TEST(Pipeline, MergesHandEditsWithDesignChanges)
{
    Fixture f;
    f.generate();
    std::string edited = *f.filesystem->readFile("src/home.gen");
    edited = text::replaceAll(edited, "// footer", "// my note\n// footer");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());

    f.model.screens[0].root.children[0].properties["content"] = "Changed";
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Merged);
    std::string merged = *f.filesystem->readFile("src/home.gen");
    EXPECT_NE(merged.find("// my note"), std::string::npos);
    EXPECT_NE(merged.find("Changed"), std::string::npos);
}

TEST(Pipeline, OverlappingEditsAreReportedAndNotOverwritten)
{
    Fixture f;
    f.generate();
    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "text = \"Welcome\";", "text = compute();");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());
    f.model.screens[0].root.children[0].properties["content"] = "Changed";
    auto report = f.generate();
    EXPECT_TRUE(report.hasConflicts());
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Conflict);
    EXPECT_EQ(*f.filesystem->readFile("src/home.gen"), edited);
    EXPECT_FALSE(findReport(report, "src/home.gen")->mergedPreview.empty());
}

TEST(Pipeline, ForeignFilesAreNeverOverwritten)
{
    Fixture f;
    ASSERT_TRUE(f.filesystem->writeFile("build.cfg", "hand written\n").ok());
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "build.cfg")->outcome, FileOutcome::Conflict);
    EXPECT_EQ(*f.filesystem->readFile("build.cfg"), "hand written\n");
}

TEST(Pipeline, ResolveConflictChoices)
{
    Fixture f;
    f.generate();
    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "text = \"Welcome\";", "text = compute();");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());
    f.model.screens[0].root.children[0].properties["content"] = "Changed";
    f.generate();

    ASSERT_TRUE(resolveConflict(*f.filesystem, f.generator, f.model, f.config, f.options, "src/home.gen", ConflictChoice::KeepMine).ok());
    EXPECT_EQ(*f.filesystem->readFile("src/home.gen"), edited);
    auto again = f.generate();
    EXPECT_FALSE(again.hasConflicts());
    EXPECT_NE(f.filesystem->readFile("src/home.gen")->find("compute()"), std::string::npos);

    ASSERT_TRUE(resolveConflict(*f.filesystem, f.generator, f.model, f.config, f.options, "src/home.gen", ConflictChoice::TakeGenerated).ok());
    EXPECT_EQ(f.filesystem->readFile("src/home.gen")->find("compute()"), std::string::npos);
}

TEST(Pipeline, ObsoleteFilesRemovedOnlyWhenUnedited)
{
    Fixture f;
    f.model.screens.push_back(ir::makeScreen("settings", "Settings", 390, 844));
    f.generate();
    ASSERT_TRUE(f.filesystem->exists("src/settings.gen"));
    f.model.screens.pop_back();
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/settings.gen")->outcome, FileOutcome::RemovedObsolete);
    EXPECT_FALSE(f.filesystem->exists("src/settings.gen"));

    f.model.screens.push_back(ir::makeScreen("extra", "Extra", 390, 844));
    f.generate();
    ASSERT_TRUE(f.filesystem->writeFile("src/extra.gen", "edited by hand\n").ok());
    f.model.screens.pop_back();
    report = f.generate();
    EXPECT_EQ(findReport(report, "src/extra.gen")->outcome, FileOutcome::KeptObsolete);
    EXPECT_TRUE(f.filesystem->exists("src/extra.gen"));
}

TEST(Pipeline, DryRunWritesNothing)
{
    Fixture f;
    f.options.dryRun = true;
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Created);
    EXPECT_FALSE(f.filesystem->exists("src/home.gen"));
    EXPECT_FALSE(f.filesystem->exists(".forma/generated/main/manifest.json"));
}

TEST(Pipeline, ValidationErrorsBlockGeneration)
{
    Fixture f;
    f.model.screens[0].root.children[0].style.background = "not a color";
    auto report = f.generate();
    EXPECT_TRUE(report.blockedByErrors);
    EXPECT_FALSE(f.filesystem->exists("src/home.gen"));
}

TEST(Pipeline, CrLfOnDiskIsNotAnEdit)
{
    Fixture f;
    f.generate();
    std::string content = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "\n", "\r\n");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", content).ok());
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Unchanged);
}

TEST(Sync, CodeEditsUpdateTheDesign)
{
    Fixture f;
    f.generate();
    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "text = \"Welcome\";", "text = \"Welcome back\";");
    edited = text::replaceAll(edited, "size = 28;", "size = 32;");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());

    auto analysis = analyzeCodeChanges(*f.filesystem, f.model, "main");
    ASSERT_EQ(analysis.updates.size(), 2u);
    EXPECT_TRUE(analysis.unmapped.empty());

    ir::ModelEditor editor(f.model);
    ASSERT_TRUE(applyCodeChanges(editor, analysis).ok());
    const ir::Component* title = ir::findComponent(editor.model().screens[0].root, "title");
    EXPECT_EQ(ir::propertyString(*title, "content"), "Welcome back");
    EXPECT_EQ(*title->typography.size, 32);
    ASSERT_TRUE(editor.undo().ok());
    title = ir::findComponent(editor.model().screens[0].root, "title");
    EXPECT_EQ(ir::propertyString(*title, "content"), "Welcome");

    ASSERT_TRUE(editor.redo().ok());
    f.model = editor.model();
    auto report = f.generate();
    EXPECT_EQ(findReport(report, "src/home.gen")->outcome, FileOutcome::Unchanged);
}

TEST(Sync, UnsupportedCodeIsKeptAndMarkedCodeManaged)
{
    Fixture f;
    f.generate();
    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "text = \"Welcome\";", "text = loadGreetingFromServer();");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());

    auto analysis = analyzeCodeChanges(*f.filesystem, f.model, "main");
    EXPECT_TRUE(analysis.updates.empty());
    ASSERT_EQ(analysis.unmapped.size(), 1u);
    EXPECT_EQ(analysis.unmapped[0].component, "title");

    ir::ModelEditor editor(f.model);
    ASSERT_TRUE(applyCodeChanges(editor, analysis).ok());
    EXPECT_EQ(ir::findComponent(editor.model().screens[0].root, "title")->codeManaged, "partial");

    f.model = editor.model();
    auto report = f.generate();
    EXPECT_FALSE(report.hasConflicts());
    EXPECT_NE(f.filesystem->readFile("src/home.gen")->find("loadGreetingFromServer"), std::string::npos);
}

TEST(Sync, BothSidesChangedIsAConflictNotASilentWin)
{
    Fixture f;
    f.generate();
    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "text = \"Welcome\";", "text = \"From code\";");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());
    f.model.screens[0].root.children[0].properties["content"] = "From canvas";
    auto analysis = analyzeCodeChanges(*f.filesystem, f.model, "main");
    EXPECT_TRUE(analysis.updates.empty());
    ASSERT_EQ(analysis.conflicts.size(), 1u);
    EXPECT_EQ(analysis.conflicts[0].codeValue, "From code");
    EXPECT_EQ(analysis.conflicts[0].designValue, "From canvas");
}

TEST(Sync, EditsElsewhereInTheFileDoNotDisturbMappings)
{
    Fixture f;
    f.generate();
    std::string edited = *f.filesystem->readFile("src/home.gen");
    edited = text::replaceAll(edited, "// generated header", "// generated header\n// line one\n// line two");
    edited = text::replaceAll(edited, "text = \"Body text\";", "text = \"New body\";");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());
    auto analysis = analyzeCodeChanges(*f.filesystem, f.model, "main");
    ASSERT_EQ(analysis.updates.size(), 1u);
    EXPECT_EQ(analysis.updates[0].component, "body");
    EXPECT_EQ(analysis.updates[0].value, "New body");
}

TEST(Navigation, FindsCodeForAComponentAndBack)
{
    Fixture f;
    f.generate();
    auto location = locateComponent(*f.filesystem, "main", "home", "body");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->file, "src/home.gen");
    auto lines = text::splitLines(*f.filesystem->readFile("src/home.gen"));
    EXPECT_EQ(lines[static_cast<std::size_t>(location->line - 1)], "begin body");

    auto reference = componentAtLine(*f.filesystem, "main", "src/home.gen", location->line + 1);
    ASSERT_TRUE(reference.has_value());
    EXPECT_EQ(reference->component, "body");
    EXPECT_EQ(reference->key, "properties.content");

    std::string edited = text::replaceAll(*f.filesystem->readFile("src/home.gen"), "// generated header", "// a\n// b\n// c\n// generated header");
    ASSERT_TRUE(f.filesystem->writeFile("src/home.gen", edited).ok());
    auto moved = locateComponent(*f.filesystem, "main", "home", "body");
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(moved->line, location->line + 3);
    auto movedReference = componentAtLine(*f.filesystem, "main", "src/home.gen", moved->line);
    ASSERT_TRUE(movedReference.has_value());
    EXPECT_EQ(movedReference->component, "body");
}
