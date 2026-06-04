// Affinity PSP runtime — player rig (DSMA-style skinned glTF) rendering.
// Rigid 1-bone-per-vertex skinning done on the CPU each frame.
#pragma once

void rig_init(void);
int  rig_present(void);                    // 1 if this build has a player rig
void rig_player_start(float out[3]);       // start world position (0 if none)

// Draw the player rig at world (px,py,pz), facing yawDeg, advancing the
// animation unless frozen. No-op when there's no rig.
void rig_render(float px, float py, float pz, float yawDeg, int frozen);
