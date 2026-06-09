// Affinity PS Vita runtime — Mode 4 scene bring-up.
// Renders the exported level meshes with vitaGL. Ported from psp_runtime's
// scene.c, but simplified for the Vita's headroom: no per-frame frustum
// culling / bucketing and no texture swizzling (just upload GL textures and
// draw each mesh's full index buffer). Free-orbit camera so the level can be
// inspected on-device. Rig / sprites / sky / audio / collision come next.
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "psv_mapdata.h"   // defines afn_meshes / afn_sprites / camera start
#include "psv_rig.h"       // player rig data (skinned glTF), if AFN_HAS_PLAYER_RIG
#include "psv_player.h"    // AFN_PLAYER_COL_* (custom collision box, if authored)
#include "psv_sky.h"       // sky panorama texture (AFN_HAS_SKY)
#include "psv_sprites.h"   // billboard sprites (AFN_HAS_SPRITES)
#include "psv_rail.h"      // grind rail centerlines (AFN_HAS_RAIL_PATH)
#include "psv_hud.h"       // 2D HUD overlay elements/pieces/text (AFN_HAS_HUD)

#define SCR_W 960.0f
#define SCR_H 544.0f
#define DEG2RAD (3.14159265f / 180.0f)

static GLuint s_meshTex[256];   // one GL texture per mesh (0 = none)

// ---------------------------------------------------------------------------
// Rigs: CPU rigid skinning (ported from psp_runtime/rig.c) + vitaGL draw.
// Multi-rig: the scene can carry several distinct models (player + each NPC /
// enemy type). afn_rigs[] holds one AfnRig descriptor per used model; each
// instance (the player, and afn_npc_inst[]) names a rig slot, position, facing,
// scale and clip, and is CPU-skinned into a shared buffer then drawn.
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_PLAYER_RIG
#ifndef AFN_RIG_YAW_OFFSET
#define AFN_RIG_YAW_OFFSET 0.0f
#endif
int afn_rig_clip = AFN_PLAYER_DEFAULT_CLIP;   // player clip selector (script-set; local for bring-up)
// Script-glue globals rigs_render reads (defined later in the script-glue block).
extern int afn_skel_anim_obj, afn_skel_anim_clip;   // SetSkelAnim: NPC sprite idx + clip
extern unsigned char afn_sprite_visible[];          // SetVisible/DestroyObject per sprite
static AfnRigVertex s_skinned[AFN_RIG_MAX_VERTS];
static float  s_bonemat[AFN_RIG_MAX_BONES][12];   // 3x4 row-major per bone
static GLuint s_rigTex[AFN_RIG_COUNT][AFN_RIG_MAX_MATS];
static float  s_pframe = 0.0f;                     // player anim frame
static int    s_pclip  = AFN_PLAYER_DEFAULT_CLIP;
static int    s_npcClip[AFN_NPC_COUNT + 1];        // per-NPC clip override (-1 = use default)
static float  s_npcFrame[AFN_NPC_COUNT + 1];       // per-NPC anim frame (+1 avoids zero-size)
static float  s_npcY[AFN_NPC_COUNT + 1];           // gravity-driven dynamic Y (NDS s_npc_y parity)
static float  s_npcVY[AFN_NPC_COUNT + 1];          // vertical velocity
static unsigned char s_npcGround[AFN_NPC_COUNT + 1];
extern int afn_gravity, afn_terminal_vel;          // SetGravity/SetMaxFall (8.8 fixed, shared with player)

static void pose_to_mat(const float* p, float* m) {
    float px=p[0],py=p[1],pz=p[2], w=p[3],x=p[4],y=p[5],z=p[6];
    float n = w*w+x*x+y*y+z*z;
    if (n > 1e-8f) { n = 1.0f/sqrtf(n); w*=n; x*=n; y*=n; z*=n; }
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=1-2*(yy+zz); m[1]=2*(xy-wz);   m[2]=2*(xz+wy);   m[3]=px;
    m[4]=2*(xy+wz);   m[5]=1-2*(xx+zz); m[6]=2*(yz-wx);   m[7]=py;
    m[8]=2*(xz-wy);   m[9]=2*(yz+wx);   m[10]=1-2*(xx+yy);m[11]=pz;
}
static void build_bone_mats(const AfnRig* R, int clip, float frame) {
    const float* cd = R->clip[clip];
    int nf = R->clipframes[clip]; if (nf < 1) nf = 1;
    int f0 = (int)frame; if (f0 < 0) f0 = 0; if (f0 >= nf) f0 = nf - 1;
    int f1 = f0 + 1; if (f1 >= nf) f1 = R->cliploop[clip] ? 0 : nf - 1;
    float t = frame - (float)((int)frame);
    for (int b = 0; b < R->bones; b++) {
        const float* p0 = &cd[(f0 * R->bones + b) * 7];
        const float* p1 = &cd[(f1 * R->bones + b) * 7];
        float p[7];
        p[0]=p0[0]+(p1[0]-p0[0])*t; p[1]=p0[1]+(p1[1]-p0[1])*t; p[2]=p0[2]+(p1[2]-p0[2])*t;
        float d = p0[3]*p1[3]+p0[4]*p1[4]+p0[5]*p1[5]+p0[6]*p1[6];
        float s = (d < 0.0f) ? -1.0f : 1.0f;
        p[3]=p0[3]+(p1[3]*s-p0[3])*t; p[4]=p0[4]+(p1[4]*s-p0[4])*t;
        p[5]=p0[5]+(p1[5]*s-p0[5])*t; p[6]=p0[6]+(p1[6]*s-p0[6])*t;
        pose_to_mat(p, s_bonemat[b]);
    }
}
// Skin rig R into s_skinned (positions + normals + uv + white vertex color).
static void skin(const AfnRig* R) {
    for (int v = 0; v < R->verts; v++) {
        const float* m = s_bonemat[R->vbone[v]];
        float x=R->vpos[v*3+0], y=R->vpos[v*3+1], z=R->vpos[v*3+2];
        s_skinned[v].x = m[0]*x+m[1]*y+m[2]*z+m[3];
        s_skinned[v].y = m[4]*x+m[5]*y+m[6]*z+m[7];
        s_skinned[v].z = m[8]*x+m[9]*y+m[10]*z+m[11];
        float nx=R->vnorm[v*3+0], ny=R->vnorm[v*3+1], nz=R->vnorm[v*3+2];
        float wx=m[0]*nx+m[1]*ny+m[2]*nz, wy=m[4]*nx+m[5]*ny+m[6]*nz, wz=m[8]*nx+m[9]*ny+m[10]*nz;
        float nl=wx*wx+wy*wy+wz*wz; if (nl>1e-12f){nl=1.0f/sqrtf(nl);wx*=nl;wy*=nl;wz*=nl;}
        s_skinned[v].nx=wx; s_skinned[v].ny=wy; s_skinned[v].nz=wz;
        s_skinned[v].u = R->vuv[v*2+0]; s_skinned[v].v = R->vuv[v*2+1];
        s_skinned[v].color = 0xFFFFFFFF;
    }
}
static void rig_init(void) {
    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        s_npcClip[i] = -1;                       // no SetSkelAnim override yet
        s_npcY[i]  = afn_npc_inst[i][1];         // start at the authored Y; gravity settles it
        s_npcVY[i] = 0.0f; s_npcGround[i] = 0;
    }
    for (int r = 0; r < AFN_RIG_COUNT; r++) {
        const AfnRig* R = &afn_rigs[r];
        for (int g = 0; g < AFN_RIG_MAX_MATS; g++) {
            s_rigTex[r][g] = 0;
            if (g < R->mats && R->tex[g] && R->texw[g] > 0 && R->texh[g] > 0) {
                glGenTextures(1, &s_rigTex[r][g]);
                glBindTexture(GL_TEXTURE_2D, s_rigTex[r][g]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, R->texw[g], R->texh[g], 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, R->tex[g]);
            }
        }
    }
}
// Advance an animation frame for rig R's clip.
static float rig_advance(const AfnRig* R, int clip, float frame) {
    frame += 0.4f;
    int nf = R->clipframes[clip];
    if (nf > 1) {
        if (R->cliploop[clip]) { while (frame >= (float)nf) frame -= (float)nf; }
        else if (frame > (float)(nf-1)) frame = (float)(nf-1);
    } else frame = 0.0f;
    return frame;
}
// Draw one rig instance. s_skinned must already hold R's skinned pose. The
// headlamp is the same NDS-matched setup as before, per rig's baked direction.
static void rig_draw(const AfnRig* R, GLuint* texArr, const float* view,
                     float px, float py, float pz, float yawDeg, float instScale,
                     const float* upN) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    if (R->camlight) {
        GLfloat ldir[4]   = { -R->ldx, -R->ldy, -R->ldz, 0.0f };  // GL_POSITION = toward light (negated DS dir)
        GLfloat white[4]  = { 1, 1, 1, 1 };
        GLfloat matAmb[4] = { 8.0f/31.0f,  8.0f/31.0f,  8.0f/31.0f,  1 };
        GLfloat matDif[4] = { 28.0f/31.0f, 28.0f/31.0f, 28.0f/31.0f, 1 };
        GLfloat noAmb[4]  = { 0, 0, 0, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, ldir);
        glLightfv(GL_LIGHT0, GL_DIFFUSE,  white);
        glLightfv(GL_LIGHT0, GL_AMBIENT,  white);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, noAmb);
        glDisable(GL_COLOR_MATERIAL);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, matAmb);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, matDif);
        glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_NORMALIZE);
    } else {
        glDisable(GL_LIGHTING);
    }

    // Orient with a basis matrix: up = floor normal (slope tilt), forward = the
    // yaw heading projected onto the slope plane, right = up x fwd. Ported from
    // psp_runtime/rig.c. NPCs pass world-up so they stay vertical.
    float ux = upN ? upN[0] : 0.0f, uy = upN ? upN[1] : 1.0f, uz = upN ? upN[2] : 0.0f;
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); if (ul > 1e-6f) { ux/=ul; uy/=ul; uz/=ul; }
    float yr = yawDeg * DEG2RAD + AFN_RIG_YAW_OFFSET;
    float ydx = sinf(yr), ydz = cosf(yr);
    float d = ydx*ux + ydz*uz;                              // yawDir . up (ydy = 0)
    float fx = ydx - ux*d, fy = -uy*d, fz = ydz - uz*d;     // project onto slope plane
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-6f) { fx/=fl; fy/=fl; fz/=fl; } else { fx=0; fy=0; fz=1; }
    float rgx = uy*fz - uz*fy, rgy = uz*fx - ux*fz, rgz = ux*fy - uy*fx;  // right = up x fwd
    float S = R->scale * instScale;
    float model[16] = {     // column-major: col0=right, col1=up, col2=forward
        rgx*S, rgy*S, rgz*S, 0,
        ux*S,  uy*S,  uz*S,  0,
        fx*S,  fy*S,  fz*S,  0,
        px,    py,    pz,    1
    };
    glLoadMatrixf(view);
    glMultMatrixf(model);

    glDisable(GL_BLEND);
    if (R->cull == 2) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(R->cull == 1 ? GL_CW : GL_CCW); }

    AfnRigVertex* v = s_skinned;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnRigVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnRigVertex), &v->color);
    glNormalPointer (   GL_FLOAT,         sizeof(AfnRigVertex), &v->nx);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnRigVertex), &v->x);
    for (int g = 0; g < R->mats; g++) {
        int ic = R->idxcount[g];
        if (ic <= 0) continue;
        if (texArr[g]) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texArr[g]); }
        else glDisable(GL_TEXTURE_2D);
        glDrawElements(GL_TRIANGLES, ic, GL_UNSIGNED_SHORT, R->idx[g]);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    if (R->camlight) { glDisable(GL_LIGHTING); glDisable(GL_LIGHT0); glDisable(GL_NORMALIZE); }
}

