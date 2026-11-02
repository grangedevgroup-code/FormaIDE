#pragma once

#include <string>
#include <string_view>

#include "forma/core/json.h"

namespace forma::codegen {

Result<std::string> renderTemplate(std::string_view source, const Json& context);

}
