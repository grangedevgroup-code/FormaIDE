#pragma once

#include <memory>

#include "forma/codegen/generator.h"

namespace forma::generators {

std::unique_ptr<codegen::ICodeGenerator> createQtCppGenerator();
std::unique_ptr<codegen::ICodeGenerator> createHtmlCssGenerator();
std::unique_ptr<codegen::ICodeGenerator> createReactGenerator();
std::unique_ptr<codegen::ICodeGenerator> createWpfGenerator();
std::unique_ptr<codegen::ICodeGenerator> createTkGenerator();
std::unique_ptr<codegen::ICodeGenerator> createPySideGenerator();
std::unique_ptr<codegen::ICodeGenerator> createComposeGenerator();
std::unique_ptr<codegen::ICodeGenerator> createSwiftUiGenerator();

void registerBuiltinGenerators(codegen::GeneratorRegistry& registry);

}