// Skin + draw the player and every NPC for this frame.
static void rigs_render(const float* view, float playerX, float playerY, float playerZ,
                        float playerYaw, const float* floorN) {
    const AfnRig* PR = &afn_rigs[AFN_PLAYER_RIG_SLOT];
    if (afn_rig_clip >= 0 && afn_rig_clip < PR->clips && afn_rig_clip != s_pclip) {
        s_pclip = afn_rig_clip; s_pframe = 0.0f;
    }
    s_pframe = rig_advance(PR, s_pclip, s_pframe);
    build_bone_mats(PR, s_pclip, s_pframe); skin(PR);
    rig_draw(PR, s_rigTex[AFN_PLAYER_RIG_SLOT], view, playerX, playerY, playerZ, playerYaw, AFN_PLAYER_SCALE, floorN);

    // SetSkelAnim: set the matching NPC's clip override (by editor sprite index).
    // Needs the 8-wide afn_npc_inst (editor index in col 7) from the new export.
#ifdef AFN_HAS_SPRITE_IDX
    if (afn_skel_anim_obj >= 0) {
        for (int i = 0; i < AFN_NPC_COUNT; i++)
            if ((int)afn_npc_inst[i][7] == afn_skel_anim_obj) s_npcClip[i] = afn_skel_anim_clip;
        afn_skel_anim_obj = -1;
    }
#endif
    for (int i = 0; i < AFN_NPC_COUNT; i++) {
        const float* N = afn_npc_inst[i];
        int slot = (int)N[6]; if (slot < 0 || slot >= AFN_RIG_COUNT) continue;
        const AfnRig* R = &afn_rigs[slot];
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = (int)N[7];
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;   // hidden / destroyed
#endif
        int clip = s_npcClip[i] >= 0 ? s_npcClip[i] : (int)N[5];   // SetSkelAnim override
        if (clip < 0 || clip >= R->clips) clip = 0;
        s_npcFrame[i] = rig_advance(R, clip, s_npcFrame[i]);
        build_bone_mats(R, clip, s_npcFrame[i]); skin(R);
        // Draw at the gravity-settled Y (npc_physics updates s_npcY each frame).
        rig_draw(R, s_rigTex[slot], view, N[0], s_npcY[i], N[2], N[3], N[4], 0);  // NPCs: world up
    }
}
#endif // AFN_HAS_PLAYER_RIG

// Build a column-major 4x4 view matrix (gluLookAt equivalent — vitaGL has no GLU).
static void look_at(float m[16],
                    float ex, float ey, float ez,
                    float cx, float cy, float cz,
                    float ux, float uy, float uz)
{
    float fx = cx-ex, fy = cy-ey, fz = cz-ez;
    float fl = sqrtf(fx*fx+fy*fy+fz*fz); if (fl < 1e-6f) fl = 1.0f;
    fx/=fl; fy/=fl; fz/=fl;
    // s = f x up
    float sx = fy*uz - fz*uy, sy = fz*ux - fx*uz, sz = fx*uy - fy*ux;
    float sl = sqrtf(sx*sx+sy*sy+sz*sz); if (sl < 1e-6f) sl = 1.0f;
    sx/=sl; sy/=sl; sz/=sl;
    // u = s x f
    float ux2 = sy*fz - sz*fy, uy2 = sz*fx - sx*fz, uz2 = sx*fy - sy*fx;
    m[0]=sx;  m[4]=sy;  m[8]=sz;   m[12] = -(sx*ex + sy*ey + sz*ez);
    m[1]=ux2; m[5]=uy2; m[9]=uz2;  m[13] = -(ux2*ex + uy2*ey + uz2*ez);
    m[2]=-fx; m[6]=-fy; m[10]=-fz; m[14] =  (fx*ex + fy*ey + fz*ez);
    m[3]=0;   m[7]=0;   m[11]=0;   m[15] = 1.0f;
}

static void upload_textures(void)
{
    for (int mi = 0; mi < afn_mesh_count && mi < 256; mi++) {
        s_meshTex[mi] = 0;
        const AfnMesh* m = &afn_meshes[mi];
        if (m->textured && m->texPixels && m->texW > 0 && m->texH > 0) {
            glGenTextures(1, &s_meshTex[mi]);
            glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            // texPixels are 0xAABBGGRR == RGBA byte order in memory.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m->texW, m->texH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, m->texPixels);
        }
    }
}

static void draw_mesh(int mi)
{
    const AfnMesh* m = &afn_meshes[mi];
    if (!m->visible || m->vertCount <= 0 || !m->verts || !m->indices) return;

    // Render level meshes TWO-SIDED. Single-sided meshes (a slope, a ramp) get
    // back-face culled when you orbit around to their back, so they vanish — the
    // disappearing-slope bug. The Vita GPU has the overdraw headroom and the depth
    // buffer sorts everything, so there's no reason to cull; draw both faces. The
    // exporter's cullMode (back/front/none) is intentionally ignored here.
    glDisable(GL_CULL_FACE);

    if (m->textured && s_meshTex[mi]) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, s_meshTex[mi]);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    if (m->texHasAlpha) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
    else glDisable(GL_BLEND);

    const AfnVertex* v = m->verts;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,        sizeof(AfnVertex), &v->x);
    glDrawElements(GL_TRIANGLES, m->indexCount, GL_UNSIGNED_SHORT, m->indices);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// ---------------------------------------------------------------------------
// Sky panorama + sprite billboards (ported from psp_runtime/sky.c + billboard.c)
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_SKY
static GLuint s_skyTex = 0;
static void sky_init(void) {
    glGenTextures(1, &s_skyTex);
    glBindTexture(GL_TEXTURE_2D, s_skyTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, AFN_SKY_W, AFN_SKY_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, afn_sky_tex);
}
// Far textured quad in eye space (identity modelview) so it tracks the camera;
// U scrolls with yaw for the 360 wrap. No depth writes -> scene draws on top.
static void sky_render(float camAngle) {
    float u = camAngle / (2.0f * 3.14159265f);
    // Inside far=1500; X/Y keep the same angular coverage (ratios to D) as before.
    const float D = 1450.0f, X = 2088.0f, Y = 1218.0f;
    AfnVertex sky[4] = {
        { u,      0.0f, 0xFFFFFFFFu, -X,  Y, -D },
        { u+1.0f, 0.0f, 0xFFFFFFFFu,  X,  Y, -D },
        { u+1.0f, 1.0f, 0xFFFFFFFFu,  X, -Y, -D },
        { u,      1.0f, 0xFFFFFFFFu, -X, -Y, -D },
    };
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);  glDisable(GL_BLEND); glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);  glBindTexture(GL_TEXTURE_2D, s_skyTex);
    AfnVertex* v = sky;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &v->u);
    glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
}
#endif // AFN_HAS_SKY

