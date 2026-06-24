// Affinity PS Vita exporter — regenerates the psv_runtime headers (shared with
// the PSP data layout via GenerateAffinityHeaders) and builds affinity_psv.vpk
// through the VitaSDK CMake toolchain.
//
// The Vita renders with vitaGL (fixed-function GL), and its scene/rig data is
// byte-identical to the PSP export, so the only target-specific work here is the
// build: VitaSDK's toolchain ships as devkitPro MSYS2 binaries, so the build is
// invoked through `C:\devkitPro\msys2\usr\bin\bash.exe` (NOT WSL, which is what
// the PSP/pspdev exporter uses). The generated headers carry the "psv_" prefix
// and include "affinity_psv.h" (the GU-free data contract) instead of
// "affinity_psp.h".

#include "psv_package.h"
#include "../common/node_script_emit.h"
#include "../../navmesh/NavMeshBuilder.h"

#include <string>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Affinity {

// Live build-output log: RunMsysBash appends to it as the Vita toolchain runs,
// and the editor's compile terminal renders it. Guarded — the build runs on a
// worker thread while the UI reads this on the main thread.
std::mutex g_psvBuildLogMtx;
std::string g_psvBuildLog;
static void PsvBuildLog(const char* msg) {
    std::lock_guard<std::mutex> lk(g_psvBuildLogMtx);
    g_psvBuildLog += msg; g_psvBuildLog += "\n";
}

// ---- helpers --------------------------------------------------------------
// C:\a\b -> /c/a/b  (devkitPro MSYS2 mounts drives at /<letter>/, unlike WSL's
// /mnt/<letter>/).
static std::string ToMsysPath(const std::string& winPath) {
    std::string p = winPath;
    for (auto& c : p) if (c == '\\') c = '/';
    if (p.size() >= 2 && p[1] == ':') {
        char drive = (char)tolower(p[0]);
        p = "/" + std::string(1, drive) + p.substr(2);
    }
    return p;
}

#ifdef _WIN32
// Path to the devkitPro MSYS2 bash. The whole project builds the NDS/Vita
// runtimes through this shell, so the location is a fixed project assumption.
static const char* kMsysBash = "C:\\devkitPro\\msys2\\usr\\bin\\bash.exe";

// Run a command inside the devkitPro MSYS2 bash, capturing stdout+stderr.
// Returns the exit code, or -1 if bash itself couldn't be launched. Mirrors the
// PSP exporter's RunWslCommand: a NUL stdin (GUI app has no console) and a
// polling drain loop so a lingering inherited write-handle can't hang the read.
static int RunMsysBash(const std::string& shellCmd, std::string& output) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    si.hStdInput = hNul;

    PROCESS_INFORMATION pi = {};
    // -lc: login shell so the user's MSYS profile (PATH etc.) is sourced.
    std::string cmdLine = std::string("\"") + kMsysBash + "\" -lc \"" + shellCmd + "\"";
    BOOL ok = CreateProcessA(nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!ok) { CloseHandle(hRead); output = "Failed to launch devkitPro MSYS2 bash (is devkitPro installed at C:\\devkitPro?)"; return -1; }

    output.clear();
    char buf[512];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD n = 0;
            DWORD toRead = avail < (sizeof(buf) - 1) ? avail : (sizeof(buf) - 1);
            if (ReadFile(hRead, buf, toRead, &n, nullptr) && n > 0) {
                buf[n] = 0; output += buf;
                { std::lock_guard<std::mutex> lk(g_psvBuildLogMtx); g_psvBuildLog += buf; }   // live compile terminal
            }
        } else if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) continue;
            break;
        } else {
            Sleep(15);
        }
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
    return (int)code;
}
#else
static std::string ToMsysPath(const std::string& p) { return p; }
static int RunMsysBash(const std::string&, std::string& output) { output = "Vita build only supported on Windows host"; return -1; }
#endif

// ---- multi-rig generation (PSV-only) --------------------------------------
// The PSP rig generator emits a single (player) rig. PSV needs every rig the
// scene uses — player AND each distinct NPC/enemy model — so NPCs show their
// own character. This is PSV-specific so the PSP runtime's single-rig header is
// untouched. Geometry/clip layout matches the PSP rig data; only the symbols are
// per-rig (afn_rigN_*) with an AfnRig descriptor table the runtime iterates.
static float PVX(float e) { return (e + 512.0f) / 4.0f; }
static float PVY(float h) { return h / 4.0f; }
static std::string PFlt(float v) {
    char b[40]; snprintf(b, sizeof(b), "%.6g", v);
    std::string s = b;
    if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
    return s + "f";
}

