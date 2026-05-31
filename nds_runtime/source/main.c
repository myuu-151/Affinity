// Affinity NDS runtime — boot + main loop.
// Per-module work lives in audio.c, fps3d.c, mode0.c, hud.c, script_glue.c.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/videoGL.h>
#include <stdio.h>

static void init_video(void)
{
    powerOn(POWER_ALL_2D);

    // Top screen: Mode 0 with 3D on BG0 + OBJ for OAM sprites.
    // Without DISPLAY_SPR_ACTIVE the OAM table writes have no visible effect.
    videoSetMode(MODE_0_3D | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D_LAYOUT);

    // Bottom screen: console for debug output (Phase 5 will replace)
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleDemoInit();

    // 3D textures live on banks A + D (128KB each = 256KB total). Bank D is
    // otherwise unused; adding it as a second texture bank is what lets a
    // project hold more than ~one 256x256 texture alongside the 64KB sky.
    // (An earlier note claimed C/D "fail silently" — that was C, which is
    // taken by the bottom-screen console, and D tested *instead of* A rather
    // than *in addition*. A single texture still can't cross a 128KB bank,
    // but each 256x256 4bpp tile is only 32KB so that's fine.)
    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankD(VRAM_D_TEXTURE);
    vramSetBankE(VRAM_E_TEX_PALETTE);

    // OAM init + sprite VRAM moved to sprites.c (afn_sprite_init).

    glInit();
    // Anti-alias smooths mesh edges but fringes textured polygons (incl. the
    // sky panorama). Off by default; project-level AFN_NDS_AA toggle from
    // the editor flips it back on for projects that prefer smooth meshes.
    glEnable(GL_TEXTURE_2D);
#if defined(AFN_NDS_AA) && AFN_NDS_AA
    glEnable(GL_ANTIALIAS);
#else
    glDisable(GL_ANTIALIAS);
#endif
    // First-pass sky: solid blue clear-color. A proper textured skybox would
    // need the editor's panorama re-quantized for NDS (4bpp/8bpp single
    // palette instead of the GBA's 4-band sub-palette encoding) and rendered
    // as a view-space fullscreen quad. Phase 2c-full.
    glClearColor(10, 18, 31, 31);
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // Far plane bumped from 100 → 1024. Our coords are 16.8 fixed scaled
    // through fx8_to_f32 (shift-left-4), so a 256-unit-wide scene maps to
    // ~256 in f32 world units after the matrix — easy to put the far plane
    // inside the scene with the default value of 100.
    gluPerspective(70, 256.0 / 192.0, 0.1, 1024);

    // Warm up the geometry engine: it needs the projection matrix to settle
    // before the first user draw, otherwise the first frame renders with a
    // stale (often identity) projection — the cube appears as a single dot
    // until the next D-pad input forces a fresh frame.
    glFlush(0);
    swiWaitForVBlank();
}

// Project's start mode (0=Mode 4 / 3D, 1=Mode 0 / HUD+tilemap, 2=Mode 1 /
// Mode 7). Defaults to Mode 4 so older builds without the macro link.
#ifndef AFN_START_MODE
#define AFN_START_MODE 0
#endif

int main(void)
{
    init_video();

    afn_audio_init();
#if AFN_START_MODE == 0
    afn_fps3d_init();
#else
    afn_mode0_init();
#endif
    afn_sprite_init();
    afn_hud_init();
    afn_script_init();

    irqSet(IRQ_HBLANK, m7_hbl);
    irqEnable(IRQ_HBLANK);

    iprintf("Affinity NDS\n");

#if AFN_START_MODE == 0
    // Pre-roll the GPU pipeline: submit-display latency means the first user
    // frame appears with stale state. A few warmup frames push valid geometry
    // through before the player sees anything.
    for (int w = 0; w < 4; w++) {
        afn_fps3d_update();
        swiWaitForVBlank();
    }
#endif

    while (pmMainLoop())
    {
        afn_script_tick();
        afn_scene_tick();      // fade state machine
        // Dispatch on runtime mode so ChangeScene transitions can flip
        // between Mode 0 / Mode 4 without recompiling.
        if (afn_current_mode == 1) {
            afn_mode0_update();
        } else {
            afn_fps3d_update();
            afn_sprite_update();
        }
        afn_hud_draw();
        afn_audio_tick();
        swiWaitForVBlank();
    }

    return 0;
}