#ifdef AFN_HAS_SPRITES
static float  s_sprFrame[AFN_SPR_INST_COUNT];
static GLuint s_sprTex[sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0])];
static void billboards_init(void) {
    int nf = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
    for (int f = 0; f < nf; f++) {
        glGenTextures(1, &s_sprTex[f]);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[f]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);   // crisp pixel-art sprites
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, afn_spr_frame_w[f], afn_spr_frame_h[f], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, afn_spr_frame_ptrs[f]);
    }
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) s_sprFrame[i] = (float)afn_spr_fstart[i];
}
// Camera-facing (Y-axis) textured quads in world space, drawn through the view.
// camEyeX/Z is the camera world position, used to pick an 8-facing direction for
// directional sprites (N,NE,E,SE,S,SW,W,NW = dir 0..7).
static void billboards_render(const float* view, float camAngle, float camEyeX, float camEyeZ) {
    float rx = cosf(camAngle), rz = -sinf(camAngle);   // camera right in XZ
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    for (int i = 0; i < AFN_SPR_INST_COUNT; i++) {
#ifdef AFN_HAS_SPRITE_IDX
        int eidx = afn_spr_editor_idx[i];
        if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
        int NF = (int)(sizeof(afn_spr_frame_ptrs)/sizeof(afn_spr_frame_ptrs[0]));
        float sz = afn_spr_basesize[i] * afn_spr_scale[i] * 0.25f, hw = sz * 0.5f;
        float px = afn_spr_x[i], py = afn_spr_y[i], pz = afn_spr_z[i];
        int cf;
        if (afn_spr_directional[i]) {
            // 8-facing: pick the art for the direction the camera views from.
            // bearing 0 = camera at +Z (south) -> show S(4); +X(east) -> E(2).
            float bearing = atan2f(camEyeX - px, camEyeZ - pz);
            int n = (int)lroundf(bearing / (3.14159265f / 4.0f));
            int dir = (4 - n) & 7;
            cf = afn_spr_dir_base[i] + dir;
        } else {
            int lo = afn_spr_fstart[i], hi = afn_spr_fend[i]; if (hi < lo) hi = lo;
            if (hi > NF-1) hi = NF-1; if (lo > NF-1) lo = NF-1; if (lo < 0) lo = 0;  // never index past the table
            s_sprFrame[i] += afn_spr_fps[i] / 60.0f;
            if (s_sprFrame[i] >= (float)(hi+1)) s_sprFrame[i] = (float)lo;
            cf = (int)s_sprFrame[i]; if (cf < lo) cf = lo; if (cf > hi) cf = hi;
        }
        if (cf < 0) cf = 0; if (cf > NF-1) cf = NF-1;
        float lx = px - rx*hw, lz = pz - rz*hw, Rx = px + rx*hw, Rz = pz + rz*hw;
        float top = py + sz, bot = py;
        AfnVertex q[4] = {
            { 0,0, 0xFFFFFFFFu, lx, top, lz },
            { 1,0, 0xFFFFFFFFu, Rx, top, Rz },
            { 1,1, 0xFFFFFFFFu, Rx, bot, Rz },
            { 0,1, 0xFFFFFFFFu, lx, bot, lz },
        };
        glTexCoordPointer(2, GL_FLOAT,        sizeof(AfnVertex), &q->u);
        glColorPointer  (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &q->color);
        glVertexPointer (3, GL_FLOAT,         sizeof(AfnVertex), &q->x);
        glBindTexture(GL_TEXTURE_2D, s_sprTex[cf]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_CULL_FACE);
}
#endif // AFN_HAS_SPRITES

// ---------------------------------------------------------------------------
// Mesh collision (floor/wall) — float port of psp_runtime/collision.c. Faces are
// built once from the exported mesh geometry, transformed to world space, and
// bucketed into an XZ grid for cheap per-cell floor/wall queries.
// ---------------------------------------------------------------------------
#define COL_GN     16
#define COL_NCELL  (COL_GN * COL_GN)
typedef struct {
    float ax, ay, az, bx, by, bz, cx, cy, cz;
    float nx, ny, nz;
    int   flags;   // 1 floor, 2 ceiling, 4 wall
    int   sprite;  // editor sprite index of the source instance (-1 = none)
} ColFace;
int afn_floor_sprite = -1;   // editor sprite index of the floor the player stands on (grind)
static ColFace* s_faces = 0;
static int      s_faceCount = 0;
static int*     s_cellStart = 0;
static int*     s_cellCount = 0;
static int*     s_cellFaces = 0;
static float    s_minX, s_minZ, s_cellSize;
static unsigned* s_faceStamp = 0;
static unsigned  s_queryStamp = 0;

static int cell_x(float x) { int c=(int)((x-s_minX)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }
static int cell_z(float z) { int c=(int)((z-s_minZ)/s_cellSize); return c<0?0:(c>=COL_GN?COL_GN-1:c); }

static void collide_build(void) {
    int total = 0;
    for (int si = 0; si < afn_sprite_count; si++) {
        int mi = afn_sprites[si].meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        total += afn_meshes[mi].indexCount / 3;
    }
    if (total <= 0) return;
    s_faces = (ColFace*)malloc(sizeof(ColFace) * total);
    if (!s_faces) return;
    float mnx=1e30f, mnz=1e30f, mxx=-1e30f, mxz=-1e30f;
    for (int si = 0; si < afn_sprite_count; si++) {
        const AfnSpriteInst* sp = &afn_sprites[si];
        int mi = sp->meshIdx;
        if (mi < 0 || mi >= afn_mesh_count) continue;
        const AfnMesh* m = &afn_meshes[mi];
        const AfnVertex* V = m->verts;
        const unsigned short* I = m->indices;
        float ry=sp->rotY*DEG2RAD, rx=sp->rotX*DEG2RAD, rz=sp->rotZ*DEG2RAD;
        float cY=cosf(ry),sY=sinf(ry),cX=cosf(rx),sX=sinf(rx),cZ=cosf(rz),sZ=sinf(rz);
        float scl=sp->scale;
        for (int t = 0; t + 3 <= m->indexCount; t += 3) {
            float wp[9];
            for (int k=0;k<3;k++){
                const AfnVertex* vv=&V[I[t+k]];
                float lx=vv->x*scl, ly=vv->y*scl, lz=vv->z*scl;
                float ax= lx*cY+lz*sY, az=-lx*sY+lz*cY, ay=ly;
                float ay2=ay*cX-az*sX, az2=ay*sX+az*cX;
                float ax2=ax*cZ-ay2*sZ, ay3=ax*sZ+ay2*cZ;
                wp[k*3+0]=sp->x+ax2; wp[k*3+1]=sp->y+ay3; wp[k*3+2]=sp->z+az2;
            }
            float e1x=wp[3]-wp[0],e1y=wp[4]-wp[1],e1z=wp[5]-wp[2];
            float e2x=wp[6]-wp[0],e2y=wp[7]-wp[1],e2z=wp[8]-wp[2];
            float nx=e1y*e2z-e1z*e2y, ny=e1z*e2x-e1x*e2z, nz=e1x*e2y-e1y*e2x;
            float len=sqrtf(nx*nx+ny*ny+nz*nz); if (len<1e-6f) continue;
            nx/=len; ny/=len; nz/=len;
            ColFace* F=&s_faces[s_faceCount++];
            F->ax=wp[0];F->ay=wp[1];F->az=wp[2]; F->bx=wp[3];F->by=wp[4];F->bz=wp[5]; F->cx=wp[6];F->cy=wp[7];F->cz=wp[8];
            F->flags=(ny>0.3f)?1:(ny<-0.7f)?2:4;
            F->nx=nx; F->ny=ny; F->nz=nz;
#ifdef AFN_HAS_SPRITE_IDX
            F->sprite = afn_mesh_inst_sprite[si];   // for afn_floor_sprite (grind rail detect)
#else
            F->sprite = -1;
#endif
            for (int k=0;k<3;k++){ float X=wp[k*3],Z=wp[k*3+2]; if(X<mnx)mnx=X; if(X>mxx)mxx=X; if(Z<mnz)mnz=Z; if(Z>mxz)mxz=Z; }
        }
    }
    if (s_faceCount<=0) return;
    s_minX=mnx; s_minZ=mnz;
    float span=(mxx-mnx)>(mxz-mnz)?(mxx-mnx):(mxz-mnz);
    s_cellSize=span/COL_GN; if (s_cellSize<1.0f) s_cellSize=1.0f;
    s_cellStart=(int*)calloc(COL_NCELL,sizeof(int));
    s_cellCount=(int*)calloc(COL_NCELL,sizeof(int));
    s_faceStamp=(unsigned*)calloc(s_faceCount,sizeof(unsigned));
    if (!s_cellStart||!s_cellCount||!s_faceStamp) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++) s_cellCount[gz*COL_GN+gx]++;
    }
    int te=0; for (int c=0;c<COL_NCELL;c++){ s_cellStart[c]=te; te+=s_cellCount[c]; s_cellCount[c]=0; }
    s_cellFaces=(int*)malloc(sizeof(int)*(te>0?te:1)); if (!s_cellFaces) return;
    for (int i=0;i<s_faceCount;i++){
        const ColFace* F=&s_faces[i];
        float mnX=fminf(F->ax,fminf(F->bx,F->cx)), mxX=fmaxf(F->ax,fmaxf(F->bx,F->cx));
        float mnZ=fminf(F->az,fminf(F->bz,F->cz)), mxZ=fmaxf(F->az,fmaxf(F->bz,F->cz));
        for (int gz=cell_z(mnZ);gz<=cell_z(mxZ);gz++) for (int gx=cell_x(mnX);gx<=cell_x(mxX);gx++){
            int c=gz*COL_GN+gx; s_cellFaces[s_cellStart[c]+s_cellCount[c]++]=i; }
    }
}

#ifdef AFN_HAS_PLAYER_COL
#define COL_RADIUS AFN_PLAYER_COL_RADIUS
#define COL_BOTTOM AFN_PLAYER_COL_BOTTOM
#define COL_TOP    AFN_PLAYER_COL_TOP
#else
#define COL_RADIUS 6.0f
#define COL_BOTTOM 0.0f
#define COL_TOP    24.0f
#endif
#define WALL_TOP_TOL 5.0f

static int collide_floor(float x, float z, float py, float* outY, float* outN) {
    if (!s_cellFaces) return 0;
    int c=cell_z(z)*COL_GN+cell_x(x);
    int start=s_cellStart[c], count=s_cellCount[c];
    float bestY=0; int found=0; const ColFace* bestF=0;
    for (int i=0;i<count;i++){
        const ColFace* F=&s_faces[s_cellFaces[start+i]];
        if (!(F->flags&1)) continue;
        float c0=(F->bx-F->ax)*(z-F->az)-(F->bz-F->az)*(x-F->ax);
        float c1=(F->cx-F->bx)*(z-F->bz)-(F->cz-F->bz)*(x-F->bx);
        float c2=(F->ax-F->cx)*(z-F->cz)-(F->az-F->cz)*(x-F->cx);
        if (!((c0>=0&&c1>=0&&c2>=0)||(c0<=0&&c1<=0&&c2<=0))) continue;
        float cs=c0+c1+c2;
        float fy=(cs==0)?(F->ay+F->by+F->cy)/3.0f:(c1*F->ay+c2*F->by+c0*F->cy)/cs;
        if (fy>py+COL_TOP) continue;
        if (!found||fy>bestY){ bestY=fy; found=1; bestF=F; }
    }
    *outY=bestY;
    if (outN){ if(bestF){outN[0]=bestF->nx;outN[1]=bestF->ny;outN[2]=bestF->nz;} else {outN[0]=0;outN[1]=1;outN[2]=0;} }
    afn_floor_sprite = bestF ? bestF->sprite : -1;   // which sprite's floor (grind rail detect)
    return found;
}

