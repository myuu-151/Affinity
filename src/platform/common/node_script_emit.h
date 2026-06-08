#pragma once
#include <ostream>
#include <vector>
#include "../gba/gba_package.h"

namespace Affinity {

// Emit the visual-script node graph as C (event dispatchers + blueprint
// dispatchers) to `f`. Shared by the NDS and PSV exporters so both run the
// SAME generated code. Caller emits the surrounding extern/define block and the
// runtime defines the variables the emitted code touches.
void EmitNodeScriptBodies(std::ostream& f,
                          const GBAScriptExport& script,
                          const std::vector<GBABlueprintExport>& blueprints,
                          const std::vector<GBABlueprintInstanceExport>& bpInstances,
                          const std::vector<GBASpriteExport>& sprites,
                          const std::vector<GBASoundInstanceExport>& soundInstances);

} // namespace Affinity
