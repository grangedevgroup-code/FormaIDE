#pragma once

#include "forma/core/filesystem_service.h"
#include "forma/core/json.h"
#include "forma/ir/model.h"

namespace forma::ir {

Json componentToJson(const Component& component, bool includeChildren = true);
Result<Component> componentFromJson(const Json& json);

Json screenToJson(const Screen& screen);
Result<Screen> screenFromJson(const Json& json);

Json definitionToJson(const ComponentDefinition& definition);
Result<ComponentDefinition> definitionFromJson(const Json& json);

Json themeToJson(const Theme& theme);
Result<Theme> themeFromJson(const Json& json);

Json applicationToJson(const ApplicationModel& model);
Result<ApplicationModel> applicationFromJson(const Json& json);

Json applicationToFullJson(const ApplicationModel& model);
Result<ApplicationModel> applicationFromFullJson(const Json& json);

Result<ApplicationModel> loadApplication(const IFilesystemService& filesystem, std::string_view designDirectory = "design");
Status saveApplication(IFilesystemService& filesystem, const ApplicationModel& model, std::string_view designDirectory = "design",
                       Origin origin = Origin::User);

Status setComponentField(Component& component, std::string_view key, const Json& value);
Json getComponentField(const Component& component, std::string_view key);
Status applyFieldOverrides(Component& component, const Json& overrides);

}
