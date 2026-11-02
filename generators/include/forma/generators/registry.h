#pragma once

#include <memory>

#include "forma/codegen/generator.h"

namespace forma::generators {

std::unique_ptr<codegen::ICodeGenerator> createQtCppGenerator();

void registerBuiltinGenerators(codegen::GeneratorRegistry& registry);

}
