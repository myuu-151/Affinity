#pragma once

#include "../math/math_types.h"

namespace Affinity
{

// Initialize editor state
void FrameInit();

// Run one editor frame (called every frame from main loop)
void FrameTick(float dt);

// Get the current camera (for external queries)
const Mode7Camera& GetCamera();

// Render 3D viewport (call AFTER ImGui render, before swap)
void Render3DViewport();

} // namespace Affinity
