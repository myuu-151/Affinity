// Affinity PSP runtime — sky panorama. Drawn as a far textured quad in VIEW
// space (identity view) so it tracks the camera; U scrolls with yaw for the
// 360° wrap (matches the NDS, which scrolls a flat panorama rather than a true
// skybox). Reuses the proven AfnVertex/sceGumDrawArray path. Writes no depth so
// the 3D scene always draws on top.
#include "sky.h"
#include "affinity_psp.h"
#include "psp_sky.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#ifdef AFN_HAS_SKY

static AfnVertex __attribute__((aligned(16))) s_sky[4];

void sky_render(float camAngle) {
    float u = camAngle / (2.0f * 3.14159265f);
    const float D = 5000.0f, X = 7200.0f, Y = 4200.0f;  // far quad over-filling the view
    s_sky[0].u=u;        s_sky[0].v=0.0f; s_sky[0].color=0xFFFFFFFF; s_sky[0].x=-X; s_sky[0].y= Y; s_sky[0].z=-D;
    s_sky[1].u=u+1.0f;   s_sky[1].v=0.0f; s_sky[1].color=0xFFFFFFFF; s_sky[1].x= X; s_sky[1].y= Y; s_sky[1].z=-D;
    s_sky[2].u=u+1.0f;   s_sky[2].v=1.0f; s_sky[2].color=0xFFFFFFFF; s_sky[2].x= X; s_sky[2].y=-Y; s_sky[2].z=-D;
    s_sky[3].u=u;        s_sky[3].v=1.0f; s_sky[3].color=0xFFFFFFFF; s_sky[3].x=-X; s_sky[3].y=-Y; s_sky[3].z=-D;
    sceKernelDcacheWritebackRange(s_sky, sizeof(s_sky));

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(75.0f, 480.0f / 272.0f, 1.0f, 20000.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();

    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);     // 1 = no depth writes (stay behind the scene)
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, AFN_SKY_W, AFN_SKY_H, AFN_SKY_W, afn_sky_tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
    sceGuTexWrap(GU_REPEAT, GU_CLAMP);
    sceGumDrawArray(GU_TRIANGLE_FAN, AFN_VERTEX_FLAGS, 4, 0, s_sky);

    sceGuDepthMask(GU_FALSE);
    sceGuEnable(GU_DEPTH_TEST);
}
int sky_present(void) { return 1; }

#else

void sky_render(float a) { (void)a; }
int  sky_present(void) { return 0; }

#endif
