#pragma once
#include <ostream>
#include <vector>
#include "afn_export_ir.h"

namespace Affinity {

// Emit the visual-script node graph as C (event dispatchers + blueprint
// dispatchers) to `f`. Shared by the NDS and PSV exporters so both run the
// SAME generated code. Caller emits the surrounding extern/define block and the
// runtime defines the variables the emitted code touches.
// hudLayerRemap (optional): maps a node's flat editor-layer index (the index a
// PlayHudAnim/StopHudAnim node stores) to the runtime afn_hud_layer[] index. PSV
// emits one runtime layer PER ITEM (per-item keyframe tracks), so an element with
// a multi-item layer shifts later layer indices; this remaps node references to
// match. Empty (NDS) = identity, one runtime layer per editor-layer.
void EmitNodeScriptBodies(std::ostream& f,
                          const AfnScriptExport& script,
                          const std::vector<AfnBlueprintExport>& blueprints,
                          const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                          const std::vector<AfnSpriteExport>& sprites,
                          const std::vector<AfnSoundInstanceExport>& soundInstances,
                          const std::vector<int>& hudLayerRemap = {});

} // namespace Affinity
