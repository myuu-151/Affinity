// Affinity PSP runtime — mesh collision (floor height + wall pushback).
// Faces are built once at load from the exported mesh geometry (world-space
// triangles), classified floor/wall/ceiling by normal, and bucketed into an
// XZ grid. Ported from nds_runtime/collision.c (float instead of 16.8 fixed).
#pragma once

void collide_build(void);
// Highest floor at/below (py + headroom) under (x,z). Returns 1 + sets *outY.
int  collide_floor(float x, float z, float py, float* outY);
// Push (x,z) out of nearby wall faces at height py.
void collide_walls(float* x, float* z, float py);