static void EmitRigArrays(std::ofstream& f, int ru, const AfnRigExport& rig) {
    int bc = rig.boneCount;
    int vc = (int)rig.verts.size();
    int mc = (int)rig.materials.size(); if (mc < 1) mc = 1;
    int cc = (int)rig.clips.size();
    int tc = (int)rig.indices.size() / 3;
    std::string S = "afn_rig" + std::to_string(ru) + "_";
    std::string D = "AFN_RIG" + std::to_string(ru) + "_";

    f << "static const float " << S << "vpos[" << vc*3 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << PFlt(V.px) << "," << PFlt(V.py) << "," << PFlt(V.pz) << ","; if (v%4==3) f << "\n"; }
    f << "\n};\n";
    f << "static const float " << S << "vnorm[" << vc*3 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << PFlt(V.nx) << "," << PFlt(V.ny) << "," << PFlt(V.nz) << ","; if (v%4==3) f << "\n"; }
    f << "\n};\n";
    f << "static const float " << S << "vuv[" << vc*2 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << PFlt(V.u) << "," << PFlt(V.v) << ","; if (v%6==5) f << "\n"; }
    f << "\n};\n";
    f << "static const unsigned char " << S << "vbone[" << vc << "] = {\n";
    for (int v = 0; v < vc; v++) { f << rig.verts[v].bone << ","; if (v%24==23) f << "\n"; }
    f << "\n};\n";

    for (int g = 0; g < mc; g++) {
        std::vector<unsigned short> gi;
        for (int t = 0; t < tc; t++) {
            int slot = (t < (int)rig.triMaterial.size()) ? rig.triMaterial[t] : 0;
            if (slot == g) { gi.push_back((unsigned short)rig.indices[t*3+0]);
                             gi.push_back((unsigned short)rig.indices[t*3+1]);
                             gi.push_back((unsigned short)rig.indices[t*3+2]); }
        }
        f << "static const unsigned short " << S << "idx" << g << "[" << (gi.empty()?1:gi.size()) << "] = {";
        for (size_t k = 0; k < gi.size(); k++) { if (k%16==0) f << "\n  "; f << gi[k] << ","; }
        if (gi.empty()) f << "0,";
        f << "\n};\n";
        f << "#define " << D << "IDX" << g << "_COUNT " << gi.size() << "\n";

        const AfnRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : AfnRigMaterial{};
        bool tex = M.textured && M.texW > 0 && !M.pixels.empty();
        if (tex) {
            int n = M.texW * M.texH;
            f << "static const unsigned int __attribute__((aligned(16))) " << S << "tex" << g << "[" << n << "] = {";
            for (int p = 0; p < n; p++) {
                if (p%8==0) f << "\n  ";
                unsigned char idx = (p < (int)M.pixels.size()) ? M.pixels[p] : 0;
                unsigned int c = M.palette[idx];
                if (rig.useAlpha && idx == 0) c &= 0x00FFFFFFu;
                f << "0x" << std::hex << c << std::dec << "u,";
            }
            f << "\n};\n";
        }
        f << "#define " << D << "TEX" << g << "_W " << (tex ? M.texW : 0) << "\n";
        f << "#define " << D << "TEX" << g << "_H " << (tex ? M.texH : 0) << "\n";
    }
    f << "static const unsigned short* const " << S << "idx_ptrs[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << S << "idx" << g << ",";
    f << "};\n";
    f << "static const int " << S << "idx_counts[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << D << "IDX" << g << "_COUNT,";
    f << "};\n";
    f << "static const unsigned int* const " << S << "tex_ptrs[" << mc << "] = {";
    for (int g = 0; g < mc; g++) {
        const AfnRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : AfnRigMaterial{};
        bool tex = M.textured && M.texW > 0 && !M.pixels.empty();
        f << (tex ? (S + "tex" + std::to_string(g)) : std::string("0")) << ",";
    }
    f << "};\n";
    f << "static const int " << S << "tex_w[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << D << "TEX" << g << "_W,";
    f << "};\n";
    f << "static const int " << S << "tex_h[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << D << "TEX" << g << "_H,";
    f << "};\n";

    for (int c = 0; c < cc; c++) {
        const AfnRigClip& cl = rig.clips[c];
        int nf = cl.frameCount;
        f << "static const float " << S << "clip" << c << "[" << (nf*bc*7) << "] = {\n";
        for (int fr = 0; fr < nf; fr++)
            for (int b = 0; b < bc; b++) {
                const AfnRigBonePose& P = cl.frames[fr*bc + b];
                f << PFlt(P.px) << "," << PFlt(P.py) << "," << PFlt(P.pz) << ","
                  << PFlt(P.qw) << "," << PFlt(P.qx) << "," << PFlt(P.qy) << "," << PFlt(P.qz) << ",\n";
            }
        f << "};\n";
    }
    f << "static const float* const " << S << "clip_ptrs[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << S << "clip" << c << ",";
    f << "};\n";
    f << "static const int " << S << "clip_frames[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << rig.clips[c].frameCount << ",";
    f << "};\n";
    f << "static const unsigned char " << S << "clip_loop[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << (rig.clips[c].loop ? 1 : 0) << ",";
    f << "};\n\n";
}

