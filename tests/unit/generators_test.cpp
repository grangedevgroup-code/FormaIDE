#include <gtest/gtest.h>

#include <set>

#include "forma/codegen/generator.h"
#include "forma/generators/registry.h"
#include "forma/ir/catalog.h"
#include "forma/ir/model.h"
#include "forma/project/project.h"

using namespace forma;

namespace {

ir::ApplicationModel richModel()
{
    project::ProjectDescriptor descriptor;
    descriptor.name = "Rich App";
    descriptor.bundleId = "com.example.richapp";
    ir::ApplicationModel model = project::buildStarterModel(descriptor, "desktop");
    auto catalog = ir::builtinCatalog();
    ir::Component& root = model.screens.front().root;
    int counter = 0;
    for (const char* type : {"text", "button", "textinput", "checkbox", "radio", "switch", "dropdown", "slider", "divider", "spacer", "list", "image"}) {
        ir::Component child = catalog.instantiate(type, std::string("extra") + std::to_string(++counter));
        if (child.type == "text")
            child.properties["content"] = "Quote \" and <angle> and \\ and $dollar";
        if (child.type == "button")
            child.properties["label"] = "Save & close";
        root.children.push_back(child);
    }
    ir::Component row = catalog.instantiate("container", "rowBox");
    row.layout.kind = ir::LayoutKind::Horizontal;
    row.layout.spacing = 8;
    row.children.push_back(catalog.instantiate("button", "rowButton"));
    root.children.push_back(row);
    ir::Component grid = catalog.instantiate("container", "gridBox");
    grid.layout.kind = ir::LayoutKind::Grid;
    grid.layout.columns = {{ir::TrackKind::Fraction, 1}, {ir::TrackKind::Fixed, 100}};
    grid.layout.rows = {{ir::TrackKind::Auto, 0}};
    grid.children.push_back(catalog.instantiate("text", "gridText"));
    root.children.push_back(grid);
    ir::Component canvas = catalog.instantiate("container", "absoluteBox");
    canvas.layout.kind = ir::LayoutKind::Absolute;
    canvas.children.push_back(catalog.instantiate("text", "absoluteText"));
    root.children.push_back(canvas);
    return model;
}

}

TEST(Generators, RegisteredListMatchesWhatIsTested)
{
    codegen::GeneratorRegistry registry;
    generators::registerBuiltinGenerators(registry);
    std::set<std::string> ids;
    for (const auto* generator : registry.all())
        ids.insert(generator->info().id);
    std::set<std::string> expected = {"qt-cpp", "html-css", "react-ts", "wpf-csharp", "python-tk", "pyside6", "compose-kotlin", "swiftui"};
    EXPECT_EQ(ids, expected);
}

TEST(Generators, EveryGeneratorHandlesEveryCoreComponent)
{
    codegen::GeneratorRegistry registry;
    generators::registerBuiltinGenerators(registry);
    ir::ApplicationModel model = richModel();
    codegen::ProjectConfiguration config;
    config.name = "Rich App";
    config.bundleId = "com.example.richapp";
    for (const auto* generator : registry.all()) {
        SCOPED_TRACE(generator->info().id);
        auto first = generator->generate(model, config);
        auto second = generator->generate(model, config);
        ASSERT_FALSE(first.files.empty());
        ASSERT_EQ(first.files.size(), second.files.size());
        std::set<std::string> paths;
        int userFiles = 0;
        for (std::size_t i = 0; i < first.files.size(); ++i) {
            const auto& file = first.files[i];
            EXPECT_TRUE(paths.insert(file.path).second) << "duplicate " << file.path;
            EXPECT_EQ(file.content, second.files[i].content) << "not deterministic: " << file.path;
            EXPECT_EQ(file.path.find(".."), std::string::npos);
            userFiles += file.category == codegen::FileCategory::User ? 1 : 0;
            for (const auto& binding : file.bindings)
                EXPECT_GT(binding.line, 0);
        }
        EXPECT_GT(userFiles, 0);
    }
}

TEST(Generators, EveryStarterOnlyOffersRegisteredGeneratorsWhenFiltered)
{
    codegen::GeneratorRegistry registry;
    generators::registerBuiltinGenerators(registry);
    project::TemplateRegistry templates;
    int usable = 0;
    for (const auto& item : templates.all()) {
        if (item.generator.empty() || registry.find(item.generator) != nullptr)
            ++usable;
    }
    EXPECT_GE(usable, 14);
}

TEST(Generators, MarkupEscapesUserText)
{
    codegen::GeneratorRegistry registry;
    generators::registerBuiltinGenerators(registry);
    ir::ApplicationModel model = richModel();
    codegen::ProjectConfiguration config;
    config.name = "Rich App";
    auto html = registry.find("html-css")->generate(model, config);
    bool checked = false;
    for (const auto& file : html.files) {
        if (file.path == "index.html") {
            EXPECT_NE(file.content.find("Save &amp; close"), std::string::npos);
            EXPECT_EQ(file.content.find("<angle>"), std::string::npos);
            checked = true;
        }
    }
    EXPECT_TRUE(checked);
}
