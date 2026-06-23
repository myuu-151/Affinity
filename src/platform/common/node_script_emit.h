#pragma once
#include <ostream>
#include <vector>
#include "afn_export_ir.h"

namespace Affinity {

// Emit the visual-script node graph as C (event dispatchers + blueprint
// dispatchers) to `f`. Shared by the NDS and PSV exporters so both run the
// SAME generated code. Caller emits the surrounding extern/define block and the
// runtime defines the variables the emitted code touches.
void EmitNodeScriptBodies(std::ostream& f,
                          const AfnScriptExport& script,
                          const std::vector<AfnBlueprintExport>& blueprints,
                          const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                          const std::vector<AfnSpriteExport>& sprites,
                          const std::vector<AfnSoundInstanceExport>& soundInstances);

} // namespace Affinity
