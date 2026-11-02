#pragma once

#include <string>
#include <vector>

#include "forma/ir/catalog.h"
#include "forma/ir/model.h"

namespace forma::ir {

enum class Severity { Info, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::string screen;
    std::string component;
    std::string key;
};

std::vector<Diagnostic> validateModel(const ApplicationModel& model, const ComponentCatalog& catalog = builtinCatalog());
bool hasErrors(const std::vector<Diagnostic>& diagnostics);
const char* severityName(Severity severity);

}
