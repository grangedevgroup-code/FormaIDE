#include "forma/codegen/generator.h"

#include <algorithm>

#include "forma/ir/catalog.h"

namespace forma::codegen {

void GeneratorRegistry::add(std::unique_ptr<ICodeGenerator> generator)
{
    std::string id = generator->info().id;
    auto existing = std::find_if(generators_.begin(), generators_.end(), [&](const auto& item) { return item->info().id == id; });
    if (existing != generators_.end())
        *existing = std::move(generator);
    else
        generators_.push_back(std::move(generator));
}

const ICodeGenerator* GeneratorRegistry::find(std::string_view id) const
{
    auto it = std::find_if(generators_.begin(), generators_.end(), [&](const auto& item) { return item->info().id == id; });
    return it == generators_.end() ? nullptr : it->get();
}

std::vector<const ICodeGenerator*> GeneratorRegistry::all() const
{
    std::vector<const ICodeGenerator*> output;
    for (const auto& generator : generators_)
        output.push_back(generator.get());
    return output;
}

const char* fileCategoryName(FileCategory category)
{
    switch (category) {
    case FileCategory::Generated:
        return "generated";
    case FileCategory::User:
        return "user";
    case FileCategory::Protected:
        return "protected";
    }
    return "generated";
}

FileCategory fileCategoryFromName(std::string_view name)
{
    if (name == "user")
        return FileCategory::User;
    if (name == "protected")
        return FileCategory::Protected;
    return FileCategory::Generated;
}

std::vector<ir::Diagnostic> checkCapabilities(const ir::ApplicationModel& model, const GeneratorCapabilities& capabilities, std::string_view generatorLabel)
{
    std::vector<ir::Diagnostic> diagnostics;
    auto supports = [](const std::vector<std::string>& list, const std::string& value) {
        return list.empty() || std::find(list.begin(), list.end(), value) != list.end();
    };
    auto check = [&](const ir::Component& component, const std::string& owner) {
        if (component.type != "screen" && !supports(capabilities.components, component.type)) {
            diagnostics.push_back({ir::Severity::Warning, "generator.component",
                                   std::string(generatorLabel) + " has no native match for " + component.type + "; it will generate a placeholder.",
                                   owner, component.id, {}});
        }
        if (!component.children.empty() && !supports(capabilities.layouts, ir::layoutKindName(component.layout.kind))) {
            diagnostics.push_back({ir::Severity::Warning, "generator.layout",
                                   std::string(generatorLabel) + " cannot express the " + ir::layoutKindName(component.layout.kind) + " layout exactly; it will use the closest match.",
                                   owner, component.id, "layout.type"});
        }
        for (const auto& [event, action] : component.events) {
            if (!supports(capabilities.events, event)) {
                diagnostics.push_back({ir::Severity::Warning, "generator.event",
                                       std::string(generatorLabel) + " does not wire up " + event + " on " + component.type + ".",
                                       owner, component.id, "events." + event});
            }
        }
    };
    for (const auto& screen : model.screens)
        ir::visitComponents(screen.root, [&](const ir::Component& component, const ir::Component*) { check(component, screen.root.id); });
    for (const auto& definition : model.components)
        ir::visitComponents(definition.root, [&](const ir::Component& component, const ir::Component*) { check(component, definition.root.id); });
    return diagnostics;
}

}