static void collide_walls(float* x, float* z, float py) {
    if (!s_cellFaces||!s_faceStamp) return;
    float ppx=*x, ppz=*z;
    int gx0=cell_x(ppx-COL_RADIUS), gx1=cell_x(ppx+COL_RADIUS);
    int gz0=cell_z(ppz-COL_RADIUS), gz1=cell_z(ppz+COL_RADIUS);
    unsigned stamp=++s_queryStamp;
    for (int gz=gz0;gz<=gz1;gz++) for (int gx=gx0;gx<=gx1;gx++){
        int c=gz*COL_GN+gx; int start=s_cellStart[c], count=s_cellCount[c];
        for (int i=0;i<count;i++){
            int fi=s_cellFaces[start+i];
            if (s_faceStamp[fi]==stamp) continue; s_faceStamp[fi]=stamp;
            const ColFace* F=&s_faces[fi];
            if (!(F->flags&4)) continue;
            float fMinY=fminf(F->ay,fminf(F->by,F->cy)), fMaxY=fmaxf(F->ay,fmaxf(F->by,F->cy));
            if (py+COL_TOP<fMinY || py+COL_BOTTOM>=fMaxY-WALL_TOP_TOL) continue;
            if (F->nx*F->nx+F->nz*F->nz<1e-8f) continue;
            float vx[3]={F->ax,F->bx,F->cx}, vz[3]={F->az,F->bz,F->cz};
            float bestPx=ppx,bestPz=ppz,bestD2=1e30f;
            for (int e=0;e<3;e++){
                float x0=vx[e],z0=vz[e], sx=vx[(e+1)%3]-x0, sz=vz[(e+1)%3]-z0;
                float L2=sx*sx+sz*sz;
                float t=(L2>1e-8f)?((ppx-x0)*sx+(ppz-z0)*sz)/L2:0.0f;
                if (t<0)t=0; else if (t>1)t=1;
                float Px=x0+sx*t, Pz=z0+sz*t;
                float dx=ppx-Px, dz=ppz-Pz, d2=dx*dx+dz*dz;
                if (d2<bestD2){ bestD2=d2; bestPx=Px; bestPz=Pz; }
            }
            if (bestD2>=COL_RADIUS*COL_RADIUS) continue;
            float d=sqrtf(bestD2), push=COL_RADIUS-d;
            if (d>1e-4f){ ppx+=(ppx-bestPx)/d*push; ppz+=(ppz-bestPz)/d*push; }
            else { float xl=sqrtf(F->nx*F->nx+F->nz*F->nz); ppx+=F->nx/xl*push; ppz+=F->nz/xl*push; }
        }
    }
    *x=ppx; *z=ppz;
}

// ---------------------------------------------------------------------------
// Grind-rail path helpers — float port of fps3d.c afn_railpath_*. afn_rail_pts is
// world-px float, arc-length parameterized. (psv_rail.h, AFN_HAS_RAIL_PATH.)
// ---------------------------------------------------------------------------
#ifdef AFN_HAS_RAIL_PATH
static float rail_seg_len(int start, int i) {
    float dx = afn_rail_pts[start+i][0]-afn_rail_pts[start+i-1][0];
    float dz = afn_rail_pts[start+i][2]-afn_rail_pts[start+i-1][2];
    return sqrtf(dx*dx+dz*dz);
}
static float rail_len(int rail) {
    int n = afn_rail_count[rail]; if (n < 2) return 0;
    int start = afn_rail_start[rail];
    float total = 0;
    for (int i = 1; i < n; i++) total += rail_seg_len(start, i);
    return total;
}
static void rail_sample(int rail, float s, float* ox, float* oy, float* oz, float* tdx, float* tdz) {
    int n = afn_rail_count[rail], start = afn_rail_start[rail];
    if (n < 2) { *ox=*oy=*oz=0; *tdx=1; *tdz=0; return; }
    if (s < 0) s = 0;
    float acc = 0;
    for (int i = 1; i < n; i++) {
        float ax=afn_rail_pts[start+i-1][0],ay=afn_rail_pts[start+i-1][1],az=afn_rail_pts[start+i-1][2];
        float bx=afn_rail_pts[start+i][0],by=afn_rail_pts[start+i][1],bz=afn_rail_pts[start+i][2];
        float seg = rail_seg_len(start, i); if (seg < 0.0001f) seg = 0.0001f;
        if (s <= acc+seg || i == n-1) {
            float t = (s - acc)/seg; if (t<0) t=0; if (t>1) t=1;
            *ox = ax+(bx-ax)*t; *oy = ay+(by-ay)*t; *oz = az+(bz-az)*t;
            float dx=bx-ax, dz=bz-az, l=sqrtf(dx*dx+dz*dz); if (l<0.0001f) l=0.0001f;
            *tdx = dx/l; *tdz = dz/l; return;
        }
        acc += seg;
    }
}
static float rail_nearest(int rail, float px, float pz, float* outD2) {
    int n = afn_rail_count[rail], start = afn_rail_start[rail];
    if (n < 2) { if(outD2)*outD2=0; return 0; }
    float bestD=1e30f, bestArc=0, acc=0;
    for (int i = 1; i < n; i++) {
        float ax=afn_rail_pts[start+i-1][0],az=afn_rail_pts[start+i-1][2];
        float bx=afn_rail_pts[start+i][0],bz=afn_rail_pts[start+i][2];
        float ex=bx-ax,ez=bz-az, el2=ex*ex+ez*ez; if(el2<0.0001f)el2=0.0001f;
        float t=((px-ax)*ex+(pz-az)*ez)/el2; if(t<0)t=0; if(t>1)t=1;
        float cx=ax+ex*t, cz=az+ez*t, dd=(px-cx)*(px-cx)+(pz-cz)*(pz-cz);
        float seg=rail_seg_len(start,i);
        if (dd<bestD){ bestD=dd; bestArc=acc+seg*t; }
        acc += seg;
    }
    if(outD2)*outD2=bestD;
    return bestArc;
}
#endif // AFN_HAS_RAIL_PATH

// ---------------------------------------------------------------------------
// Input + node-script variable layer. Behaviour is node-driven, per the engine
// convention: the emitted script (psv_script.h) sets these each frame and the
// movement/camera/rig below only READ them. input_update() supplies the raw
// analog/button defaults so the game is playable before/without a script
// (mirrors psp_runtime input.c + script_glue.c).
// ---------------------------------------------------------------------------
int afn_input_fwd = 0, afn_input_right = 0;   // camera-space move intent (256 = full)
int afn_move_speed = 0, afn_speed_prio = 0;   // node-set speed (0 = use walk default)
int afn_player_frozen = 0;
int orbit_angle = 0;                          // camera yaw, brad (65536 = full circle)
int orbit_pitch = 0;                          // camera pitch, brad (node OrbitCamera Up/Down + right stick)

enum { KEY_A=1,KEY_B=2,KEY_X=4,KEY_Y=8,KEY_L=16,KEY_R=32,KEY_START=64,KEY_SELECT=128,
       KEY_UP=256,KEY_DOWN=512,KEY_LEFT=1024,KEY_RIGHT=2048,
       // Analog directions, reported separately from the d-pad so a node can bind
       // each stick independently (left stick != d-pad).
       KEY_LSTICK_UP=4096,    KEY_LSTICK_DOWN=8192,    KEY_LSTICK_LEFT=16384,   KEY_LSTICK_RIGHT=32768,
       KEY_RSTICK_UP=65536,   KEY_RSTICK_DOWN=131072,  KEY_RSTICK_LEFT=262144,  KEY_RSTICK_RIGHT=524288 };
unsigned afn_keys_held=0, afn_keys_pressed=0, afn_keys_released=0;
static int key_is_down(unsigned k){ return (afn_keys_held & k)!=0; }
static int key_hit(unsigned k){ return (afn_keys_pressed & k)!=0; }
static int key_released(unsigned k){ return (afn_keys_released & k)!=0; }

static void input_update(const SceCtrlData* pad) {
    unsigned b = pad->buttons, k = 0;
    if (b & SCE_CTRL_CROSS)    k|=KEY_A;
    if (b & SCE_CTRL_CIRCLE)   k|=KEY_B;
    if (b & SCE_CTRL_SQUARE)   k|=KEY_X;
    if (b & SCE_CTRL_TRIANGLE) k|=KEY_Y;
    if (b & SCE_CTRL_LTRIGGER) k|=KEY_L;
    if (b & SCE_CTRL_RTRIGGER) k|=KEY_R;
    if (b & SCE_CTRL_START)    k|=KEY_START;
    if (b & SCE_CTRL_SELECT)   k|=KEY_SELECT;
    if (b & SCE_CTRL_UP)       k|=KEY_UP;
    if (b & SCE_CTRL_DOWN)     k|=KEY_DOWN;
    if (b & SCE_CTRL_LEFT)     k|=KEY_LEFT;
    if (b & SCE_CTRL_RIGHT)    k|=KEY_RIGHT;
    // Sticks reported as their OWN keys (NOT folded into the d-pad), so a node can
    // bind the d-pad, left stick, and right stick independently. ±48 deadzone.
    int ax = (int)pad->lx - 128, ay = (int)pad->ly - 128;
    if (ay<-48) k|=KEY_LSTICK_UP;   if (ay>48) k|=KEY_LSTICK_DOWN;
    if (ax<-48) k|=KEY_LSTICK_LEFT; if (ax>48) k|=KEY_LSTICK_RIGHT;
    int rx = (int)pad->rx - 128, ry = (int)pad->ry - 128;
    if (ry<-48) k|=KEY_RSTICK_UP;   if (ry>48) k|=KEY_RSTICK_DOWN;
    if (rx<-48) k|=KEY_RSTICK_LEFT; if (rx>48) k|=KEY_RSTICK_RIGHT;
    afn_keys_pressed  = k & ~afn_keys_held;
    afn_keys_released = ~k & afn_keys_held;
    afn_keys_held     = k;
}

