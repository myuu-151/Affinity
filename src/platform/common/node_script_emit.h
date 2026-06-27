#pragma once
#include <ostream>
#include <vector>
#include <string>
#include "afn_export_ir.h"

namespace Affinity {

// Emit the visual-script node graph as C (event dispatchers + blueprint
// dispatchers) to `f`. Shared by the NDS and PSV exporters so both run the
// SAME generated code. Caller emits the surrounding extern/define block and the
// runtime defines the variables the emitted code touches.
// hudLayerRemap / hudLayerCount (optional): map a node's flat editor-layer index
// (what a PlayHudAnim/StopHudAnim node stores) to a CONTIGUOUS range of runtime
// afn_hud_layer[] entries — [hudLayerRemap[F], hudLayerRemap[F] + hudLayerCount[F]).
// PSV emits one runtime layer PER ITEM (per-item keyframe tracks), so an editor
// layer with N animated pieces becomes N runtime layers; a node must drive all of
// them (e.g. a dropshadow + main both blinking). Empty (NDS) = identity (one layer).
void EmitNodeScriptBodies(std::ostream& f,
                          const AfnScriptExport& script,
                          const std::vector<AfnBlueprintExport>& blueprints,
                          const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                          const std::vector<AfnSpriteExport>& sprites,
                          const std::vector<AfnSoundInstanceExport>& soundInstances,
                          const std::vector<int>& hudLayerRemap = {},
                          const std::vector<int>& hudLayerCount = {},
                          // Rig clip names in INDEX ORDER (clipNames[i] = name of clip i),
                          // so the AI Clips node can name-resolve enemy clip defaults and
                          // survive a glTF re-sort. Empty (NDS) = literal fallbacks.
                          const std::vector<std::string>& clipNames = {});

} // namespace Affinity
