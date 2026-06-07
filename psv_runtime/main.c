// Affinity PS Vita runtime — bring-up.
// Step 1: validate the vitaGL graphics layer (the basis for the Mode 4 port)
// with a spinning 3D triangle. The scene renderer will be ported from
// psp_runtime/ onto vitaGL after this proves out.
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>

int main(void)
{
    vglInit(0x800000);                 // pool size for vitaGL
    vglWaitVblankStart(GL_TRUE);

    glClearColor(0.10f, 0.12f, 0.18f, 1.0f);
    glEnable(GL_DEPTH_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // 960x544; perspective via glFrustum (vitaGL has no GLU). 60 deg vertical FOV.
    {
        const float nearp = 0.1f, farp = 100.0f, aspect = 960.0f / 544.0f;
        const float top = nearp * 0.57735f;   // tan(30 deg)
        const float right = top * aspect;
        glFrustum(-right, right, -top, top, nearp, farp);
    }
    glMatrixMode(GL_MODELVIEW);

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    float ang = 0.0f;
    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -3.0f);
        glRotatef(ang, 0.0f, 1.0f, 0.0f);
        ang += 1.5f;

        glBegin(GL_TRIANGLES);
            glColor3f(1.0f, 0.2f, 0.2f); glVertex3f( 0.0f,  0.8f, 0.0f);
            glColor3f(0.2f, 1.0f, 0.2f); glVertex3f(-0.8f, -0.6f, 0.0f);
            glColor3f(0.2f, 0.4f, 1.0f); glVertex3f( 0.8f, -0.6f, 0.0f);
        glEnd();

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}
