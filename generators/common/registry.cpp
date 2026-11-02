#include "forma/generators/registry.h"

namespace forma::generators {

void registerBuiltinGenerators(codegen::GeneratorRegistry& registry)
{
    registry.add(createQtCppGenerator());
}

}
