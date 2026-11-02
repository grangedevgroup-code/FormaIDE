#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "forma/core/json.h"
#include "forma/ir/model.h"

namespace forma::ir {

enum class PropertyType { Text, MultilineText, Number, Boolean, Color, Enum, StringList, Asset, Json };

struct PropertySchema {
    std::string key;
    std::string label;
    std::string group;
    PropertyType type = PropertyType::Text;
    forma::Json defaultValue;
    bool required = false;
    std::vector<std::string> options;
};

struct ComponentSchema {
    std::string type;
    std::string label;
    std::string category;
    std::string icon;
    bool container = false;
    forma::Json defaults = forma::Json::object();
    std::vector<PropertySchema> properties;
    std::vector<std::string> events;
};

class ComponentCatalog {
public:
    void add(ComponentSchema schema);
    const ComponentSchema* find(std::string_view type) const;
    const std::vector<ComponentSchema>& all() const { return schemas_; }
    std::vector<std::string> categories() const;
    Component instantiate(std::string_view type, std::string id) const;

private:
    std::vector<ComponentSchema> schemas_;
};

const ComponentCatalog& builtinCatalog();

}