// ---- Script-glue variables ------------------------------------------------
// The emitted node graph writes a LOT of variables that belong to subsystems
// the PSV runtime hasn't ported yet (HUD, grind rails, fades, sprite manip,
// Mode 0). They're defined here as inert globals — exactly like PSP's
// script_glue.c — so the generated code links; only the movement/camera/anim
// ones above are actually consumed. As each subsystem lands, its var stops
// being inert.
#ifndef NUM_SPRITES
#define NUM_SPRITES 64
#endif
int afn_play_anim=0, afn_anim_prio=0, afn_anim_speed=0, afn_auto_orbit_speed=0;
int afn_skel_anim_obj=-1, afn_skel_anim_clip=0, afn_sprite_anim_spr=-1, afn_sprite_anim_val=0;
int afn_collided_sprite=-1, afn_collided_tm_obj=-1, afn_bp_cur_spr_idx=-1, afn_bp_cur_tm_obj=-1;
int afn_current_mode=0, afn_current_scene=0, afn_scripts_stopped=0;
int afn_start_x=0, afn_start_y=0, afn_start_z=0, afn_text_color=0, afn_timer_visible=0;
int afn_wall_collided_sprite=-1, afn_fade_target=0, afn_fade_frames=0, afn_fade_counter=0, afn_fade_level=0;
int player_vy=0, player_ground_y=0, afn_player_vx_world=0, afn_player_vz_world=0;
int afn_velocity_falloff=0, afn_pending_boost_fwd=0;
int afn_grinding=0, afn_grinding_active=0, afn_grind_rail=-1, afn_grind_power=0;
int afn_grind_boost=0, afn_grind_bleed=0, afn_grind_catch_y=0, afn_grind_catch_x=0;
int afn_grind_vel=0, afn_grind_dx=0, afn_grind_dz=0;   // runtime grind state (IsGrinding reads vel)
// Gravity/terminal in 8.8 fixed (256 = 1 world unit/frame), so a SetGravity /
// SetMaxFall node (which writes value*256) drives them. Seeded to the PSV
// world defaults (0.8 / 30) rather than the weak editor-pixel defaults.
int afn_gravity=205, afn_terminal_vel=7680, afn_friction=0, afn_force_x=0, afn_force_z=0;
int afn_cam_locked=0, afn_cam_speed=0, afn_tank_camera=0, afn_player_heading=0;
int afn_player_height=0, afn_player_width=0, afn_bg_color=0, afn_anim_speed_dummy=0;
int afn_active_element=0, afn_elem_idx=0, afn_cursor_stop=0, afn_stop_count=0;
int afn_hud_value[4]={0};   // SetHudValue counter slots (text rows bind to these)
int afn_checkpoint_set=0, afn_checkpoint_x=0, afn_checkpoint_y=0, afn_checkpoint_z=0;
int afn_score=0, afn_shake_frames=0, afn_shake_intensity=0, afn_last_key=0;
int afn_frame_count=0, afn_dt_tick=0;
// Scene transition (ChangeScene/ReloadScene call this as a FUNCTION). PSV exports
// one scene, so a swap to a DIFFERENT scene index can only fade + reset to spawn
// (full multi-scene needs an all-scenes export); ReloadScene (same index) is a
// true respawn. Phase machine ticked in the main loop (it owns the player vars).
int afn_scene_phase = 0;       // 0 idle, 1 fading out (awaiting swap), 2 fading in
int afn_scene_pending = 0, afn_scene_pending_mode = 0;
void afn_scene_start_transition(int scene, int mode, int frames) {
    afn_scene_pending = scene; afn_scene_pending_mode = mode;
    extern int afn_fade_target, afn_fade_frames, afn_fade_counter;
    afn_fade_target = -16; afn_fade_frames = frames > 0 ? frames : 15; afn_fade_counter = afn_fade_frames;
    afn_scene_phase = 1;
}
// Player physics vars the emitted code reads/writes (NDS defines these in
// fps3d.c). Kept inert for now — the movement loop uses its own playerX/Y/Z;
// wiring teleport/IsMoving/Jump nodes to these is a follow-up.
int player_x=0, player_y=0, player_z=0, player_vy_unused=0;
int player_on_ground=1, player_moving=0;
unsigned int afn_flags=0, afn_rng=1;
// afn_hud_visible is indexed by HUD ELEMENT (not sprite); size to the larger of
// the two so ShowHUD/CursorUp (element indices) and any sprite-keyed use both fit.
#if defined(AFN_HAS_HUD) && (AFN_HUD_ELEM_COUNT > NUM_SPRITES)
  #define AFN_HUD_VIS_N AFN_HUD_ELEM_COUNT
#else
  #define AFN_HUD_VIS_N NUM_SPRITES
#endif
unsigned char afn_hud_visible[AFN_HUD_VIS_N]={0};
unsigned char afn_sprite_visible[NUM_SPRITES]={0};
unsigned char afn_sprite_flip[NUM_SPRITES]={0}, afn_collision_enabled[NUM_SPRITES]={0};
int afn_hp[NUM_SPRITES]={0}, afn_state_timer[NUM_SPRITES]={0};
int afn_stop_links[16]={0};
int afn_hud_layer_frame[8]={0}, afn_hud_layer_tick[8]={0};
unsigned char afn_hud_layer_active[8]={0}, afn_hud_layer_speed_override[8]={0};
int tm_fol_active=0, tm_fol_obj=-1, tm_fol_dist=0, tm_fol_facing=0, tm_fol_moving=0, tm_fol_speed=0;
int tm_player_tx=0, tm_player_ty=0;
// Audio entry points — defined in audio.c (sceAudio software mixer + sequencer).
void afn_play_sound(int id);
void afn_play_sfx(int smpIdx, int gain, int fifo);
void afn_stop_sound(void);
void afn_stop_sfx_sample(int smpIdx);
void afn_audio_init(void);
void afn_audio_tick(void);

// The emitted node graph. Included AFTER the variables/keys above so its static
// functions can reference them (single translation unit). Defines AFN_HAS_SCRIPT
// when the scene actually has nodes; otherwise this is an inert stub and the raw
// stick input above drives movement.
#include "psv_script.h"

#ifdef AFN_HAS_SCRIPT
static void script_start(void) { afn_emitted_script_start(); afn_bp_dispatch_start(); }
static void script_tick(void) {
    // Node-driven inputs are recomputed from scratch each frame by the graph.
    afn_input_fwd = 0; afn_input_right = 0; afn_speed_prio = 0; afn_move_speed = 0;
    afn_emitted_script_update();
    afn_emitted_script_key_held();
    afn_emitted_script_key_pressed();
    afn_emitted_script_key_released();
    afn_bp_dispatch_update();
    afn_bp_dispatch_key_held();
    afn_bp_dispatch_key_pressed();
    afn_bp_dispatch_key_released();
}
static int script_present(void) { return 1; }
#else
static void script_start(void) {}
static void script_tick(void)  {}
static int  script_present(void){ return 0; }
#endif

// ---------------------------------------------------------------------------
// HUD overlay (Phase 8). 2D screen-space pieces/text/cursor authored in
// GBA-native 240x160 (CLAUDE.md), drawn in an ortho pass scaled to the Vita
// 960x544 screen via the projection. Pieces/cursor are RGBA frame textures from
// psv_hud.h; text uses an embedded 8x8 font (uppercase + digits + symbols).
//
// UNTESTED: no project exported during development carried HUD elements. The
// code compiles inert (no AFN_HAS_HUD) and activates when a scene with HUD data
// is re-exported. Author coords drive 240x160; glOrthof maps that to the screen.
#ifdef AFN_HAS_HUD
static GLuint s_hudTex[AFN_HUD_FRAME_COUNT];
static GLuint s_hudFontTex;

// Compact 8x8 font: space, 0-9, A-Z, then : / % - . x + !  (lowercase folds up).
// 1 byte per row, bit7 = leftmost pixel. Index 0 is a blank cell.
#define HUD_FONT_GLYPHS 45
static const unsigned char s_hudFont[HUD_FONT_GLYPHS * 8] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // ' '
    0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00, // 0
    0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00, // 1
    0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00, // 2
    0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00, // 3
    0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00, // 4
    0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00, // 5
    0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00, // 6
    0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00, // 7
    0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00, // 8
    0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00, // 9
    0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00, // A
    0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00, // B
    0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00, // C
    0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00, // D
    0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00, // E
    0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00, // F
    0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00, // G
    0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00, // H
    0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00, // I
    0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00, // J
    0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00, // K
    0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00, // L
    0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00, // M
    0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00, // N
    0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00, // O
    0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00, // P
    0x3C,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00, // Q
    0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00, // R
    0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00, // S
    0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00, // T
    0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00, // U
    0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00, // V
    0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00, // W
    0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00, // X
    0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00, // Y
    0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00, // Z
    0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00, // :
    0x06,0x0C,0x0C,0x18,0x30,0x30,0x60,0x00, // /
    0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00, // %
    0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00, // -
    0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00, // .
    0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00, // x
    0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00, // +
    0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00, // !
};
// Map an ASCII char to a glyph slot (0 = blank for anything unknown).
static int hud_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    switch (c) { case ':': return 37; case '/': return 38; case '%': return 39;
                 case '-': return 40; case '.': return 41; case 'x': return 11+('X'-'A');
                 case '+': return 43; case '!': return 44; }
    return 0;
}