static bool GeneratePSVRigData(const std::string& runtimeDir,
                               const std::vector<AfnRigExport>& rigs,
                               int playerRigIdx,
                               const std::vector<AfnSpriteExport>& sprites,
                               std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\psv_rig.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PS Vita rig data (multi-rig) — GENERATED by Export PSV. Do not edit.\n#pragma once\n#include \"affinity_psv.h\"\n\n";

    auto valid = [&](int ri) {
        return ri >= 0 && ri < (int)rigs.size() && !rigs[ri].verts.empty()
            && !rigs[ri].clips.empty() && rigs[ri].boneCount > 0;
    };
    // Order the used rigs: player first (slot 0), then each distinct NPC rig.
    std::vector<int> used;
    auto useRig = [&](int ri) -> int {
        if (!valid(ri)) return -1;
        for (size_t i = 0; i < used.size(); i++) if (used[i] == ri) return (int)i;
        used.push_back(ri); return (int)used.size() - 1;
    };
    int playerSlot = useRig(playerRigIdx);

    float psx=0, psy=0, psz=0, pscale=1.0f; int pclip=0, playerSprite=-1;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        if (s.spriteType == 1 && s.riggedMeshIdx == playerRigIdx) {
            psx=PVX(s.x); psy=PVY(s.y); psz=PVX(s.z); pscale=s.scale; pclip=s.rigAnimIdx;
            playerSprite=(int)i; break;
        }
    }

    struct Inst { float x,y,z,rot,scale; int clip, slot, sprite; };
    std::vector<Inst> npcs;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        if (s.spriteType == 1) continue;
        int slot = useRig(s.riggedMeshIdx);
        if (slot < 0) continue;
        int clip = (s.rigAnimIdx >= 0 && s.rigAnimIdx < (int)rigs[s.riggedMeshIdx].clips.size()) ? s.rigAnimIdx : 0;
        npcs.push_back({ PVX(s.x), PVY(s.y), PVX(s.z), s.rotation, s.scale, clip, slot, (int)i });
    }

    if (used.empty()) { f << "// (no rigs in this scene)\n"; return true; }

    for (size_t i = 0; i < used.size(); i++) EmitRigArrays(f, (int)i, rigs[used[i]]);

    int maxV=0, maxB=0, maxM=0;
    for (int ri : used) {
        int vc=(int)rigs[ri].verts.size(), bc=rigs[ri].boneCount;
        int mc=(int)rigs[ri].materials.size(); if (mc<1) mc=1;
        if (vc>maxV) maxV=vc; if (bc>maxB) maxB=bc; if (mc>maxM) maxM=mc;
    }
    f << "#define AFN_RIG_COUNT "     << used.size() << "\n";
    f << "#define AFN_RIG_MAX_VERTS " << maxV << "\n";
    f << "#define AFN_RIG_MAX_BONES " << maxB << "\n";
    f << "#define AFN_RIG_MAX_MATS "  << maxM << "\n";
    f << "static const AfnRig afn_rigs[AFN_RIG_COUNT] = {\n";
    for (size_t i = 0; i < used.size(); i++) {
        const auto& rig = rigs[used[i]];
        int mc = (int)rig.materials.size(); if (mc<1) mc=1;
        float ldx=0, ldy=0, ldz=-1.0f; int cl = rig.cameraLight ? 1 : 0;
        if (cl) {
            float ax = rig.lightX*3.14159265f/180.0f, ay = rig.lightY*3.14159265f/180.0f;
            float cx=cosf(ax), sx=sinf(ax), cy=cosf(ay), sy=sinf(ay);
            ldx=-cx*sy; ldy=sx; ldz=-cx*cy;
        }
        std::string S = "afn_rig" + std::to_string(i) + "_";
        f << "  { " << rig.boneCount << ", " << (int)rig.verts.size() << ", " << mc << ", "
          << (int)rig.clips.size() << ", " << rig.cullMode << ", 0.25f, " << cl << ", "
          << PFlt(ldx) << "," << PFlt(ldy) << "," << PFlt(ldz) << ", "
          << PFlt(rig.yawOffset * 3.14159265f / 180.0f) << ", "   // model yaw correction (rad)
          << S << "vpos, " << S << "vnorm, " << S << "vuv, " << S << "vbone, "
          << S << "idx_ptrs, " << S << "idx_counts, " << S << "tex_ptrs, " << S << "tex_w, " << S << "tex_h, "
          << S << "clip_ptrs, " << S << "clip_frames, " << S << "clip_loop },\n";
    }
    f << "};\n\n";

    if (playerSlot >= 0) {
        f << "#define AFN_HAS_PLAYER_RIG 1\n";
        f << "#define AFN_PLAYER_RIG_SLOT "    << playerSlot << "\n";
        f << "#define AFN_PLAYER_START_X "     << PFlt(psx) << "\n";
        f << "#define AFN_PLAYER_START_Y "     << PFlt(psy) << "\n";
        f << "#define AFN_PLAYER_START_Z "     << PFlt(psz) << "\n";
        f << "#define AFN_PLAYER_SCALE "       << PFlt(pscale) << "\n";
        f << "#define AFN_PLAYER_DEFAULT_CLIP " << pclip << "\n";
        f << "#define AFN_PLAYER_SPRITE_IDX "   << playerSprite << "\n";
    }

    // NPC instances: { x, y, z (world px), rotY (deg), scale, clip, rig slot,
    // editor sprite index }. The sprite index lets SetVisible/SetSkelAnim/
    // collision nodes target this NPC.
    f << "#define AFN_NPC_COUNT " << npcs.size() << "\n";
    f << "static const float afn_npc_inst[" << (npcs.empty()?1:npcs.size()) << "][8] = {\n";
    for (const auto& n : npcs)
        f << "  { " << PFlt(n.x) << "," << PFlt(n.y) << "," << PFlt(n.z) << "," << PFlt(n.rot)
          << "," << PFlt(n.scale) << "," << n.clip << "," << n.slot << "," << n.sprite << " },\n";
    if (npcs.empty()) f << "  {0,0,0,0,0,0,0,0},\n";
    f << "};\n";

    // Per-NPC collision box: { hx,hy,hz, cx,cy,cz } in WORLD px — the full glTF
    // bounding box. Half-extents (hx,hy,hz) + center offset (cx,cy,cz) from the
    // rig's authored box (collisionType/colExtents/colCenter, rig-local) * the
    // instance render scale (scale*0.25, matching rig_draw). The runtime uses the
    // true AABB (independent X/Z, center offset honored) for gravity floor-snap
    // and the player-vs-NPC collision/blocker. Default box if none authored.
    f << "static const float afn_npc_col[" << (npcs.empty()?1:npcs.size()) << "][6] = {\n";
    for (const auto& n : npcs) {
        const AfnRigExport& rig = rigs[used[n.slot]];
        float Sc = n.scale * 0.25f;
        float hx, hy, hz, cx, cy, cz;
        if (rig.collisionType == 1) {
            hx = rig.colExtents[0] * Sc; hy = rig.colExtents[1] * Sc; hz = rig.colExtents[2] * Sc;
            cx = rig.colCenter[0]  * Sc; cy = rig.colCenter[1]  * Sc; cz = rig.colCenter[2]  * Sc;
        } else {
            hx = hz = 3.0f * n.scale; hy = 3.0f * n.scale; cx = cz = 0.0f; cy = 3.0f * n.scale;
        }
        f << "  { " << PFlt(hx) << "," << PFlt(hy) << "," << PFlt(hz) << ","
          << PFlt(cx) << "," << PFlt(cy) << "," << PFlt(cz) << " },\n";
    }
    if (npcs.empty()) f << "  {3.0f,3.0f,3.0f,0.0f,3.0f,0.0f},\n";
    f << "};\n";

    // Per-NPC navigation (editor Navigation section on NPC/Enemy objects):
    // { mode (0 off / 1 follow player / 2 wander), speed (world px/frame),
    //   stop distance (world px), repath frames, move clip (-1 = keep) }.
    // Parallel to afn_npc_inst. Speed/distance authored in editor units, /4
    // to world px like every other exported coordinate. The runtime's
    // npc_nav_update() consumes this with the psv_nav.h navmesh blob.
    f << "static const float afn_npc_nav[" << (npcs.empty()?1:npcs.size()) << "][5] = {\n";
    for (const auto& n : npcs) {
        const auto& s = sprites[n.sprite];
        int mclip = (s.navMoveClip >= 0 && s.navMoveClip < (int)rigs[used[n.slot]].clips.size())
                  ? s.navMoveClip : -1;
        f << "  { " << s.navMode << ", " << PFlt(s.navSpeed / 4.0f) << ", "
          << PFlt(s.navStopDist / 4.0f) << ", " << (s.navRepath > 0 ? s.navRepath : 30)
          << ", " << mclip << " },\n";
    }
    if (npcs.empty()) f << "  {0,0,0,30,-1},\n";
    f << "};\n";

    f.close();
    return true;
}

