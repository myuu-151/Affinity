#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../gba/gba_package.h" // Reuse export structs — they're platform-neutral

namespace Affinity
{

// Package the current map into a .nds ROM.
// runtimeDir: path to nds_runtime/ directory
// outputPath: where to write the final .nds
// Returns true on success. errorMsg receives details on failure.
bool PackageNDS(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                std::string& errorMsg);

} // namespace Affinity