static void hud_init(void) {
    for (int i = 0; i < AFN_HUD_FRAME_COUNT; i++) {
        glGenTextures(1, &s_hudTex[i]);
        glBindTexture(GL_TEXTURE_2D, s_hudTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, afn_hud_frame_w[i], afn_hud_frame_h[i], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, afn_hud_frames[i]);
    }
    // Build a font atlas: HUD_FONT_GLYPHS columns of 8x8, white where the bit is
    // set (alpha 0 elsewhere) so glColor modulation tints the text.
    static unsigned int atlas[HUD_FONT_GLYPHS * 8 * 8];
    int aw = HUD_FONT_GLYPHS * 8;
    for (int g = 0; g < HUD_FONT_GLYPHS; g++)
        for (int row = 0; row < 8; row++) {
            unsigned char bits = s_hudFont[g * 8 + row];
            for (int col = 0; col < 8; col++) {
                int on = (bits >> (7 - col)) & 1;
                atlas[row * aw + g * 8 + col] = on ? 0xFFFFFFFFu : 0u;
            }
        }
    glGenTextures(1, &s_hudFontTex);
    glBindTexture(GL_TEXTURE_2D, s_hudFontTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
}

static void hud_quad(GLuint tex, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1, unsigned int col) {
    AfnVertex q[4] = {
        { u0, v0, col, x0, y0, 0 }, { u1, v0, col, x1, y0, 0 },
        { u1, v1, col, x1, y1, 0 }, { u0, v1, col, x0, y1, 0 },
    };
    AfnVertex* v = q;
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT,         sizeof(AfnVertex), &v->u);
    glColorPointer   (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
    glVertexPointer  (3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

// Draw a string at (x,y) in HUD coords; returns the pen x after the string.
static float hud_text(const char* s, float x, float y, int scale, unsigned int col) {
    const float aw = (float)(HUD_FONT_GLYPHS * 8);
    float gw = 8.0f * scale, adv = 6.0f * scale;   // glyphs are 8 wide, 6 px advance
    for (const char* p = s; *p; p++) {
        int g = hud_glyph(*p);
        if (g != 0) {   // skip drawing blanks, still advance
            float u0 = (g * 8) / aw, u1 = (g * 8 + 8) / aw;
            hud_quad(s_hudFontTex, x, y, x + gw, y + gw, u0, 0.0f, u1, 1.0f, col);
        }
        x += adv;
    }
    return x;
}

static void hud_render(void) {
    // Ortho 240x160 with a top-left origin (y grows downward) to match the
    // editor's authoring space. The viewport is the full Vita screen, so this
    // stretches 240x160 -> 960x544 (same 1.5:1 letterbox the scene uses).
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0, 240, 160, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    for (int e = 0; e < AFN_HUD_ELEM_COUNT; e++) {
        const AfnHudElem* el = &afn_hud_elems[e];
        if (!afn_hud_visible[e]) continue;
        if (el->mode == 2) continue;                       // Mode-0-only element
        if (!(el->sceneMask & (1u << afn_current_scene))) continue;
        float bx = el->screenX, by = el->screenY;
        // Pieces (graphics).
        for (int k = 0; k < el->pieceCount; k++) {
            const AfnHudPiece* pc = &afn_hud_piece[el->pieceStart + k];
            hud_quad(s_hudTex[pc->tex], bx + pc->x, by + pc->y,
                     bx + pc->x + pc->w, by + pc->y + pc->h, 0, 0, 1, 1, 0xFFFFFFFFu);
        }
        // Text rows (static label or counter bound to afn_hud_value[slot]).
        for (int k = 0; k < el->textCount; k++) {
            const AfnHudText* tr = &afn_hud_text[el->textStart + k];
            char buf[40];
            const char* str = tr->text;
            if (tr->slot < 4 && tr->pad >= 0 && tr->text[0] == '\0') {
                // pure counter row (no static text): render the slot value
                int val = afn_hud_value[tr->slot];
                int n = 0; char tmp[16];
                if (val < 0) { buf[n++] = '-'; val = -val; }
                int d = 0; do { tmp[d++] = (char)('0' + val % 10); val /= 10; } while (val && d < 15);
                while (d < tr->pad && n + (tr->pad - d) < 39) buf[n++] = '0';   // zero-pad
                while (d > 0) buf[n++] = tmp[--d];
                buf[n] = '\0'; str = buf;
            }
            hud_text(str, bx + tr->x, by + tr->y, tr->scale < 1 ? 1 : tr->scale, tr->color);
        }
        // Cursor at the active stop (menu selection).
        if (el->curTex >= 0 && afn_active_element == e && el->stopCount > 0) {
            int sidx = afn_cursor_stop; if (sidx < 0) sidx = 0; if (sidx >= el->stopCount) sidx = el->stopCount - 1;
            const AfnHudStop* st = &afn_hud_stops[el->stopStart + sidx];
            float cw = afn_hud_frame_w[el->curTex], ch = afn_hud_frame_h[el->curTex];
            hud_quad(s_hudTex[el->curTex], bx + st->x + el->curX, by + st->y + el->curY,
                     bx + st->x + el->curX + cw, by + st->y + el->curY + ch, 0, 0, 1, 1, 0xFFFFFFFFu);
        }
    }
    glEnable(GL_DEPTH_TEST);
}
#endif // AFN_HAS_HUD

int main(void)
{
    // Max out the Vita clocks. 444 MHz is the hardware/API ceiling for the
    // Cortex-A9 cores (the default app clock is 333 MHz); scePower clamps
    // anything higher, so a true 500 MHz needs a kernel overclock plugin and is
    // not done here. Bus/GPU are pushed to their maxima too for headroom.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // Force the GLSL/fixed-function pipeline to FULL precision. vitaGL can fold
    // shader floats down to half (fp16); at world coords ~150 that quantizes
    // positions to ~0.1 units, which z-fights and shifts as the view rotates
    // (large floor/slope flicker while the small-coord rig stays clean).
    vglUseLowPrecision(GL_FALSE);
    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    upload_textures();
    afn_audio_init();   // software mixer thread (no-op if the scene has no sound)
#ifdef AFN_HAS_SKY
    sky_init();
#endif
#ifdef AFN_HAS_SPRITES
    billboards_init();
#endif
#ifdef AFN_HAS_PLAYER_RIG
    rig_init();
#endif
#ifdef AFN_HAS_HUD
    hud_init();
    // Seed per-element visibility from the authored "visible" flag (ShowHUD /
    // CursorUp toggle it at runtime).
    for (int i = 0; i < AFN_HUD_ELEM_COUNT; i++) afn_hud_visible[i] = afn_hud_elems[i].startVis;
#endif

    // Player state. The follow camera and NPCs read playerX/Y/Z; the movement
    // step below drives it (camera-relative left stick + gravity + floor/wall
    // collision against the level mesh).
#ifdef AFN_HAS_PLAYER_RIG
    float playerX = AFN_PLAYER_START_X, playerY = AFN_PLAYER_START_Y, playerZ = AFN_PLAYER_START_Z;
#else
    float playerX = afn_cam_start_x, playerY = afn_cam_start_h, playerZ = afn_cam_start_z;
#endif
    float playerYaw = 0.0f;   // rig facing (degrees)
    float playerVY  = 0.0f;   // vertical velocity (gravity / jump)
    int   grounded  = 1;
    float s_floorN[3] = {0.0f, 1.0f, 0.0f};   // smoothed floor normal (slope tilt)
#ifdef AFN_HAS_RAIL_PATH
    int   gr_on = 0; float gr_arc = 0.0f; int gr_dir = 1; float gr_speed = 0.0f, gr_prevY = 0.0f;
#endif
    collide_build();
    {   // Drop onto the floor at spawn so we don't start mid-air.
        float fy, fn[3];
        if (collide_floor(playerX, playerZ, playerY + 200.0f, &fy, fn))
            playerY = fy - COL_BOTTOM;
    }

    // Orbit-follow camera, anchored to the active camera slot
    // (afn_cam_slots[afn_active_camera] = { yaw, dist, height, horizon }). The
    // player orbits freely with the right stick / L-R; triggers zoom. The slot's
    // height sets the default downward tilt and the look-at target height. PSV
    // has no scripts yet so afn_active_camera stays 0 (scene default), but the
    // runtime re-reads it each frame so a future SetCamera node just works.
    if (afn_active_camera < 0 || afn_active_camera >= AFN_CAM_SLOT_COUNT) afn_active_camera = 0;
    const float* slot0 = afn_cam_slots[afn_active_camera];
    orbit_angle       = (int)(slot0[0] * (65536.0f / 6.2831853f));  // GLOBAL brad (node + manual)
    afn_player_heading = orbit_angle;   // tank heading starts facing the camera-forward
    float camDist     = slot0[1] > 1.0f ? slot0[1] : 60.0f;  // world px
    float camHeight   = slot0[2];                            // world px
    // Pitch is also brad in the global orbit_pitch so the OrbitCamera Up/Down
    // node and the right stick drive it together. Seed it from the slot's tilt.
    orbit_pitch       = (int)(atan2f(camHeight > 0.0f ? camHeight : 8.0f, camDist) * (65536.0f / 6.2831853f));

    SceCtrlData pad;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#ifdef AFN_HAS_SPRITE_IDX
    // All sprites start visible + collidable (the inert arrays default to 0 =
    // hidden). DestroyObject/SetVisible nodes flip these at runtime.
    for (int i = 0; i < NUM_SPRITES; i++) { afn_sprite_visible[i] = 1; afn_collision_enabled[i] = 1; }
#endif
    script_start();   // OnStart + blueprint start hooks

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;

        // Expose engine state to the node graph BEFORE it ticks: player_on_ground
        // (Jump/IsJumping gates), player_moving (IsMoving), and player_x/y/z
        // (teleport/distance/checkpoint nodes read+write the world position).
        player_on_ground = grounded;
        int pteleX = player_x = (int)playerX;
        int pteleY = player_y = (int)playerY;
        int pteleZ = player_z = (int)playerZ;

        // Node-driven: input_update() sets the raw defaults, then the emitted
        // graph (script_tick) overrides afn_input_fwd/right, afn_move_speed,
        // orbit_angle and afn_rig_clip. The movement/camera below only READ them.
        input_update(&pad);
        script_tick();
        afn_audio_tick();   // 60 Hz sequencer clock (envelopes / note scheduling)

        // Screen-fade tick: ease afn_fade_level toward afn_fade_target over
        // afn_fade_counter frames (set by ChangeScene / fade nodes). Rendered as
        // a fullscreen overlay below (vitaGL has no REG_MASTER_BRIGHT).
        if (afn_fade_counter > 0) {
            afn_fade_counter--;
            int span = afn_fade_frames > 0 ? afn_fade_frames : 1;
            afn_fade_level = afn_fade_target * (span - afn_fade_counter) / span;
            if (afn_fade_counter == 0) afn_fade_level = afn_fade_target;
        }
        // Scene transition: at fade-out completion, swap scene index + respawn,
        // then fade back in. ReloadScene = true respawn; ChangeScene to another
        // index resets in the SAME geometry (full multi-scene needs an all-scenes
        // export — only one scene's data is present).
        if (afn_scene_phase == 1 && afn_fade_counter == 0) {
            afn_current_scene = afn_scene_pending; afn_current_mode = afn_scene_pending_mode;
#ifdef AFN_HAS_PLAYER_RIG
            playerX = AFN_PLAYER_START_X; playerY = AFN_PLAYER_START_Y; playerZ = AFN_PLAYER_START_Z;
#else
            playerX = afn_cam_start_x; playerY = afn_cam_start_h; playerZ = afn_cam_start_z;
#endif
            playerVY = 0.0f; grounded = 1; afn_player_heading = orbit_angle;
#ifdef AFN_HAS_SPRITE_IDX
            for (int i = 0; i < NUM_SPRITES; i++) { afn_sprite_visible[i] = 1; afn_collision_enabled[i] = 1; }
#endif
            afn_fade_target = 0; afn_fade_frames = 15; afn_fade_counter = 15; afn_fade_level = -16;
            afn_scene_phase = 2;
        } else if (afn_scene_phase == 2 && afn_fade_counter == 0) {
            afn_scene_phase = 0;
        }

        // A teleport/checkpoint node wrote player_x/y/z — apply it to the float
        // position (normal frames leave them unchanged, so no precision loss).
        if (player_x != pteleX) playerX = (float)player_x;
        if (player_y != pteleY) playerY = (float)player_y;
        if (player_z != pteleZ) playerZ = (float)player_z;

        // Re-read the active slot each frame (a SetCamera node retargets it).
        // Clamp first (matches NDS fps3d.c): a Set Camera fed an out-of-range slot
        // (e.g. an Integer >= AFN_CAM_SLOT_COUNT) would otherwise index past
        // afn_cam_slots and crash. Out-of-range falls back to slot 0 (scene default).
        if (afn_active_camera < 0 || afn_active_camera >= AFN_CAM_SLOT_COUNT) afn_active_camera = 0;
        const float* S = afn_cam_slots[afn_active_camera];
        camDist   = S[1] > 1.0f ? S[1] : camDist;   // keep manual zoom unless slot overrides
        camHeight = S[2];

        // Camera orbit is purely node-driven: the OrbitCamera node writes
        // orbit_angle/orbit_pitch (reading KEY_L/KEY_R etc.). No hardcoded right
        // stick or trigger control here.
        // Clamp pitch to ~±80 deg in brad (65536 = 360 deg -> ±14563).
        if (orbit_pitch >  14563) orbit_pitch =  14563;
        if (orbit_pitch < -14563) orbit_pitch = -14563;

        // Camera yaw + pitch (radians) from the node/manual orbit_angle/pitch (brad).
        float camAngle = orbit_angle * (6.2831853f / 65536.0f);
        float pitch    = orbit_pitch * (6.2831853f / 65536.0f);

        // --- Player movement: reads the node-set move intent (afn_input_fwd/right,
        //     256 = full) in camera space, scaled by afn_move_speed (or the walk
        //     default when no script). Ported from PSP scene_update. ---
        float fAmt = afn_input_fwd  / 256.0f;
        float rAmt = afn_input_right / 256.0f;
        // Tank controls: when afn_tank_camera is set (a Turn Player / tank node),
        // movement + facing follow afn_player_heading (D-pad turned), so the
        // camera orbits independently. Otherwise movement is camera-relative.
        float moveAngle = afn_tank_camera ? (afn_player_heading * (6.2831853f/65536.0f)) : camAngle;
        float fwdX = sinf(moveAngle), fwdZ = cosf(moveAngle);
        float rgtX = cosf(moveAngle), rgtZ = -sinf(moveAngle);
        float mvX = fAmt*fwdX + rAmt*rgtX;
        float mvZ = fAmt*fwdZ + rAmt*rgtZ;
        // Move only when the node graph asks: a movement node sets afn_input_fwd/
        // right AND afn_move_speed. No walk-speed fallback — purely node-driven.
        if ((mvX*mvX + mvZ*mvZ > 0.0001f) && afn_move_speed > 0 && !afn_player_frozen) {
            float speed = afn_move_speed * 0.08f;
            float dx = mvX*speed, dz = mvZ*speed;
            float dlen = sqrtf(dx*dx + dz*dz);
            int steps = (int)(dlen / 3.0f) + 1;   // MAX_MOVE_STEP: don't tunnel walls
            float ix = dx/steps, iz = dz/steps;
            for (int st = 0; st < steps; st++) { playerX += ix; playerZ += iz; collide_walls(&playerX, &playerZ, playerY); }
            playerYaw = atan2f(mvX, mvZ) * (180.0f/3.14159265f);
        }
        // In tank mode the rig faces the heading even when standing still.
        if (afn_tank_camera) playerYaw = afn_player_heading * (360.0f/65536.0f);

        // Node-driven world-axis push velocity (boost pads / knockback). 8.8
        // fixed. BoostForward wrote a pending magnitude -> decompose along the
        // camera forward; SetVelocityX/Z write the globals directly. Linear
        // VelocityFalloff(N) decay over N frames. (Mirrors fps3d.c:1190-1207.)
        if (afn_pending_boost_fwd) {
            afn_player_vx_world = (int)(sinf(camAngle) * afn_pending_boost_fwd);
            afn_player_vz_world = (int)(cosf(camAngle) * afn_pending_boost_fwd);
            afn_pending_boost_fwd = 0;
        }
        if (afn_player_vx_world || afn_player_vz_world) {
            playerX += afn_player_vx_world / 256.0f;
            playerZ += afn_player_vz_world / 256.0f;
            collide_walls(&playerX, &playerZ, playerY);
            if (afn_velocity_falloff > 0) {
                afn_player_vx_world -= afn_player_vx_world / afn_velocity_falloff;
                afn_player_vz_world -= afn_player_vz_world / afn_velocity_falloff;
                if (--afn_velocity_falloff == 0) { afn_player_vx_world = 0; afn_player_vz_world = 0; }
            }
        }

        player_moving = (afn_input_fwd != 0 || afn_input_right != 0);   // IsMoving gate
        // Jump is node-driven: a Jump node sets player_vy (8.8). Capture it once
        // here so both the normal jump and the grind exit (below) react to the
        // same node event — no hardcoded jump button.
        float jumpVel = 0.0f;
        if (player_vy != 0) { jumpVel = player_vy / 256.0f; player_vy = 0; }
        if (jumpVel != 0.0f) { playerVY = jumpVel; grounded = 0; }   // Jump node
        collide_walls(&playerX, &playerZ, playerY);
        playerVY -= afn_gravity / 256.0f;                  // gravity (SetGravity node, 8.8)
        float term = afn_terminal_vel / 256.0f;
        if (playerVY < -term) playerVY = -term;            // terminal velocity (SetMaxFall node)
        playerY += playerVY;
        {
            float fy, fn[3];
            if (collide_floor(playerX, playerZ, playerY, &fy, fn) && playerY <= fy - COL_BOTTOM) {
                playerY = fy - COL_BOTTOM; playerVY = 0.0f; grounded = 1;
                // Ease the rig's up-axis toward the floor normal (slope tilt).
                s_floorN[0] += (fn[0]-s_floorN[0])*0.2f;
                s_floorN[1] += (fn[1]-s_floorN[1])*0.2f;
                s_floorN[2] += (fn[2]-s_floorN[2])*0.2f;
            } else {
                grounded = 0;
                s_floorN[0] += (0.0f-s_floorN[0])*0.1f;   // airborne: ease back upright
                s_floorN[1] += (1.0f-s_floorN[1])*0.1f;
                s_floorN[2] += (0.0f-s_floorN[2])*0.1f;
            }
        }

#ifdef AFN_HAS_PLAYER_RIG
        // Per-NPC gravity + floor landing (NDS render_npc_rigs parity): enemies
        // fall, settle on the ground, and can be knocked airborne (set s_npcVY>0
        // to launch). Same gravity/terminal as the player; each NPC rests with
        // its authored collision-box bottom on the floor surface. collide_floor
        // tags afn_floor_sprite as a side effect, so save/restore it — the
        // player's grind floor-sprite must not be clobbered by NPC queries.
        {
            int savedFloorSpr = afn_floor_sprite;
            float ng = afn_gravity / 256.0f, nterm = afn_terminal_vel / 256.0f;
            for (int i = 0; i < AFN_NPC_COUNT; i++) {
                int eidx = (int)afn_npc_inst[i][7];
                if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;
                // True box: half-extents (hx,hy,hz) + center offset (cx,cy,cz),
                // all in world px relative to the NPC origin. Y rests the box
                // bottom (cy-hy) on the floor.
                float hx = afn_npc_col[i][0], hy = afn_npc_col[i][1], hz = afn_npc_col[i][2];
                float cx = afn_npc_col[i][3], cy = afn_npc_col[i][4], cz = afn_npc_col[i][5];
                float nbottom = cy - hy;
                s_npcVY[i] -= ng;
                if (s_npcVY[i] < -nterm) s_npcVY[i] = -nterm;
                s_npcY[i] += s_npcVY[i];
                float nfy, nfn[3];
                if (collide_floor(afn_npc_inst[i][0], afn_npc_inst[i][2], s_npcY[i], &nfy, nfn)
                    && s_npcY[i] <= nfy - nbottom) {
                    s_npcY[i] = nfy - nbottom; s_npcVY[i] = 0.0f; s_npcGround[i] = 1;
                } else {
                    s_npcGround[i] = 0;
                }

                // SOLID blocker: push the player (a cylinder of COL_RADIUS) out
                // of the NPC's AABB. True box — independent X/Z half-extents and
                // the box's center offset are honored (circle-vs-AABB resolve).
                if (eidx < 0 || afn_collision_enabled[eidx]) {
                    float bcx = afn_npc_inst[i][0] + cx, bcz = afn_npc_inst[i][2] + cz;
                    float xmin = bcx - hx, xmax = bcx + hx, zmin = bcz - hz, zmax = bcz + hz;
                    float npcBot = s_npcY[i] + cy - hy, npcTop = s_npcY[i] + cy + hy;
                    float plBot  = playerY + COL_BOTTOM, plTop = playerY + COL_TOP;
                    if (plTop > npcBot && plBot < npcTop) {            // vertical overlap
                        float R = COL_RADIUS;
                        if (playerX > xmin - R && playerX < xmax + R &&
                            playerZ > zmin - R && playerZ < zmax + R) {
                            if (playerX > xmin && playerX < xmax &&
                                playerZ > zmin && playerZ < zmax) {
                                // Center inside the core box: eject out the nearest face.
                                float pxl = playerX - xmin, pxr = xmax - playerX;
                                float pzl = playerZ - zmin, pzr = zmax - playerZ;
                                float mx = pxl < pxr ? pxl : pxr, mz = pzl < pzr ? pzl : pzr;
                                if (mx < mz) playerX = (pxl < pxr) ? xmin - R : xmax + R;
                                else         playerZ = (pzl < pzr) ? zmin - R : zmax + R;
                            } else {
                                // Edge/corner: push to the circle boundary off the
                                // closest point on the box.
                                float qx = playerX < xmin ? xmin : (playerX > xmax ? xmax : playerX);
                                float qz = playerZ < zmin ? zmin : (playerZ > zmax ? zmax : playerZ);
                                float dx = playerX - qx, dz = playerZ - qz, d2 = dx*dx + dz*dz;
                                if (d2 < R*R && d2 > 1e-8f) {
                                    float d = sqrtf(d2), k = R / d;
                                    playerX = qx + dx*k; playerZ = qz + dz*k;
                                }
                            }
                        }
                    }
                }
            }
            afn_floor_sprite = savedFloorSpr;
        }
#endif

#ifdef AFN_HAS_RAIL_PATH
        // Grind rails: StartGrind sets afn_grinding + afn_grind_rail (a sprite
        // index that matches afn_floor_sprite). Engage when on/near that rail's
        // floor, then slide the centerline — faster downhill (grade), launch off
        // the end / on jump with momentum. Float-spirit port of fps3d.c's grind.
        {
            int rail = afn_grind_rail;
            if (!gr_on && afn_grinding && rail >= 0 && rail < NUM_SPRITES
                && afn_rail_count[rail] >= 2 && playerVY <= 0.5f) {
                float d2; float arc = rail_nearest(rail, playerX, playerZ, &d2);
                float gx,gy,gz,tdx,tdz; rail_sample(rail, arc, &gx,&gy,&gz,&tdx,&tdz);
                float catchR = afn_grind_catch_x > 0 ? (float)afn_grind_catch_x : COL_RADIUS*3.0f;
                float vWin   = COL_TOP + (afn_grind_catch_y > 0 ? (float)afn_grind_catch_y : COL_TOP);
                int onRailFloor = (grounded && afn_floor_sprite == rail);
                if ((onRailFloor || d2 <= catchR*catchR) && fabsf(playerY - gy) <= vWin) {
                    gr_on = 1; gr_arc = arc; gr_prevY = gy;
                    float h = sinf(playerYaw*DEG2RAD)*tdx + cosf(playerYaw*DEG2RAD)*tdz;
                    gr_dir = (h >= 0.0f) ? 1 : -1;
                    gr_speed = 12.0f;          // initial slide speed (px/frame)
                    afn_grind_vel = 1;         // flag: grinding (node SFX gates read this)
                }
            }
            if (gr_on) {
                int rs = afn_rail_start[rail], rn = afn_rail_count[rail];
                float total = rail_len(rail);
                if ((jumpVel != 0.0f) || !afn_grinding) {   // jump off (node) or grind disabled
                    float gx,gy,gz,tdx,tdz; rail_sample(rail, gr_arc, &gx,&gy,&gz,&tdx,&tdz);
                    afn_player_vx_world = (int)(tdx * gr_dir * gr_speed * 256.0f);   // launch w/ momentum
                    afn_player_vz_world = (int)(tdz * gr_dir * gr_speed * 256.0f);
                    afn_velocity_falloff = 30;
                    if (jumpVel != 0.0f) playerVY = jumpVel;
                    gr_on = 0; afn_grind_vel = 0; afn_grinding = 0;
                } else {
                    float gx,gy,gz,tdx,tdz; rail_sample(rail, gr_arc, &gx,&gy,&gz,&tdx,&tdz);
                    playerX = gx; playerY = gy - COL_BOTTOM; playerZ = gz; playerVY = 0.0f; grounded = 1;
                    playerYaw = atan2f(tdx*gr_dir, tdz*gr_dir) * (180.0f/3.14159265f);
                    float grade = gr_prevY - gy; gr_prevY = gy;          // +down / -up
                    float gpow = (afn_grind_power ? afn_grind_power : 24) / 256.0f;
                    gr_speed += (grade > 0.0f ? grade * gpow : grade * 0.05f);
                    if (gr_speed < 3.0f)  gr_speed = 3.0f;
                    if (gr_speed > 45.0f) gr_speed = 45.0f;
                    gr_arc += gr_dir * gr_speed;
                    if (gr_arc <= 0.0f || gr_arc >= total) {
                        int term = (gr_arc <= 0.0f) ? 0 : (rn-1);
                        if (afn_rail_pt_bounce[rs+term]) {              // bumper -> reverse
                            gr_dir = -gr_dir; gr_arc = (gr_arc <= 0.0f) ? 0.0f : total;
                        } else {                                       // launch off end
                            float lx,ly,lz,ltx,ltz; rail_sample(rail, gr_arc<=0.0f?0.0f:total, &lx,&ly,&lz,&ltx,&ltz);
                            afn_player_vx_world = (int)(ltx * gr_dir * gr_speed * 256.0f);
                            afn_player_vz_world = (int)(ltz * gr_dir * gr_speed * 256.0f);
                            afn_velocity_falloff = 30;
                            gr_on = 0; afn_grind_vel = 0; afn_grinding = 0;
                        }
                    }
                }
            }
        }
#endif // AFN_HAS_RAIL_PATH

        // Sprite collision -> OnCollision chains. Player circle vs each NPC's
        // position (proximity). Sets afn_collided_sprite to the NPC's editor
        // sprite index, then fires the scene + blueprint collision dispatchers
        // (a blueprint attached to that sprite reacts). 10-frame spawn grace.
        afn_frame_count++;
#ifdef AFN_HAS_SPRITE_IDX
        if (afn_frame_count > 10) {
            for (int i = 0; i < AFN_NPC_COUNT; i++) {
                int sp = (int)afn_npc_inst[i][7];
                if (sp < 0 || sp >= NUM_SPRITES) continue;
                if (!afn_sprite_visible[sp] || !afn_collision_enabled[sp]) continue;
                // OnCollision trigger: player cylinder (COL_RADIUS) vs the NPC's
                // true AABB (independent X/Z extents + center offset + settled Y),
                // matching the solid-blocker test above.
#ifdef AFN_HAS_PLAYER_RIG
                float hx = afn_npc_col[i][0], hy = afn_npc_col[i][1], hz = afn_npc_col[i][2];
                float cx = afn_npc_col[i][3], cy = afn_npc_col[i][4], cz = afn_npc_col[i][5];
                float bcx = afn_npc_inst[i][0] + cx, bcz = afn_npc_inst[i][2] + cz;
                float xmin = bcx - hx, xmax = bcx + hx, zmin = bcz - hz, zmax = bcz + hz;
                float qx = playerX < xmin ? xmin : (playerX > xmax ? xmax : playerX);
                float qz = playerZ < zmin ? zmin : (playerZ > zmax ? zmax : playerZ);
                float ddx = playerX - qx, ddz = playerZ - qz;
                float npcBot = s_npcY[i] + cy - hy, npcTop = s_npcY[i] + cy + hy;
                float plBot = playerY + COL_BOTTOM, plTop = playerY + COL_TOP;
                int hit = (ddx*ddx + ddz*ddz < COL_RADIUS*COL_RADIUS) && (plTop > npcBot && plBot < npcTop);
#else
                float dx = playerX - afn_npc_inst[i][0], dz = playerZ - afn_npc_inst[i][2];
                float dy = playerY - afn_npc_inst[i][1];
                int hit = (dx*dx + dz*dz < (COL_RADIUS*2)*(COL_RADIUS*2)) && (dy > -COL_TOP && dy < COL_TOP);
#endif
                if (hit) {
                    afn_collided_sprite = sp;
                    afn_emitted_script_collision();
                    afn_bp_dispatch_collision();
                }
            }
        }
#endif

        // Follow the player, framed at ~half the camera height (torso, not feet).
        float targetX = playerX, targetY = playerY + camHeight * 0.5f, targetZ = playerZ;

        float cp = cosf(pitch);
        float ex = targetX - sinf(camAngle)*cp*camDist;
        float ez = targetZ - cosf(camAngle)*cp*camDist;
        float ey = targetY + sinf(pitch)*camDist;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Projection.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        {
            // near small (1.0) so the slope — which starts close to the camera —
            // isn't near-clipped; far pulled in to 1500 for better 16-bit depth
            // precision at the floor/slope intersection without touching near.
            const float nearp = 1.0f, farp = 1500.0f, aspect = SCR_W / SCR_H;
            const float top = nearp * 0.767f;     // tan(37.5 deg) ~ vfov 75
            const float right = top * aspect;
            glFrustum(-right, right, -top, top, nearp, farp);
        }

        // View.
        glMatrixMode(GL_MODELVIEW);
        float view[16];
        look_at(view, ex, ey, ez, targetX, targetY, targetZ, 0.0f, 1.0f, 0.0f);

#ifdef AFN_HAS_SKY
        sky_render(camAngle);   // far panorama behind everything (no depth write)
#endif

        for (int si = 0; si < afn_sprite_count; si++) {
            int mi = afn_sprites[si].meshIdx;
            if (mi < 0 || mi >= afn_mesh_count) continue;
#ifdef AFN_HAS_SPRITE_IDX
            int eidx = afn_mesh_inst_sprite[si];
            if (eidx >= 0 && eidx < NUM_SPRITES && !afn_sprite_visible[eidx]) continue;  // hidden/destroyed
#endif
            const AfnSpriteInst* sp = &afn_sprites[si];
            glLoadMatrixf(view);
            glTranslatef(sp->x, sp->y, sp->z);
            if (sp->rotZ != 0.0f) glRotatef(sp->rotZ, 0,0,1);
            if (sp->rotX != 0.0f) glRotatef(sp->rotX, 1,0,0);
            if (sp->rotY != 0.0f) glRotatef(sp->rotY, 0,1,0);
            glScalef(sp->scale, sp->scale, sp->scale);
            draw_mesh(mi);
        }

#ifdef AFN_HAS_PLAYER_RIG
        // Player rig + every NPC: each skinned from its own rig at its own
        // transform/clip (player follows the camera; NPCs at their world spots).
        rigs_render(view, playerX, playerY, playerZ, playerYaw, s_floorN);
#endif

#ifdef AFN_HAS_SPRITES
        billboards_render(view, camAngle, ex, ez);   // camera-facing animated/directional sprites
#endif

#ifdef AFN_HAS_HUD
        hud_render();   // 2D overlay (pieces/text/cursor) on top of the scene
#endif

        // Fade overlay: a fullscreen quad over the scene. level<0 darkens (fade
        // out to black), level>0 brightens (fade in from white). |level|/16 alpha.
        if (afn_fade_level != 0) {
            int lv = afn_fade_level; if (lv > 16) lv = 16; if (lv < -16) lv = -16;
            unsigned int A = (unsigned int)((lv < 0 ? -lv : lv) * 255 / 16);
            unsigned int col = (A << 24) | (lv > 0 ? 0x00FFFFFFu : 0u);   // 0xAABBGGRR
            AfnVertex fq[4] = { {0,0,col,0,0,0}, {0,0,col,1,0,0}, {0,0,col,1,1,0}, {0,0,col,0,1,0} };
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrthof(0,1,0,1,-1,1);
            glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
            glDisable(GL_DEPTH_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            AfnVertex* v = fq;
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer (4, GL_UNSIGNED_BYTE, sizeof(AfnVertex), &v->color);
            glVertexPointer(3, GL_FLOAT,         sizeof(AfnVertex), &v->x);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glEnable(GL_DEPTH_TEST);
        }

        vglSwapBuffers(GL_FALSE);
    }

    sceKernelExitProcess(0);
    return 0;
}
