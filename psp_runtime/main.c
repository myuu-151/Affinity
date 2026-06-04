// Affinity PSP runtime — entry point, sceGu init, frame loop.
// Mode 4 (3D) scene rendered with the PSP GE (hardware T&L) via sceGu/sceGum.

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <stdlib.h>

#include "scene.h"

PSP_MODULE_INFO("Affinity", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// ---- Display / GE setup ---------------------------------------------------
#define BUF_WIDTH   512   // framebuffer stride (must be >= screen width, pow2-ish)
#define SCR_WIDTH   480
#define SCR_HEIGHT  272

// GE display list — must be 16-byte aligned, in a section the GE can DMA.
static unsigned int __attribute__((aligned(16))) gu_list[262144];

// ---- Exit callback so HOME quits cleanly ---------------------------------
static int exit_requested = 0;
static int exit_callback(int arg1, int arg2, void* common) { exit_requested = 1; return 0; }
static int callback_thread(SceSize args, void* argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) sceKernelStartThread(thid, 0, 0);
}

// vram cursor for static framebuffer/zbuffer allocation
static unsigned int vram_offset = 0;
static void* vram_alloc(unsigned int w, unsigned int h, unsigned int psm) {
    unsigned int bpp = (psm == GU_PSM_T8) ? 1 : (psm == GU_PSM_5650 || psm == GU_PSM_5551 || psm == GU_PSM_4444) ? 2 : 4;
    unsigned int sz = w * h * bpp;
    void* p = (void*)(vram_offset);
    vram_offset += sz;
    return p;
}

static void gu_init(void) {
    void* fbp0 = vram_alloc(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888);
    void* fbp1 = vram_alloc(BUF_WIDTH, SCR_HEIGHT, GU_PSM_8888);
    void* zbp  = vram_alloc(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);

    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp1, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int main(void) {
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    gu_init();
    scene_init();

    while (!exit_requested) {
        sceGuStart(GU_DIRECT, gu_list);
        sceGuClearColor(0xff402010);    // dark slate (ABGR)
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        scene_update();
        scene_render();

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
