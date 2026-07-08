// Affinity Switch runtime — platform shim over libnx.
// The PSV runtime (main.c/audio.c) is a fork with its Sony calls redirected
// here: EGL replaces vitaGL's init/swap, libnx pad fills a Vita-shaped
// SceCtrlData, and the scePower/vgl knobs become no-ops. Keeping the Vita API
// shapes means the forked main.c stays near-identical to psv_runtime/main.c —
// port fixes across by diff.
#pragma once
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>       // OpenGL ES 1.1 fixed-function (mesa/nouveau)
#include <stdlib.h>
#include <stdbool.h>

// Display surface (the NWindow default). SCR_W/SCR_H in main.c stay 960x544 as
// the COORDINATE space (HUD ortho, aspect) — GL scales it to this viewport.
#define SWITCH_DISP_W 1280
#define SWITCH_DISP_H 720

// ---- Vita ctrl API shape ----------------------------------------------------
// Bits are our own (main.c only uses the names). Sticks are Vita-format:
// unsigned 0..255, center 128, Y grows DOWNWARD.
#define SCE_CTRL_SELECT   0x000001
#define SCE_CTRL_L3       0x000002
#define SCE_CTRL_R3       0x000004
#define SCE_CTRL_START    0x000008
#define SCE_CTRL_UP       0x000010
#define SCE_CTRL_RIGHT    0x000020
#define SCE_CTRL_DOWN     0x000040
#define SCE_CTRL_LEFT     0x000080
#define SCE_CTRL_LTRIGGER 0x000100   // Vita: the L2 emu bit
#define SCE_CTRL_RTRIGGER 0x000200
#define SCE_CTRL_L1       0x000400
#define SCE_CTRL_R1       0x000800
#define SCE_CTRL_L2       SCE_CTRL_LTRIGGER
#define SCE_CTRL_R2       SCE_CTRL_RTRIGGER
#define SCE_CTRL_TRIANGLE 0x001000
#define SCE_CTRL_CIRCLE   0x002000
#define SCE_CTRL_CROSS    0x004000
#define SCE_CTRL_SQUARE   0x008000
#define SCE_CTRL_MODE     0x000000   // not a real button — always 0

typedef struct {
    unsigned int  buttons;
    unsigned char lx, ly, rx, ry;   // 0..255, center 128, Y down-positive (Vita)
} SceCtrlData;

#define SCE_CTRL_MODE_ANALOG_WIDE 0
#define sceCtrlSetSamplingModeExt(m) ((void)0)

static PadState g_switchPad;
static int      g_switchPadInit = 0;

static inline unsigned char switch_axis_to_vita(s32 v, int invert) {
    // libnx: -32767..32767, +Y up. Vita: 0..255, 128 center, +Y DOWN.
    if (invert) v = -v;
    int c = 128 + (v * 127) / 32767;
    if (c < 0) c = 0; else if (c > 255) c = 255;
    return (unsigned char)c;
}

// Positional face-button mapping (Vita gloss on Switch layout):
// CROSS = bottom (B), CIRCLE = right (A), SQUARE = left (Y), TRIANGLE = top (X).
static inline int sceCtrlPeekBufferPositiveExt2(int port, SceCtrlData* pad, int count) {
    (void)port; (void)count;
    if (!g_switchPadInit) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_switchPad);
        g_switchPadInit = 1;
    }
    padUpdate(&g_switchPad);
    u64 b = padGetButtons(&g_switchPad);
    unsigned int v = 0;
    if (b & HidNpadButton_B)       v |= SCE_CTRL_CROSS;
    if (b & HidNpadButton_A)       v |= SCE_CTRL_CIRCLE;
    if (b & HidNpadButton_Y)       v |= SCE_CTRL_SQUARE;
    if (b & HidNpadButton_X)       v |= SCE_CTRL_TRIANGLE;
    if (b & HidNpadButton_L)       v |= SCE_CTRL_L1;
    if (b & HidNpadButton_R)       v |= SCE_CTRL_R1;
    if (b & HidNpadButton_ZL)      v |= SCE_CTRL_L2;
    if (b & HidNpadButton_ZR)      v |= SCE_CTRL_R2;
    if (b & HidNpadButton_StickL)  v |= SCE_CTRL_L3;
    if (b & HidNpadButton_StickR)  v |= SCE_CTRL_R3;
    if (b & HidNpadButton_Plus)    v |= SCE_CTRL_START;
    if (b & HidNpadButton_Minus)   v |= SCE_CTRL_SELECT;
    if (b & HidNpadButton_Up)      v |= SCE_CTRL_UP;
    if (b & HidNpadButton_Down)    v |= SCE_CTRL_DOWN;
    if (b & HidNpadButton_Left)    v |= SCE_CTRL_LEFT;
    if (b & HidNpadButton_Right)   v |= SCE_CTRL_RIGHT;
    pad->buttons = v;
    HidAnalogStickState ls = padGetStickPos(&g_switchPad, 0);
    HidAnalogStickState rs = padGetStickPos(&g_switchPad, 1);
    pad->lx = switch_axis_to_vita(ls.x, 0);
    pad->ly = switch_axis_to_vita(ls.y, 1);   // Vita Y is down-positive
    pad->rx = switch_axis_to_vita(rs.x, 0);
    pad->ry = switch_axis_to_vita(rs.y, 1);
    return 1;
}

// ---- scePower / vitaGL knobs -> no-ops / EGL --------------------------------
#define scePowerSetArmClockFrequency(x)     ((void)0)
#define scePowerSetBusClockFrequency(x)     ((void)0)
#define scePowerSetGpuClockFrequency(x)     ((void)0)
#define scePowerSetGpuXbarClockFrequency(x) ((void)0)
#define vglUseLowPrecision(x)               ((void)0)
#define vglWaitVblankStart(x)               ((void)0)   // EGL swap interval 1 is the default
#define sceKernelExitProcess(x)             ((void)0)   // fall through to main's return

static EGLDisplay s_eglDisplay;
static EGLContext s_eglContext;
static EGLSurface s_eglSurface;

static inline bool switch_gl_init(void) {
    s_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_eglDisplay) return false;
    eglInitialize(s_eglDisplay, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig config; EGLint n;
    static const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    eglChooseConfig(s_eglDisplay, attribs, &config, 1, &n);
    if (n == 0) return false;
    s_eglSurface = eglCreateWindowSurface(s_eglDisplay, config, nwindowGetDefault(), NULL);
    if (!s_eglSurface) return false;
    static const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
    s_eglContext = eglCreateContext(s_eglDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (!s_eglContext) return false;
    eglMakeCurrent(s_eglDisplay, s_eglSurface, s_eglSurface, s_eglContext);
    glViewport(0, 0, SWITCH_DISP_W, SWITCH_DISP_H);   // vitaGL defaulted this; EGL doesn't
    return true;
}
#define vglInit(sz) switch_gl_init()
#define vglSwapBuffers(x) eglSwapBuffers(s_eglDisplay, s_eglSurface)

// GLES1.1 fixed-function name gaps vs desktop GL 1.x (vitaGL).
#define glFrustum  glFrustumf
#define glOrtho    glOrthof
#define glColorMaterial(face, mode) ((void)0)   // ES1.1 is hardwired to AMBIENT_AND_DIFFUSE