// ---- Navigation mesh (PSV) ------------------------------------------------
// Builds a Recast navmesh from the SAME world-space triangle soup the runtime's
// collide_build() assembles (psv_runtime/main.c): every mesh-instance sprite,
// mesh-local verts /4 (WL), scale -> rotY -> rotX -> rotZ -> translate to the
// instance position (PVX/PVY). The Detour tile blob is emitted into psv_nav.h;
// the runtime loads it through nav_bridge (afn_nav_init) and NPCs with a
// Navigation mode path across it. Skipped (no AFN_HAS_NAVMESH) when no NPC
// has navigation enabled, so scenes without nav pay nothing.
static bool GeneratePSVNav(const std::string& runtimeDir,
                           const std::vector<AfnSpriteExport>& sprites,
                           const std::vector<AfnMeshExport>& meshes,
                           std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\psv_nav.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PS Vita navmesh — GENERATED by Export PSV. Do not edit.\n#pragma once\n\n";

    bool anyNav = false;
    for (const auto& s : sprites) if (s.navMode != 0) { anyNav = true; break; }
    if (!anyNav) { f << "// (no NPC navigation in this scene)\n"; return true; }

    // World-space triangle soup, mirroring collide_build()'s transform exactly.
    std::vector<float> verts;
    std::vector<int> tris;
    std::vector<unsigned char> triFlags;
    const float DEG2RAD = 3.14159265f / 180.0f;
    // Nav bounds boxes (world space, {x0,y0,z0,x1,y1,z1,negate} per slot):
    // ADDITIVE — geometry inside a walkable box is force-marked walkable,
    // inside a NEGATOR box force-marked non-walkable (negators win). Whole
    // scene always participates. Same semantics as BuildNavMeshPreview.
    std::vector<std::array<float,7>> navBoxes;
    for (const auto& s : sprites) {
        if (!s.isNavPlane) continue;
        float hw = s.navPlaneW * 0.5f, hh = s.navPlaneH * 0.5f, hd = s.navPlaneD * 0.5f;
        navBoxes.push_back({ PVX(s.x - hw), PVY(s.y - hh), PVX(s.z - hd),
                             PVX(s.x + hw), PVY(s.y + hh), PVX(s.z + hd),
                             s.navNegate ? 1.0f : 0.0f });
    }
    for (const auto& s : sprites) {
        if (s.meshIdx < 0 || s.meshIdx >= (int)meshes.size()) continue;
        const auto& m = meshes[s.meshIdx];
        std::vector<unsigned int> idx = m.indices;
        for (size_t q = 0; q + 4 <= m.quadIndices.size(); q += 4) {
            unsigned a = m.quadIndices[q], b = m.quadIndices[q+1], c = m.quadIndices[q+2], d = m.quadIndices[q+3];
            idx.push_back(a); idx.push_back(b); idx.push_back(c);
            idx.push_back(a); idx.push_back(c); idx.push_back(d);
        }
        float ry = s.rotation * DEG2RAD, rx = s.rotationX * DEG2RAD, rz = s.rotationZ * DEG2RAD;
        float cY = cosf(ry), sY = sinf(ry), cX = cosf(rx), sX = sinf(rx), cZ = cosf(rz), sZ = sinf(rz);
        float px = PVX(s.x), py = PVY(s.y), pz = PVX(s.z);
        for (size_t t = 0; t + 3 <= idx.size(); t += 3) {
            float wp[9];
            bool bad = false;
            for (int k = 0; k < 3 && !bad; k++) {
                unsigned vi = idx[t + k];
                if ((vi + 1) * 3 > m.positions.size()) { bad = true; break; }
                // mesh-local /4 (WL) then the runtime's transform order
                float lx = m.positions[vi*3+0] / 4.0f * s.scale;
                float ly = m.positions[vi*3+1] / 4.0f * s.scale;
                float lz = m.positions[vi*3+2] / 4.0f * s.scale;
                float ax = lx*cY + lz*sY, az = -lx*sY + lz*cY, ay = ly;
                float ay2 = ay*cX - az*sX, az2 = ay*sX + az*cX;
                float ax2 = ax*cZ - ay2*sZ, ay3 = ax*sZ + ay2*cZ;
                wp[k*3+0] = px + ax2; wp[k*3+1] = py + ay3; wp[k*3+2] = pz + az2;
            }
            if (bad) continue;
            // Box test: WALKABLE boxes flag the tri force-walkable; negator
            // boxes carve per-voxel inside NavMeshBuild (rcMarkBoxArea).
            unsigned char flag = 0;
            for (int k = 0; k < 3 && !flag; k++)
                for (const auto& b : navBoxes)
                    if (b[6] == 0.0f &&
                        wp[k*3+0] >= b[0] && wp[k*3+0] <= b[3] &&
                        wp[k*3+1] >= b[1] && wp[k*3+1] <= b[4] &&
                        wp[k*3+2] >= b[2] && wp[k*3+2] <= b[5]) { flag = 2; break; }
            int base = (int)verts.size() / 3;
            for (int k = 0; k < 9; k++) verts.push_back(wp[k]);
            tris.push_back(base); tris.push_back(base + 1); tris.push_back(base + 2);
            triFlags.push_back(flag);
        }
    }

    if (tris.empty()) {
        f << "// (navigation requested but no mesh geometry to build from)\n";
        return true;
    }

    // Negator boxes, packed for the per-voxel carve.
    std::vector<float> negBoxes;
    for (const auto& b : navBoxes)
        if (b[6] != 0.0f) {
            negBoxes.push_back(b[0]); negBoxes.push_back(b[1]); negBoxes.push_back(b[2]);
            negBoxes.push_back(b[3]); negBoxes.push_back(b[4]); negBoxes.push_back(b[5]);
        }

    std::vector<uint8_t> blob;
    bool built = NavMeshBuild(verts.data(), (int)verts.size() / 3,
                              tris.data(), (int)tris.size() / 3,
                              NavMeshParams{}, triFlags.data(),
                              negBoxes.empty() ? nullptr : negBoxes.data(), (int)negBoxes.size() / 6)
              && NavMeshSaveBinary(blob);
    NavMeshClear();
    if (!built || blob.empty()) {
        // Non-fatal: the export still succeeds, NPCs just won't path.
        f << "// (navmesh build FAILED — check geometry / NavMeshParams scale)\n";
        return true;
    }

    f << "#define AFN_HAS_NAVMESH 1\n";
    f << "static const int afn_navmesh_bin_size = " << (int)blob.size() << ";\n";
    f << "static const unsigned char __attribute__((aligned(16))) afn_navmesh_bin[" << blob.size() << "] = {";
    for (size_t i = 0; i < blob.size(); i++) {
        if (i % 24 == 0) f << "\n  ";
        f << (unsigned)blob[i] << ",";
    }
    f << "\n};\n";
    f.close();
    return true;
}

// ---- Grind rail paths (PSV) -----------------------------------------------
// Hand-authored per-sprite centerlines. Float port of the NDS emission
// (nds_package.cpp:269-318): afn_rail_pts holds all points concatenated in world
// px; afn_rail_start/count index per editor sprite; *_end/_bounce are per-point
// terminus flags; afn_rail_spline = follow a smooth Catmull-Rom curve.
static bool GeneratePSVRail(const std::string& runtimeDir,
                            const std::vector<AfnSpriteExport>& sprites,
                            std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\psv_rail.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PS Vita grind rails — GENERATED by Export PSV. Do not edit.\n#pragma once\n\n";

    int totalRailPts = 0;
    for (const auto& s : sprites) totalRailPts += (int)s.railPath.size();
    if (totalRailPts <= 0) { f << "// (no grind rails in this scene)\n"; return true; }

    f << "#define AFN_HAS_RAIL_PATH 1\n";
    f << "static const float afn_rail_pts[" << totalRailPts << "][3] = {\n";
    std::vector<int> railStart(sprites.size(),0), railCount(sprites.size(),0);
    std::vector<int> railEnd, railBounce;
    int running = 0;
    for (size_t i = 0; i < sprites.size(); i++) {
        railStart[i] = running;
        railCount[i] = (int)sprites[i].railPath.size();
        for (auto& p : sprites[i].railPath) {
            f << "  { " << PFlt(PVX(p[0])) << ", " << PFlt(PVY(p[1])) << ", " << PFlt(PVX(p[2])) << " },\n";
            railEnd.push_back((p[3] != 0.0f || p[5] != 0.0f) ? 1 : 0);   // End or Start = clean terminus
            railBounce.push_back(p[4] != 0.0f ? 1 : 0);                  // bumper terminus
            running++;
        }
    }
    f << "};\n";
    f << "static const unsigned char afn_rail_pt_end[" << totalRailPts << "] = {";
    for (size_t k = 0; k < railEnd.size(); k++) f << (k?",":"") << railEnd[k];
    f << "};\n";
    f << "static const unsigned char afn_rail_pt_bounce[" << totalRailPts << "] = {";
    for (size_t k = 0; k < railBounce.size(); k++) f << (k?",":"") << railBounce[k];
    f << "};\n";
    int sc = (int)sprites.size(); if (sc < 1) sc = 1;
    f << "static const int afn_rail_start[" << sc << "] = {";
    for (size_t i = 0; i < sprites.size(); i++) f << (i?",":"") << railStart[i];
    if (sprites.empty()) f << "0";
    f << "};\n";
    f << "static const int afn_rail_count[" << sc << "] = {";
    for (size_t i = 0; i < sprites.size(); i++) f << (i?",":"") << railCount[i];
    if (sprites.empty()) f << "0";
    f << "};\n";
    f << "static const unsigned char afn_rail_spline[" << sc << "] = {";
    for (size_t i = 0; i < sprites.size(); i++) f << (i?",":"") << (sprites[i].railSpline ? 1 : 0);
    if (sprites.empty()) f << "0";
    f << "};\n";
    f.close();
    return true;
}

