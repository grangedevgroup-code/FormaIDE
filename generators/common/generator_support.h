#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "forma/codegen/code_writer.h"
#include "forma/codegen/generator.h"
#include "forma/ir/catalog.h"
#include "forma/ir/model.h"

namespace forma::generators {

struct ColorRef {
    bool isToken = false;
    std::string token;
    std::string literal;
};

ColorRef colorRef(const ir::ApplicationModel& model, std::string_view value);

class NameTable {
public:
    enum class Style { Camel, Pascal, Snake };

    NameTable(Style style, std::set<std::string> reserved) : style_(style), reserved_(std::move(reserved)) {}
    std::string name(const std::string& id);

private:
    Style style_;
    std::set<std::string> reserved_;
    std::map<std::string, std::string> names_;
    std::set<std::string> used_;
};

struct ScreenNames {
    const ir::Screen* screen = nullptr;
    std::string className;
};

std::vector<ScreenNames> screenNames(const ir::ApplicationModel& model, std::string_view suffix = "Screen");
std::string startScreenId(const ir::ApplicationModel& model);
std::string definitionClassName(const ir::ComponentDefinition& definition);

bool isContainerType(const ir::Component& component);
bool usesType(const ir::ApplicationModel& model, std::string_view type);
bool usesLayout(const ir::ApplicationModel& model, ir::LayoutKind kind);
bool usesWrap(const ir::ApplicationModel& model);
std::vector<std::string> collectEventActions(const ir::Component& component);

std::string stateDefaultString(const ir::StateVariable& variable);
double stateDefaultNumber(const ir::StateVariable& variable);
bool stateDefaultBool(const ir::StateVariable& variable);
std::vector<std::string> stateDefaultList(const ir::StateVariable& variable);

const std::vector<std::string>& standardEvents();
std::vector<std::string> allComponentTypes();
std::vector<std::string> allLayoutKinds();

std::string projectSlug(std::string_view name);
std::string projectClass(std::string_view name);
std::string packageName(std::string_view bundleId);

std::string bindingKeyFor(const ir::Component& component);
std::optional<std::string> boundState(const ir::Component& component, const std::string& key);

struct PrimaryText {
    std::string key;
    std::string value;
};

std::optional<PrimaryText> primaryText(const ir::Component& component);

}
