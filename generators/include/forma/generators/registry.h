#pragma once

#include <memory>

#include "forma/codegen/generator.h"

namespace forma::generators {

std::unique_ptr<codegen::ICodeGenerator> createQtCppGenerator();
std::unique_ptr<codegen::ICodeGenerator> createHtmlCssGenerator();
std::unique_ptr<codegen::ICodeGenerator> createReactGenerator();
std::unique_ptr<codegen::ICodeGenerator> createWpfGenerator();

void registerBuiltinGenerators(codegen::GeneratorRegistry& registry);

}