// ---- HUD overlay (PSV) ----------------------------------------------------
// 2D screen-space elements authored in GBA-native 240x160 (CLAUDE.md). Each
// element holds pieces (sprite-asset sub-images), text rows, and an optional
// cursor + stops (menu nav). Pieces are emitted as RGBA atlases; the runtime
// (psv_hud renderer in main.c) draws them in an ortho pass scaled to the Vita
// screen. Static positions (keyframe/anim-layer animation is not ported).
static bool GeneratePSVHud(const std::string& runtimeDir,
                           const std::vector<AfnHudElementExport>& elems,
                           const std::vector<AfnSpriteAssetExport>& assets,
                           std::string& errorMsg,
                           std::vector<int>& nodeLayerRemap,
                           std::vector<int>& nodeLayerCount) {
    std::string path = runtimeDir + "\\include\\psv_hud.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PS Vita HUD — GENERATED by Export PSV. Do not edit.\n#pragma once\n\n";
    if (elems.empty()) { f << "// (no HUD elements in this scene)\n"; return true; }
    f << "#define AFN_HAS_HUD 1\n";

    // Flatten pieces/texts/stops across elements; emit each piece's RGBA frame.
    struct P { int x, y, w, h, tex, black, opacity, barSrc, barAxis, barStart, barEnd, cycleSlot, cycleCount, xfToScene, xfToElem, xfToPiece; };
    struct T { int x, y; unsigned int color; int font, slot, pad, scale; std::string text; };
    struct S { int x, y, link; };
    std::vector<P> pieces; std::vector<T> texts; std::vector<S> stops;
    std::vector<int> frameW, frameH;
    std::vector<int> cycleOffX, cycleOffY;  // per-frame position offset (cycle frame slots; 0 otherwise)
    int frameCount = 0;
    auto emitFrame = [&](int ai, int frame, int sz) -> std::pair<int,int> {
        // RGBA frame data is emitted as a byte STRING LITERAL (octal-escaped),
        // not a {0x..,..} initializer list: GCC parses a multi-MB string literal
        // orders of magnitude faster than that many uint constant-expressions,
        // which is what made the Vita build crawl for 512/960 HUD frames. The
        // runtime uploads it straight to glTexImage2D (GL_RGBA, bytes [R,G,B,A]).
        if (ai < 0 || ai >= (int)assets.size() || frame < 0 || frame >= (int)assets[ai].frames.size())
            { f << "static const unsigned char afn_hud_f" << frameCount << "[] = \"\\000\\000\\000\\000\";\n"; frameCount++; frameW.push_back(1); frameH.push_back(1); cycleOffX.push_back(0); cycleOffY.push_back(0); return {1,1}; }
        const auto& a = assets[ai];
        // PSV higher-color path: when the asset is >16 colors, emit from the
        // re-quantized psvFrames/psvPalette (up to 128 colors) instead of the
        // 16-color palette. Frame counts match (same source strip).
        bool usePsv = (a.psvColors > 16 && frame < (int)a.psvFrames.size());
        const auto& fr = usePsv ? a.psvFrames[frame] : a.frames[frame];
        const uint32_t* pal = usePsv ? a.psvPalette : a.palette;
        int palMask = usePsv ? 127 : 15;
        int w = fr.width > 0 ? fr.width : (sz>0?sz:a.baseSize);
        int h = fr.height > 0 ? fr.height : (sz>0?sz:a.baseSize);
        frameW.push_back(w); frameH.push_back(h); cycleOffX.push_back(0); cycleOffY.push_back(0);
        // Soft alpha: when the asset opted into Use Alpha, carry the source PNG's
        // per-pixel alpha so a HUD piece's feathered edges blend instead of the
        // hard 50% cutout (jagged edges). Else binary (idx 0 = transparent).
        bool softAlpha = a.useAlpha && (int)fr.alpha.size() >= w * h;
        f << "static const unsigned char afn_hud_f" << frameCount << "[] =\n  \"";
        int run = 0;
        for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
            unsigned char idx = fr.pixels[py * kExportMaxFrameSize + px];
            unsigned c = (idx == 0) ? 0u : pal[idx & palMask];
            if (softAlpha && idx != 0)
                c = (c & 0x00FFFFFFu) | ((unsigned)fr.alpha[py * w + px] << 24);
            unsigned bb[4] = { c & 0xFF, (c>>8) & 0xFF, (c>>16) & 0xFF, (c>>24) & 0xFF };
            for (int k = 0; k < 4; k++)
                f << '\\' << ((bb[k]>>6)&7) << ((bb[k]>>3)&7) << (bb[k]&7);  // \ooo octal byte
            if (++run >= 64) { f << "\"\n  \""; run = 0; }   // split into concatenated literals
        }
        f << "\";\n";
        frameCount++; return {w,h};
    };

    // Per-element ranges.
    struct E { int x,y,mode; unsigned int sceneMask, sceneMask2D; int pS,pC,tS,tC,sS,sC,curTex,curX,curY,curSize,trackCursor,curElemRef; bool startVis; };
    std::vector<E> es;
    for (const auto& he : elems) {
        E e; e.x = he.screenX; e.y = he.screenY; e.mode = he.runtimeMode;
        e.sceneMask = he.mode4SceneMask;     // 3D-scene gate
        e.sceneMask2D = he.mode0SceneMask;   // 2D-scene gate (used when booted in 2D mode)
        e.startVis = he.visible;
        e.pS = (int)pieces.size();
        for (const auto& pc : he.pieces) {
            // Draw size = the piece's chosen size (matches the editor canvas), so a
            // native-512 graphic set to 256 renders at 256 (GL scales the texture).
            // Non-square presets: 960=960x544, 640=512x256, 384=256x128, 192=128x64.
            int dispW = (pc.size == 960) ? 960 : (pc.size == 640) ? 512 : (pc.size == 384) ? 256 : (pc.size == 192) ? 128 : pc.size;
            int dispH = (pc.size == 960) ? 544 : (pc.size == 640) ? 256 : (pc.size == 384) ? 128 : (pc.size == 192) ?  64 : pc.size;
            // Cycle ← Value: bake each staged frame-slot asset so the runtime can
            // pick tex = base + hud_value[slot]. A static piece bakes just one.
            int cyc, baseTex = frameCount;
            if (pc.cycleSlot >= 0 && !pc.cycleAssets.empty()) {
                for (size_t k = 0; k < pc.cycleAssets.size(); k++) {
                    int a = pc.cycleAssets[k];
                    emitFrame(a >= 0 ? a : pc.spriteAssetIdx, pc.frame, pc.size);
                    if (k < pc.cycleX.size()) cycleOffX.back() = pc.cycleX[k];   // attach per-slot offset
                    if (k < pc.cycleY.size()) cycleOffY.back() = pc.cycleY[k];
                }
                cyc = (int)pc.cycleAssets.size();
            } else {
                emitFrame(pc.spriteAssetIdx, pc.frame, pc.size);   // texture stays native res
                cyc = 1;
            }
            pieces.push_back({ pc.localX, pc.localY, dispW, dispH, baseTex, pc.blackTint?1:0, pc.opacity, pc.barSource, pc.barAxis, pc.barStart, pc.barEnd, pc.cycleSlot >= 0 && !pc.cycleAssets.empty() ? pc.cycleSlot : -1, cyc, pc.xfToScene, pc.xfToElem, pc.xfToPiece });
        }
        e.pC = (int)pieces.size() - e.pS;
        e.tS = (int)texts.size();
        for (const auto& tr : he.textRows) {
            unsigned r5 = tr.colorRGB15 & 0x1F, g5=(tr.colorRGB15>>5)&0x1F, b5=(tr.colorRGB15>>10)&0x1F;
            unsigned col = 0xFF000000u | ((b5<<3)<<16) | ((g5<<3)<<8) | (r5<<3);
            std::string s; for (int i=0;i<31 && tr.text[i];i++){ char c=tr.text[i]; s += (c>=32&&c<127)?c:'?'; }
            texts.push_back({ tr.localX, tr.localY, col, tr.font, tr.sourceSlot, tr.pad, tr.scale<1?1:tr.scale, s });
        }
        e.tC = (int)texts.size() - e.tS;
        e.sS = (int)stops.size();
        for (const auto& st : he.stops) stops.push_back({ st.localX, st.localY, st.linkedElement });
        e.sC = (int)stops.size() - e.sS;
        e.curTex = -1; e.curX = he.cursorOffX; e.curY = he.cursorOffY; e.curSize = he.cursorSize;
        e.curElemRef = he.cursorElementIdx; e.trackCursor = -1;
        // An element-cursor suppresses this (menu) element's single-sprite cursor.
        if (he.cursorAssetIdx >= 0 && he.cursorElementIdx < 0) { emitFrame(he.cursorAssetIdx, he.cursorFrame, 0); e.curTex = frameCount-1; }
        es.push_back(e);
    }
    // Resolve cursor-element references: the referenced (pointer) element tracks
    // the menu element's active cursor stop, rendered there with its own keyframes.
    for (int mi = 0; mi < (int)es.size(); mi++) {
        int ref = es[mi].curElemRef;
        if (ref >= 0 && ref < (int)es.size()) es[ref].trackCursor = mi;
    }

    // Frame pointer + size tables.
    f << "static const unsigned char* const afn_hud_frames[" << (frameCount?frameCount:1) << "] = {";
    for (int i=0;i<frameCount;i++) f << "afn_hud_f" << i << ","; if(!frameCount) f << "0,"; f << "};\n";
    f << "#define AFN_HUD_FRAME_COUNT " << (frameCount?frameCount:1) << "\n";
    f << "static const short afn_hud_frame_w[" << (frameCount?frameCount:1) << "] = {";
    for (int i=0;i<frameCount;i++) f << frameW[i] << ","; if(!frameCount) f << "1,"; f << "};\n";
    f << "static const short afn_hud_frame_h[" << (frameCount?frameCount:1) << "] = {";
    for (int i=0;i<frameCount;i++) f << frameH[i] << ","; if(!frameCount) f << "1,"; f << "};\n";
    // Per-frame position offset (set on cycle frame slots; 0 elsewhere). Indexed by
    // texture so the runtime can shift a cycled piece by the active frame's offset.
    f << "#define AFN_HUD_PIECE_CYCLE_OFF 1\n";
    f << "static const short afn_hud_cycle_off_x[" << (frameCount?frameCount:1) << "] = {";
    for (int i=0;i<frameCount;i++) f << cycleOffX[i] << ","; if(!frameCount) f << "0,"; f << "};\n";
    f << "static const short afn_hud_cycle_off_y[" << (frameCount?frameCount:1) << "] = {";
    for (int i=0;i<frameCount;i++) f << cycleOffY[i] << ","; if(!frameCount) f << "0,"; f << "};\n";
    // NOTE: struct/array names (afn_hud_elems[].stopStart/.stopCount,
    // afn_hud_stops[].link) match what the shared node emitter (node_script_emit
    // .cpp) writes into psv_script.h for ShowHUD/CursorUp/CursorDown, so those
    // nodes compile against this header.
    f << "#define AFN_HUD_ELEM_COUNT " << es.size() << "\n";
    f << "#define AFN_HUD_MODE0_MASK 1\n";   // AfnHudElem carries sceneMask2D (Mode 0 / 2D scene gate)
    f << "typedef struct { short screenX,screenY; unsigned char mode; unsigned int sceneMask,sceneMask2D; short pieceStart,pieceCount,textStart,textCount,stopStart,stopCount; short curTex,curX,curY,curSize,trackCursor; unsigned char startVis; } AfnHudElem;\n";
    f << "#define AFN_HUD_CURSOR_SIZE 1\n";   // AfnHudElem carries curSize (cursor draw square)
    f << "#define AFN_HUD_CURSOR_ELEM 1\n";   // ...and trackCursor (render this element at menu N's cursor stop)
    f << "static const AfnHudElem afn_hud_elems[" << es.size() << "] = {\n";
    for (auto& e : es)
        f << "  { " << e.x << "," << e.y << "," << e.mode << "," << e.sceneMask << "u," << e.sceneMask2D << "u," << e.pS << "," << e.pC
          << "," << e.tS << "," << e.tC << "," << e.sS << "," << e.sC << "," << e.curTex << "," << e.curX << "," << e.curY << "," << e.curSize << "," << e.trackCursor
          << "," << (e.startVis?1:0) << " },\n";
    f << "};\n";
    f << "#define AFN_HUD_PIECE_TINT 1\n";   // AfnHudPiece carries black + opacity (per-piece tint)
    f << "#define AFN_HUD_PIECE_BAR 1\n";    // ...and the bar-fill drain fields (barSrc/axis/start/end)
    f << "typedef struct { short x,y,w,h,tex; unsigned char black; short opacity; short barSrc,barAxis,barStart,barEnd,cycleSlot,cycleCount,xfToScene,xfToElem,xfToPiece; } AfnHudPiece;\n";
    f << "#define AFN_HUD_PIECE_CYCLE 1\n";   // AfnHudPiece carries cycleSlot/cycleCount (asset ← hud_value)
    f << "#define AFN_HUD_PIECE_XFADE 1\n";   // ...and xfTo* (crossfade into element xfToElem's piece xfToPiece on the change to xfToScene)
    f << "static const AfnHudPiece afn_hud_piece[" << (pieces.empty()?1:pieces.size()) << "] = {\n";
    for (auto& p : pieces) f << "  { " << p.x << "," << p.y << "," << p.w << "," << p.h << "," << p.tex << "," << p.black << "," << p.opacity
                              << "," << p.barSrc << "," << p.barAxis << "," << p.barStart << "," << p.barEnd << "," << p.cycleSlot << "," << p.cycleCount << "," << p.xfToScene << "," << p.xfToElem << "," << p.xfToPiece << " },\n";
    if (pieces.empty()) f << "  {0,0,0,0,0,0,16,0,0,0,0,-1,1,-1,-1,-1},\n";
    f << "};\n";
    f << "typedef struct { short x,y; unsigned int color; unsigned char font,slot,pad,scale; char text[32]; } AfnHudText;\n";
    f << "static const AfnHudText afn_hud_text[" << (texts.empty()?1:texts.size()) << "] = {\n";
    for (auto& t : texts) {
        std::string esc; for (char c : t.text) { if (c=='"'||c=='\\') esc+='\\'; esc+=c; }
        f << "  { " << t.x << "," << t.y << ",0x" << std::hex << t.color << std::dec << "u," << t.font << ","
          << t.slot << "," << t.pad << "," << t.scale << ",\"" << esc << "\" },\n";
    }
    if (texts.empty()) f << "  {0,0,0,0,0,0,1,\"\"},\n";
    f << "};\n";
    f << "typedef struct { short x,y,link; } AfnHudStop;\n";
    f << "static const AfnHudStop afn_hud_stops[" << (stops.empty()?1:stops.size()) << "] = {\n";
    for (auto& s : stops) f << "  { " << s.x << "," << s.y << "," << s.link << " },\n";
    if (stops.empty()) f << "  {0,0,-1},\n";
    f << "};\n";

    // ---- Keyframe anim layers (timeline animation on pieces) ----
    // Each layer carries interp/loop/speed/length + keyframes (frame, offset,
    // rotation deg, scale 8.8); afn_hud_piece_layer maps each GLOBAL piece
    // index to its layer (-1 = static). The runtime advances a vframe counter,
    // derives the playhead (vframes / speed, looped or clamped to length),
    // interpolates the surrounding keyframes and draws the piece offset/
    // scaled/rotated about its center. Piece items only for now (sprite/text/
    // cursor items in a layer are ignored on PSV).
    nodeLayerRemap.clear(); nodeLayerCount.clear();   // out: editor-layer flat index -> first runtime track + count (PlayHudAnim node remap)
    {
        struct KF { int frame, ox, oy, rot, sx, sy, hidden, op; };
        struct LY { int interp, loop, speed, step, length, kfStart, kfCount; };
        std::vector<KF> kfs;
        std::vector<LY> layers;
        std::vector<int> pieceLayer(pieces.size(), -1);
        std::vector<int> elemFirstLayer(elems.size(), -1);  // element idx -> its first global anim layer (drive-through)
        for (size_t ei = 0; ei < elems.size(); ei++) {
            for (const auto& lay : elems[ei].animLayers) {
                // Each item in a layer carries its OWN keyframe track; emit one
                // runtime layer (track) per animated PIECE item and point that
                // piece at it. Layer timing (interp/loop/speed/length) is shared
                // across the group. Sprite/text/cursor items are ignored on PSV.
                int layFirst = -1, layCount = 0;   // first runtime track + #tracks of THIS editor-layer (node remap)
                for (const auto& it : lay.items) {
                    if (it.keyframes.empty()) continue;
                    if (it.type != 0 || it.index < 0 || it.index >= es[ei].pC) continue;
                    LY L;
                    L.interp = lay.interp;
                    L.loop = lay.loop ? 1 : 0;
                    L.speed = lay.speed > 0 ? lay.speed : 1;
                    L.step  = lay.step  > 0 ? lay.step  : 1;   // frames/tick for fps>60
                    L.length = lay.length > 0 ? lay.length : 1;
                    L.kfStart = (int)kfs.size();
                    for (const auto& k : it.keyframes)
                        kfs.push_back({ k.frame, k.offX, k.offY, k.rot, k.scaleX, k.scaleY, k.hidden, k.opacity });
                    L.kfCount = (int)kfs.size() - L.kfStart;
                    int li = (int)layers.size();
                    layers.push_back(L);
                    if (elemFirstLayer[ei] < 0) elemFirstLayer[ei] = li;   // first anim track of this element (drive-through)
                    if (layFirst < 0) layFirst = li;
                    layCount++;
                    pieceLayer[es[ei].pS + it.index] = li;
                }
                // One remap entry per editor-layer, in the SAME element/layer order the
                // editor enumerates for a node's layer dropdown -> the layer's first
                // runtime track + the number of tracks (so a node drives ALL its items).
                nodeLayerRemap.push_back(layFirst);
                nodeLayerCount.push_back(layCount);
            }
        }
        if (!layers.empty()) {
            f << "#define AFN_HAS_HUD_ANIM 1\n";
            f << "#define AFN_HUD_LAYER_COUNT " << layers.size() << "\n";
            f << "#define AFN_HUD_KF_HIDE 1\n";   // AfnHudKf carries the per-keyframe hide flag (blink)
            f << "#define AFN_HUD_KF_OPACITY 1\n"; // ...and the per-keyframe opacity multiplier (glow pulse)
            f << "typedef struct { short frame,ox,oy,rot,sx,sy,hide,op; } AfnHudKf;\n";
            f << "static const AfnHudKf afn_hud_kf[" << kfs.size() << "] = {\n";
            for (auto& k : kfs)
                f << "  { " << k.frame << "," << k.ox << "," << k.oy << "," << k.rot
                  << "," << k.sx << "," << k.sy << "," << k.hidden << "," << k.op << " },\n";
            f << "};\n";
            f << "typedef struct { unsigned char interp,loop; short speed,step,length,kfStart,kfCount; } AfnHudLayer;\n";
            f << "static const AfnHudLayer afn_hud_layer[" << layers.size() << "] = {\n";
            for (auto& L : layers)
                f << "  { " << L.interp << "," << L.loop << "," << L.speed << "," << L.step << "," << L.length
                  << "," << L.kfStart << "," << L.kfCount << " },\n";
            f << "};\n";
            f << "static const short afn_hud_piece_layer[" << (pieces.empty()?1:pieces.size()) << "] = {";
            for (size_t i = 0; i < pieceLayer.size(); i++) f << pieceLayer[i] << ",";
            if (pieces.empty()) f << "-1,";
            f << "};\n";
            // Element -> first anim layer, for attached sub-sprites that "drive through
            // element" (run an element's rotation/scale keyframes on their own graphic).
            f << "static const short afn_hud_elem_first_layer[" << (elemFirstLayer.empty()?1:elemFirstLayer.size()) << "] = {";
            for (size_t i = 0; i < elemFirstLayer.size(); i++) f << elemFirstLayer[i] << ",";
            if (elemFirstLayer.empty()) f << "-1,";
            f << "};\n";
        }
    }
    f.close();
    return true;
}

