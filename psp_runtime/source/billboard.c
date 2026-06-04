// Affinity PSP runtime — sprite billboards. Non-mesh, non-rig sprites drawn as
// camera-facing (Y-axis) textured quads with per-instance frame animation.
// Drawn in world space with the scene's view/projection, depth-tested against
// the 3D scene. Alpha-blended (frame palette index 0 = transparent).
#include "billboard.h"
#include "affinity_psp.h"
#include "psp_sprites.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <math.h>

#ifdef AFN_HAS_SPRITES

static float     s_frame[AFN_SPR_INST_COUNT];
static AfnVertex __attribute__((aligned(16))) s_quads[AFN_SPR_INST_COUNT * 4];
static int       s_init = 0;

void billboards_render(float camX, float camY, float camZ, float camAngle) {
    (void)camX; (void)camY; (void)camZ;
    if (!s_init) { s_init = 1; for (int i = 0; i < AFN_SPR_INST_COUNT; i++) s_frame[i] = (float)afn_spr_fstart[i]; }

    float rx = cosf(camAngle), rz = -sinf(camAngle);   // camera right in XZ
    int gf[AFN_SPR_INST_COUNT];

    // Pass 1: advance animation + build all quads, then one cache flush.
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
        int lo = afn_spr_fstart[i], hi = afn_spr_fend[i];
        if (hi < lo) hi = lo;
        s_frame[i] += afn_spr_fps[i] / 60.0f;
        if (s_frame[i] >= (float)(hi + 1)) s_frame[i] = (float)lo;
        int cf = (int)s_frame[i]; if (cf < lo) cf = lo; if (cf > hi) cf = hi;
        gf[i] = cf;

        float sz  = afn_spr_basesize[i] * afn_spr_scale[i] * 0.25f;  // world px (tune)
        float hw  = sz * 0.5f;
        float px = afn_spr_x[i], py = afn_spr_y[i], pz = afn_spr_z[i];
        float lx = px - rx*hw, lz = pz - rz*hw;
        float Rx = px + rx*hw, Rz = pz + rz*hw;
        float top = py + sz, bot = py;
        AfnVertex* q = &s_quads[i*4];
        q[0].u=0;q[0].v=0;q[0].color=0xFFFFFFFF;q[0].x=lx;q[0].y=top;q[0].z=lz;
        q[1].u=1;q[1].v=0;q[1].color=0xFFFFFFFF;q[1].x=Rx;q[1].y=top;q[1].z=Rz;
        q[2].u=1;q[2].v=1;q[2].color=0xFFFFFFFF;q[2].x=Rx;q[2].y=bot;q[2].z=Rz;
        q[3].u=0;q[3].v=1;q[3].color=0xFFFFFFFF;q[3].x=lx;q[3].y=bot;q[3].z=lz;
    }
    sceKernelDcacheWritebackRange(s_quads, sizeof(s_quads));

    // Pass 2: draw each billboard (identity model — verts are world space).
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
        int cf = gf[i];
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, afn_spr_frame_w[cf], afn_spr_frame_h[cf], afn_spr_frame_w[cf], afn_spr_frame_ptrs[cf]);
        sceGumDrawArray(GU_TRIANGLE_FAN, AFN_VERTEX_FLAGS, 4, 0, &s_quads[i*4]);
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
}

#else

void billboards_render(float a, float b, float c, float d) { (void)a;(void)b;(void)c;(void)d; }

#endif
