// Affinity PSP runtime — Mode 4 scene (camera, input, mesh draw).
#include "scene.h"
#include "affinity_psp.h"
#include "meshcull.h"

#include <pspkernel.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <math.h>

#define DEG2RAD (3.14159265f / 180.0f)

// ---- Camera state ---------------------------------------------------------
static float camX, camY, camZ;   // eye world position
static float camAngle;           // yaw (radians); forward = (sin, 0, cos)

void scene_init(void) {
    meshcull_build();
    // Const mesh/texture data baked into the ELF is still in the CPU data cache
    // after loading; the GE reads physical RAM, so without a writeback it sees
    // stale/zero bytes — textures render black, bucket index arrays read garbage.
    // Flush once now that all scene data (incl. meshcull's malloc'd indices) is set.
    sceKernelDcacheWritebackAll();
    camX = afn_cam_start_x;
    camZ = afn_cam_start_z;
    camY = afn_cam_start_h;
    camAngle = afn_cam_start_angle;
}

void scene_update(void) {
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    // D-pad L/R yaw, U/D height.
    if (pad.Buttons & PSP_CTRL_LEFT)  camAngle -= 0.04f;
    if (pad.Buttons & PSP_CTRL_RIGHT) camAngle += 0.04f;
    if (pad.Buttons & PSP_CTRL_UP)    camY += 4.0f;
    if (pad.Buttons & PSP_CTRL_DOWN)  camY -= 4.0f;

    // Analog stick: forward/back + strafe in the look plane.
    float ax = (pad.Lx - 128) / 128.0f;
    float ay = (pad.Ly - 128) / 128.0f;
    if (ax < 0.15f && ax > -0.15f) ax = 0.0f;   // deadzone
    if (ay < 0.15f && ay > -0.15f) ay = 0.0f;

    float fwdX = sinf(camAngle), fwdZ = cosf(camAngle);
    float rgtX = cosf(camAngle), rgtZ = -sinf(camAngle);
    float speed = afn_walk_speed > 0.0f ? afn_walk_speed * 0.25f : 6.0f;
    camX += (-ay * fwdX + ax * rgtX) * speed;
    camZ += (-ay * fwdZ + ax * rgtZ) * speed;
}

void scene_render(void) {
    // Projection.
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 480.0f / 272.0f, 1.0f, 10000.0f);

    // View (camera).
    ScePspFVector3 eye    = { camX, camY, camZ };
    ScePspFVector3 center = { camX + sinf(camAngle), camY, camZ + cosf(camAngle) };
    ScePspFVector3 up     = { 0.0f, 1.0f, 0.0f };
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumLookAt(&eye, &center, &up);

    float camSin = sinf(camAngle), camCos = cosf(camAngle);
    float drawDist = afn_draw_distance;

    sceGumMatrixMode(GU_MODEL);

    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        if (!m->visible || m->vertCount <= 0) continue;

        const AfnSpriteInst* sp = &afn_sprites[si];

        // Model matrix: T * Rz * Rx * Ry * S (vertex sees scale first).
        sceGumLoadIdentity();
        ScePspFVector3 t = { sp->x, sp->y, sp->z };
        sceGumTranslate(&t);
        if (sp->rotZ != 0.0f) sceGumRotateZ(sp->rotZ * DEG2RAD);
        if (sp->rotX != 0.0f) sceGumRotateX(sp->rotX * DEG2RAD);
        if (sp->rotY != 0.0f) sceGumRotateY(sp->rotY * DEG2RAD);
        ScePspFVector3 s = { sp->scale, sp->scale, sp->scale };
        sceGumScale(&s);

        // Face culling. The exported triangle winding matches the editor/NDS
        // (DS back-cull shows the terrain top); the PSP's default front-face
        // sense is the opposite, so back-cull (0) treats CCW as front here.
        if (m->cullMode == 2) {
            sceGuDisable(GU_CULL_FACE);
        } else {
            sceGuEnable(GU_CULL_FACE);
            sceGuFrontFace(m->cullMode == 1 ? GU_CW : GU_CCW);
        }

        // Texture / blend.
        if (m->textured && m->texPixels) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
            sceGuTexImage(0, m->texW, m->texH, m->texW, m->texPixels);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            // Point sampling (no bilinear) — matches the DS, which has no texture
            // filter. The level texture is an atlas/tileset; bilinear bleeds
            // neighbouring tiles (and the black padding) across tile edges,
            // showing as seams. NEAREST + CLAMP samples only the intended texel.
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            // Only level 0 is uploaded. Without this, minified (distant) tris make
            // the GE select a non-existent mip and sample black — force LOD 0.
            sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
            sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        if (m->texHasAlpha) {
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        } else {
            sceGuDisable(GU_BLEND);
        }

        meshcull_draw(mi, sp->x, sp->y, sp->z, sp->scale,
                      sp->rotY, sp->rotX, sp->rotZ,
                      camX, camY, camZ, camSin, camCos, drawDist);
    }

    // Restore default cull state for next frame's clear, etc.
    sceGuEnable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);
    sceGuEnable(GU_TEXTURE_2D);
}
