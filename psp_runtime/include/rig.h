// Affinity PSP runtime — player rig (DSMA-style skinned glTF) rendering.
// Rigid 1-bone-per-vertex skinning done on the CPU each frame.
#pragma once

void rig_init(void);
int  rig_present(void);                    // 1 if this build has a player rig
void rig_player_start(float out[3]);       // start world position (0 if none)
void rig_set_moving(int moving);           // switch idle/walk clip

// Draw the player rig at world (px,py,pz), facing yawDeg, aligned so its up
// matches upN (the floor normal — for slope tilt), advancing the animation
// unless frozen. camR/camU/camF are the world-space camera basis (right/up/
// forward) used to aim the camera headlamp. No-op when there's no rig.
void rig_render(float px, float py, float pz, float yawDeg, const float* upN,
                const float* camR, const float* camU, const float* camF, int frozen);