bool PackagePSV(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<AfnSpriteExport>& sprites,
                const std::vector<AfnSpriteAssetExport>& assets,
                const AfnCameraExport& camera,
                const std::vector<AfnMeshExport>& meshes,
                float orbitDist,
                const std::vector<AfnSoundSampleExport>& soundSamples,
                const std::vector<AfnSoundInstanceExport>& soundInstances,
                const std::vector<AfnSkyFrameExport>& skyFrames,
                const AfnScriptExport& script,
                const std::vector<AfnBlueprintExport>& blueprints,
                const std::vector<AfnBlueprintInstanceExport>& bpInstances,
                const std::vector<AfnHudElementExport>& hudElements,
                const std::vector<AfnTmSceneExport>& /*tmScenes*/,
                int startMode,
                float /*midiMasterDb*/,
                const std::vector<AfnRiggedMeshExport>& /*rigs*/,
                const std::vector<AfnRigExport>& pspRigs,
                int playerRigIdx,
                std::string& errorMsg) {
    { std::lock_guard<std::mutex> lk(g_psvBuildLogMtx); g_psvBuildLog.clear(); }   // reset the compile terminal
    // 1) Regenerate the shared headers (mapdata/sky/sprites/sound/player), but
    //    SKIP the single-rig generator (emitRig=false) — PSV emits its own
    //    multi-rig psv_rig.h so NPCs/enemies render their own models.
    PsvBuildLog("Generating data headers (sprites, sound, sky)...");
    if (!GenerateAffinityHeaders(runtimeDir, "psv_", "affinity_psv.h",
                                 sprites, assets, camera, meshes, orbitDist,
                                 soundSamples, soundInstances, skyFrames,
                                 pspRigs, playerRigIdx, errorMsg, /*emitRig=*/false))
        return false;
    PsvBuildLog("Generating rig data...");
    if (!GeneratePSVRigData(runtimeDir, pspRigs, playerRigIdx, sprites, errorMsg))
        return false;
    PsvBuildLog("Generating grind rails...");
    if (!GeneratePSVRail(runtimeDir, sprites, errorMsg))
        return false;
    PsvBuildLog("Baking navmesh...");
    if (!GeneratePSVNav(runtimeDir, sprites, meshes, errorMsg))
        return false;
    PsvBuildLog("Generating HUD...");
    std::vector<int> hudNodeLayerRemap, hudNodeLayerCount;   // node flat-layer index -> runtime afn_hud_layer[] range
    if (!GeneratePSVHud(runtimeDir, hudElements, assets, errorMsg, hudNodeLayerRemap, hudNodeLayerCount))
        return false;

    PsvBuildLog("Emitting script...");
    // 1b) Emit the visual-script node graph (shared NDS/PSV codegen) -> psv_script.h.
    {
        std::string sp = runtimeDir + "\\include\\psv_script.h";
        std::ofstream sf(sp);
        if (!sf) { errorMsg = "Could not open " + sp + " for writing"; return false; }
        sf << "// Affinity PS Vita script (node graph -> C). GENERATED by Export PSV.\n#pragma once\n";
        if (!script.nodes.empty() || !blueprints.empty()) sf << "#define AFN_HAS_SCRIPT 1\n";
        EmitNodeScriptBodies(sf, script, blueprints, bpInstances, sprites, soundInstances, hudNodeLayerRemap, hudNodeLayerCount);
        // Boot mode/scene from the tab the build was started on: startMode is
        // 0 = 3D (Mode 4), 1 = 2D menu (Mode 0), 2 = Mode 1. The runtime seeds
        // afn_current_mode/scene from these so it boots straight into a 2D menu.
        sf << "#define AFN_START_MODE " << startMode << "\n";
        sf << "#define AFN_START_SCENE 0\n";
    }

    PsvBuildLog("Compiling (cmake + make)...\n");
    // 2) Build affinity_psv.vpk via the VitaSDK CMake toolchain. cmake re-runs
    //    are cheap on an already-configured build dir, and CMake tracks header
    //    deps, so a regenerated psv_mapdata.h forces main.c to recompile.
    std::string msysDir = ToMsysPath(runtimeDir);
    std::string buildCmd =
        "export VITASDK=/c/vitasdk; export PATH=\\\"$VITASDK/bin:$PATH\\\"; "
        "cd '" + msysDir + "' && mkdir -p build && cd build && "
        "cmake .. && make -j$(nproc) 2>&1";
    std::string out;
    int rc = RunMsysBash(buildCmd, out);
    if (rc != 0) {
        errorMsg = "psv_*.h headers generated, but the Vita build failed (rc="
                 + std::to_string(rc) + "):\n" + out
                 + "\n\nIf VitaSDK isn't set up, the headers are still written — build manually with cmake/make in psv_runtime.";
        return false;
    }

    // 3) Copy the vpk to the requested output path if it differs from where the
    //    build wrote it (build/affinity_psv.vpk).
    std::string builtVpk = msysDir + "/build/affinity_psv.vpk";
    if (!outputPath.empty()) {
        std::string dst = ToMsysPath(outputPath);
        if (dst != builtVpk) {
            std::string cp = "cp '" + builtVpk + "' '" + dst + "' 2>&1";
            std::string cpout;
            RunMsysBash(cp, cpout);   // best-effort
        }
    }
    return true;
}

} // namespace Affinity
