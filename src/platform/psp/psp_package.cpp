// Affinity PSP exporter — generates psp_runtime/include/psp_mapdata.h (float
// geometry + RGBA8888 textures) and builds EBOOT.PBP via the pspdev toolchain
// (invoked through WSL, since pspdev ships Linux binaries).
//
// First pass scope: the Mode 4 3D scene — meshes, mesh instances (sprites that
// reference a mesh), camera start, and textures. Sprites/HUD/scripts/sound are
// accepted in the signature and ignored for now (filled in over later passes).
//
// World units: "world px" = editorCoord/4, with a +512 offset on X/Z (matches
// the NDS exporter's EditorToFixed minus the 8.8 fixed-point shift), so the PSP
// scene proportions line up with what the editor/NDS show.

#include "psp_package.h"

#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Affinity {

// ---- helpers --------------------------------------------------------------
static std::string ToWslPath(const std::string& winPath) {
    // C:\a\b -> /mnt/c/a/b
    std::string p = winPath;
    for (auto& c : p) if (c == '\\') c = '/';
    if (p.size() >= 2 && p[1] == ':') {
        char drive = (char)tolower(p[0]);
        p = "/mnt/" + std::string(1, drive) + p.substr(2);
    }
    return p;
}

#ifdef _WIN32
// Run a command inside WSL, capturing stdout+stderr. Returns exit code, or -1
// if WSL itself couldn't be launched.
static int RunWslCommand(const std::string& shellCmd, std::string& output) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    // Give wsl.exe a real (NUL) stdin. The editor is a GUI app with no console,
    // so GetStdHandle(STD_INPUT_HANDLE) is NULL and wsl.exe blocks on it forever
    // (the "stuck building" hang). NUL returns EOF immediately.
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite; si.hStdError = hWrite;
    si.hStdInput = hNul;

    PROCESS_INFORMATION pi = {};
    std::string cmdLine = "wsl.exe bash -lc \"" + shellCmd + "\"";
    BOOL ok = CreateProcessA(nullptr, (LPSTR)cmdLine.c_str(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!ok) { CloseHandle(hRead); output = "Failed to launch WSL (is it installed?)"; return -1; }

    // Drain stdout by POLLING, not by waiting for pipe EOF: wsl.exe leaves a
    // relay/VM process holding the inherited write-handle open after the build
    // finishes, so a plain ReadFile-until-EOF loop blocks forever (the "endless
    // building" hang) even though make already exited. Instead, read whatever is
    // available and stop once the wsl process itself has exited.
    output.clear();
    char buf[512];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD n = 0;
            DWORD toRead = avail < (sizeof(buf) - 1) ? avail : (sizeof(buf) - 1);
            if (ReadFile(hRead, buf, toRead, &n, nullptr) && n > 0) { buf[n] = 0; output += buf; }
        } else if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            // Process exited; one last peek to flush any final bytes, then done.
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
static int RunWslCommand(const std::string&, std::string& output) { output = "WSL build only supported on Windows host"; return -1; }
#endif

// editor coords -> PSP world px
static float WX(float e) { return (e + 512.0f) / 4.0f; }   // X/Z
static float WY(float h) { return h / 4.0f; }               // height
static float WL(float p) { return p / 4.0f; }               // mesh-local

// Format a float as a valid C float literal. Default stream formatting drops
// the decimal point on whole numbers ("128"), and "128f" is not a valid
// literal — so ensure a '.' (or exponent) is present before the 'f' suffix.
static std::string Flt(float v) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.6g", v);
    std::string s = buf;
    if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
    return s + "f";
}

// RGB15 (5/5/5, index from texPalette) -> 0xAABBGGRR (PSP GU_PSM_8888)
static unsigned int Rgb15ToAbgr(unsigned short c, bool opaque) {
    unsigned r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
    unsigned r = (r5 << 3) | (r5 >> 2);
    unsigned g = (g5 << 3) | (g5 >> 2);
    unsigned b = (b5 << 3) | (b5 >> 2);
    unsigned a = opaque ? 0xFF : 0x00;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// ---- header generation ----------------------------------------------------
static bool GeneratePSPMapData(const std::string& runtimeDir,
                               const char* hdrPrefix, const char* dataInclude,
                               const std::vector<GBASpriteExport>& sprites,
                               const GBACameraExport& camera,
                               const std::vector<GBAMeshExport>& meshes,
                               float orbitDist,
                               std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "mapdata.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }

    f << "// Affinity scene data — GENERATED by the editor exporter. Do not edit.\n";
    f << "#pragma once\n#include \"" << dataInclude << "\"\n\n";

    // ---- meshes ----
    for (size_t mi = 0; mi < meshes.size(); mi++) {
        const auto& m = meshes[mi];
        int vc = (int)m.positions.size() / 3;
        bool textured = m.textured && m.texW > 0 && !m.texPixels.empty();
        bool opaqueTex = !m.textureHasAlpha;

        // Vertices (interleaved AfnVertex).
        f << "static const AfnVertex afn_mesh" << mi << "_verts[" << (vc > 0 ? vc : 1) << "] = {\n";
        for (int v = 0; v < vc; v++) {
            float u = 0.0f, vv = 0.0f;
            if (textured && (int)m.uvs.size() >= (v + 1) * 2) {
                u = m.uvs[v*2+0];
                // Flip V: UVs are authored bottom-origin (glTF/OBJ), but the GE
                // samples top-origin — same 1-v the NDS exporter applies. Without
                // this, V is upside-down (terrain hits the atlas's black bottom).
                vv = 1.0f - m.uvs[v*2+1];
            }
            unsigned int col;
            if (m.hasVertexColor && (int)m.vertexColors.size() >= (v + 1) * 3) {
                unsigned r = m.vertexColors[v*3+0], g = m.vertexColors[v*3+1], b = m.vertexColors[v*3+2];
                col = 0xFF000000u | (b << 16) | (g << 8) | r;
            } else if (textured) {
                col = 0xFFFFFFFFu;   // modulate texture unchanged
            } else {
                col = Rgb15ToAbgr(m.colorRGB15, true);
            }
            f << "  {" << Flt(u) << "," << Flt(vv) << ",0x" << std::hex << col << std::dec << "u,"
              << Flt(WL(m.positions[v*3+0])) << "," << Flt(WL(m.positions[v*3+1])) << "," << Flt(WL(m.positions[v*3+2])) << "},\n";
        }
        if (vc == 0) f << "  {0,0,0xFFFFFFFFu,0,0,0},\n";
        f << "};\n";

        // Indices: triangles + triangulated quads.
        std::vector<unsigned int> idx = m.indices;
        for (size_t q = 0; q + 4 <= m.quadIndices.size(); q += 4) {
            unsigned a = m.quadIndices[q], b = m.quadIndices[q+1], c = m.quadIndices[q+2], d = m.quadIndices[q+3];
            idx.push_back(a); idx.push_back(b); idx.push_back(c);
            idx.push_back(a); idx.push_back(c); idx.push_back(d);
        }
        f << "static const unsigned short afn_mesh" << mi << "_idx[" << (idx.empty() ? 1 : idx.size()) << "] = {";
        for (size_t k = 0; k < idx.size(); k++) { if (k % 16 == 0) f << "\n  "; f << idx[k] << ","; }
        if (idx.empty()) f << "0,";
        f << "\n};\n";

        // Texture (RGBA8888). 16-byte aligned: the GE masks the low bits of the
        // texture base address, so an unaligned texture samples wrong/empty
        // memory (black) on real hardware (PPSSPP is more forgiving).
        if (textured) {
            int n = m.texW * m.texH;
            f << "static const unsigned int __attribute__((aligned(16))) afn_mesh" << mi << "_tex[" << n << "] = {";
            for (int p = 0; p < n; p++) {
                if (p % 8 == 0) f << "\n  ";
                unsigned char pi = (p < (int)m.texPixels.size()) ? m.texPixels[p] : 0;
                bool opaque = opaqueTex || pi != 0;        // index 0 transparent when alpha on
                unsigned short pal = m.texPalette[pi];
                f << "0x" << std::hex << Rgb15ToAbgr(pal, opaque) << std::dec << "u,";
            }
            f << "\n};\n";
        }
        f << "\n";
    }

    // ---- mesh table ----
    f << "const int afn_mesh_count = " << meshes.size() << ";\n";
    f << "const AfnMesh afn_meshes[" << (meshes.empty() ? 1 : meshes.size()) << "] = {\n";
    for (size_t mi = 0; mi < meshes.size(); mi++) {
        const auto& m = meshes[mi];
        int vc = (int)m.positions.size() / 3;
        int ic = (int)m.indices.size() + (int)(m.quadIndices.size() / 4) * 6;
        bool textured = m.textured && m.texW > 0 && !m.texPixels.empty();
        f << "  { " << vc << ", " << ic << ", afn_mesh" << mi << "_verts, afn_mesh" << mi << "_idx, "
          << (textured ? 1 : 0) << ", " << (textured ? m.texW : 0) << ", " << (textured ? m.texH : 0) << ", "
          << (textured ? std::string("afn_mesh") + std::to_string(mi) + "_tex" : std::string("0")) << ", "
          << (m.textureHasAlpha ? 1 : 0) << ", " << m.cullMode << ", " << m.lit << ", " << m.visible << " },\n";
    }
    if (meshes.empty()) f << "  { 0,0,0,0,0,0,0,0,0,2,0,0 },\n";
    f << "};\n\n";

    // ---- mesh instances (sprites that carry a mesh) ----
    std::vector<size_t> inst;
    for (size_t i = 0; i < sprites.size(); i++) if (sprites[i].meshIdx >= 0) inst.push_back(i);
    f << "const int afn_sprite_count = " << inst.size() << ";\n";
    f << "const AfnSpriteInst afn_sprites[" << (inst.empty() ? 1 : inst.size()) << "] = {\n";
    for (size_t k = 0; k < inst.size(); k++) {
        const auto& s = sprites[inst[k]];
        f << "  { " << Flt(WX(s.x)) << ", " << Flt(WY(s.y)) << ", " << Flt(WX(s.z)) << ", "
          << Flt(s.scale) << ", " << Flt(s.rotation) << ", " << Flt(s.rotationX) << ", " << Flt(s.rotationZ) << ", "
          << s.meshIdx << " },\n";
    }
    if (inst.empty()) f << "  { 0,0,0,1,0,0,0,-1 },\n";
    f << "};\n";
    // Editor sprite index per mesh instance (parallel to afn_sprites) — lets the
    // node graph's SetVisible/collision target a mesh instance by sprite index.
    f << "#define AFN_HAS_SPRITE_IDX 1\n";
    f << "static const int afn_mesh_inst_sprite[" << (inst.empty()?1:inst.size()) << "] = {";
    for (size_t k = 0; k < inst.size(); k++) f << (int)inst[k] << ",";
    if (inst.empty()) f << "0,";
    f << "};\n";
    // Total editor sprite count — sizes the script-glue per-sprite arrays
    // (afn_sprite_visible[], afn_hp[], ...). Guarded so a runtime can override.
    f << "#ifndef NUM_SPRITES\n#define NUM_SPRITES " << (sprites.empty()?1:(int)sprites.size()) << "\n#endif\n\n";
    // Per-editor-sprite "start hidden" flag (the attached-sprite "Hidden (effect)"
    // toggle). The runtime seeds afn_sprite_visible from this at scene start so a
    // hidden effect sprite (e.g. the Focus Blast orb) stays invisible until a node
    // shows it — instead of every sprite being forced visible.
    f << "#define AFN_HAS_SPRITE_START_HIDDEN 1\n";
    f << "static const unsigned char afn_sprite_start_hidden[NUM_SPRITES] = {";
    for (size_t i = 0; i < sprites.size(); i++) f << (sprites[i].startHidden ? 1 : 0) << ",";
    if (sprites.empty()) f << "0,";
    f << "};\n\n";

    // ---- camera / movement ----
    f << "const float afn_cam_start_x = " << Flt(WX(camera.x)) << ";\n";
    f << "const float afn_cam_start_z = " << Flt(WX(camera.z)) << ";\n";
    f << "const float afn_cam_start_h = " << Flt(WY(camera.height)) << ";\n";
    f << "const float afn_cam_start_angle = " << Flt(camera.angle) << ";\n";
    // Initial orbit pitch (degrees, 0 = auto: derive from height/dist). PSV seeds
    // orbit_pitch from this so the scene starts at a fixed vertical angle.
    f << "const float afn_cam_start_pitch = " << Flt(camera.orbitPitch) << ";\n";
    f << "const float afn_orbit_dist = " << Flt(orbitDist / 4.0f) << ";\n";
    f << "const float afn_draw_distance = " << Flt(camera.drawDistance > 0 ? camera.drawDistance / 4.0f : 0.0f) << ";\n";
    f << "const float afn_walk_speed = " << Flt(camera.walkSpeed) << ";\n";
    f << "const float afn_sprint_speed = " << Flt(camera.sprintSpeed) << ";\n";
    // Camera-delay ease rates (x/256 catch-up per frame; editor sliders are %).
    // Same emit as nds_package.cpp — the runtime camera chase / Y-follow reads
    // these so walk/sprint/orbit/jump camera lag matches the NDS feel.
    f << "#define AFN_WALK_EASE_IN "    << (int)(camera.walkEaseIn * 256.0f / 100.0f) << "\n";
    f << "#define AFN_WALK_EASE_OUT "   << (int)(camera.walkEaseOut * 256.0f / 100.0f) << "\n";
    f << "#define AFN_SPRINT_EASE_IN "  << (int)(camera.sprintEaseIn * 256.0f / 100.0f) << "\n";
    f << "#define AFN_SPRINT_EASE_OUT " << (int)(camera.sprintEaseOut * 256.0f / 100.0f) << "\n";
    f << "#define AFN_ORBIT_EASE_IN "   << (int)(camera.orbitCamEaseIn  * 256.0f / 100.0f) << "\n";
    f << "#define AFN_ORBIT_EASE_OUT "  << (int)(camera.orbitCamEaseOut * 256.0f / 100.0f) << "\n";
    if (camera.orbitMaxDelta > 0) f << "#define AFN_ORBIT_MAX_DELTA " << camera.orbitMaxDelta << "\n";
    f << "#define AFN_JUMP_CAM_LAND "   << (int)(camera.jumpCamLand * 256.0f / 100.0f) << "\n";
    f << "#define AFN_JUMP_CAM_AIR "    << (int)(camera.jumpCamAir  * 256.0f / 100.0f) << "\n";

    // ---- camera presets / slots (Mode 4) ----
    // Slot 0 = scene default; slots 1..N are SetCamera targets. Columns:
    //   { orbit yaw (radians), orbit dist (world px), camera height (world px),
    //     horizon (editor px), orbit pitch (deg, 0 = auto) }. The runtime orbit-
    //     follows the player and blends the live camera toward afn_active_camera.
    //     PSV honors column 4 (pitch) per slot; NDS still uses column 3 (horizon).
    f << "#define AFN_CAM_SLOT_COUNT " << (1 + (int)camera.camSlots.size()) << "\n";
    f << "static const float afn_cam_slots[][5] = {\n";
    f << "    { " << Flt(camera.angle) << ", " << Flt(orbitDist / 4.0f) << ", "
                  << Flt(WY(camera.height)) << ", " << Flt(camera.horizon) << ", "
                  << Flt(camera.orbitPitch) << " },\n";
    for (const auto& cs : camera.camSlots) {
        float ang = cs.angle * 3.14159265f / 180.0f;                 // editor deg -> radians
        float di  = (cs.distance > 0.0f) ? cs.distance / 4.0f : orbitDist / 4.0f;
        float he  = WY(cs.height);
        f << "    { " << Flt(ang) << ", " << Flt(di) << ", " << Flt(he) << ", "
                      << Flt(cs.horizon) << ", " << Flt(cs.orbitPitch) << " },\n";
    }
    f << "};\n";
    // Active preset. PSV has no scripts yet, so this stays 0 (scene default); the
    // SetCamera node will drive it once script_glue is ported.
    f << "static int afn_active_camera = 0;\n";

    f.close();
    return true;
}

// ---- rig data (psp_rig.h) -------------------------------------------------
// Emitted as its own header (compiled by rig.c) so the bulky animation data
// stays out of mapdata.c. Geometry is raw model-space; the runtime CPU-skins
// (skinned = animPose[bone]·baseVert) then scales/translates to the player.
static bool GeneratePSPRigData(const std::string& runtimeDir, const char* hdrPrefix,
                               const std::vector<PSPRigExport>& pspRigs,
                               int playerRigIdx,
                               const std::vector<GBASpriteExport>& sprites,
                               std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "rig.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PSP rig data — GENERATED by Export PSP. Do not edit.\n#pragma once\n\n";

    bool ok = playerRigIdx >= 0 && playerRigIdx < (int)pspRigs.size()
              && !pspRigs[playerRigIdx].verts.empty() && !pspRigs[playerRigIdx].clips.empty()
              && pspRigs[playerRigIdx].boneCount > 0;
    if (!ok) { f << "// (no player rig in this scene)\n"; return true; }

    const PSPRigExport& rig = pspRigs[playerRigIdx];
    int bc = rig.boneCount;
    int vc = (int)rig.verts.size();
    int mc = (int)rig.materials.size(); if (mc < 1) mc = 1;
    int cc = (int)rig.clips.size();
    int tc = (int)rig.indices.size() / 3;

    // Player placement: find the Player sprite that uses this rig. Track its
    // editor index too (= its position in `sprites`, matching afn_spr_editor_idx)
    // so the runtime can re-anchor player-PARENTED attached sprites to the live
    // player position (the generic re-anchor only follows NPCs).
    float psx = 0, psy = 0, psz = 0, pscale = 1.0f; int pclip = 0, pspr = -1;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        if (s.spriteType == 1 && s.riggedMeshIdx == playerRigIdx) {
            psx = WX(s.x); psy = WY(s.y); psz = WX(s.z); pscale = s.scale; pclip = s.rigAnimIdx; pspr = (int)i; break;
        }
    }

    f << "#define AFN_HAS_PLAYER_RIG 1\n";
    f << "#define AFN_RIG_BONES "  << bc << "\n";
    f << "#define AFN_RIG_VERTS "  << vc << "\n";
    f << "#define AFN_RIG_MATS "   << mc << "\n";
    f << "#define AFN_RIG_CLIPS "  << cc << "\n";
    f << "#define AFN_RIG_CULL "   << rig.cullMode << "\n";
    f << "#define AFN_RIG_USE_ALPHA " << (rig.useAlpha ? 1 : 0) << "\n";
    f << "#define AFN_PLAYER_START_X " << Flt(psx) << "\n";
    f << "#define AFN_PLAYER_START_Y " << Flt(psy) << "\n";
    f << "#define AFN_PLAYER_START_Z " << Flt(psz) << "\n";
    if (pspr >= 0) f << "#define AFN_PLAYER_SPRITE_IDX " << pspr << "\n";
    // Model-space -> world-px. Meshes use /4 * spriteScale; rigs are a separate
    // asset so this may need tuning, but /4*scale is the consistent default.
    f << "#define AFN_PLAYER_RIG_SCALE " << Flt(pscale * 0.25f) << "\n";
    f << "#define AFN_PLAYER_DEFAULT_CLIP " << pclip << "\n";
    // Camera headlamp (per-material toggle): bake the lightX/lightY aim into an
    // eye-space light direction, same math as the NDS exporter (nds_package.cpp).
    if (rig.cameraLight) {
        f << "#define AFN_PLAYER_RIG_CAMLIGHT 1\n";
        float ax = rig.lightX * 3.14159265f/180.0f, ay = rig.lightY * 3.14159265f/180.0f;
        float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
        char lbuf[96];
        snprintf(lbuf, sizeof(lbuf), "#define AFN_PLAYER_RIG_LIGHT_DX (%.6ff)\n", -cx*sy); f << lbuf;
        snprintf(lbuf, sizeof(lbuf), "#define AFN_PLAYER_RIG_LIGHT_DY (%.6ff)\n",  sx);    f << lbuf;
        snprintf(lbuf, sizeof(lbuf), "#define AFN_PLAYER_RIG_LIGHT_DZ (%.6ff)\n", -cx*cy); f << lbuf;
    }
    f << "\n";

    // Base verts (raw model space): position, normal, uv (V flipped), bone index.
    f << "static const float afn_rig_vpos[" << vc*3 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << Flt(V.px) << "," << Flt(V.py) << "," << Flt(V.pz) << ","; if (v%4==3) f << "\n"; }
    f << "\n};\n";
    f << "static const float afn_rig_vnorm[" << vc*3 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << Flt(V.nx) << "," << Flt(V.ny) << "," << Flt(V.nz) << ","; if (v%4==3) f << "\n"; }
    f << "\n};\n";
    // NOTE: rig UVs are NOT V-flipped (unlike the OBJ meshes). The glTF image
    // decode in the editor already lands the texture the right way up for the
    // rig's raw UVs; flipping here sent them into the atlas's blank region.
    f << "static const float afn_rig_vuv[" << vc*2 << "] = {\n";
    for (int v = 0; v < vc; v++) { const auto& V = rig.verts[v];
        f << Flt(V.u) << "," << Flt(V.v) << ","; if (v%6==5) f << "\n"; }
    f << "\n};\n";
    f << "static const unsigned char afn_rig_vbone[" << vc << "] = {\n";
    for (int v = 0; v < vc; v++) { f << rig.verts[v].bone << ","; if (v%24==23) f << "\n"; }
    f << "\n};\n\n";

    // Per-material index groups (split rig.indices by triMaterial).
    for (int g = 0; g < mc; g++) {
        std::vector<unsigned short> gi;
        for (int t = 0; t < tc; t++) {
            int slot = (t < (int)rig.triMaterial.size()) ? rig.triMaterial[t] : 0;
            if (slot == g) { gi.push_back((unsigned short)rig.indices[t*3+0]);
                             gi.push_back((unsigned short)rig.indices[t*3+1]);
                             gi.push_back((unsigned short)rig.indices[t*3+2]); }
        }
        f << "static const unsigned short afn_rig_idx" << g << "[" << (gi.empty()?1:gi.size()) << "] = {";
        for (size_t k = 0; k < gi.size(); k++) { if (k%16==0) f << "\n  "; f << gi[k] << ","; }
        if (gi.empty()) f << "0,";
        f << "\n};\n";
        f << "#define AFN_RIG_IDX" << g << "_COUNT " << gi.size() << "\n";

        // Material texture (RGBA8 palette -> ABGR8888).
        const PSPRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : PSPRigMaterial{};
        bool tex = M.textured && M.texW > 0 && !M.pixels.empty();
        if (tex) {
            int n = M.texW * M.texH;
            f << "static const unsigned int __attribute__((aligned(16))) afn_rig_tex" << g << "[" << n << "] = {";
            for (int p = 0; p < n; p++) {
                if (p%8==0) f << "\n  ";
                unsigned char idx = (p < (int)M.pixels.size()) ? M.pixels[p] : 0;
                unsigned int c = M.palette[idx];
                if (rig.useAlpha && idx == 0) c &= 0x00FFFFFFu;   // index 0 transparent
                f << "0x" << std::hex << c << std::dec << "u,";
            }
            f << "\n};\n";
        }
        f << "#define AFN_RIG_TEX" << g << "_W " << (tex ? M.texW : 0) << "\n";
        f << "#define AFN_RIG_TEX" << g << "_H " << (tex ? M.texH : 0) << "\n\n";
    }
    // Pointer tables so the runtime can loop materials.
    f << "static const unsigned short* const afn_rig_idx_ptrs[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << "afn_rig_idx" << g << ",";
    f << "};\n";
    f << "static const int afn_rig_idx_counts[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << "AFN_RIG_IDX" << g << "_COUNT,";
    f << "};\n";
    f << "static const unsigned int* const afn_rig_tex_ptrs[" << mc << "] = {";
    for (int g = 0; g < mc; g++) {
        const PSPRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : PSPRigMaterial{};
        bool tex = M.textured && M.texW > 0 && !M.pixels.empty();
        f << (tex ? (std::string("afn_rig_tex")+std::to_string(g)) : std::string("0")) << ",";
    }
    f << "};\n";
    f << "static const int afn_rig_tex_w[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << "AFN_RIG_TEX" << g << "_W,";
    f << "};\n";
    f << "static const int afn_rig_tex_h[" << mc << "] = {";
    for (int g = 0; g < mc; g++) f << "AFN_RIG_TEX" << g << "_H,";
    f << "};\n\n";

    // Clips: per clip frameCount*boneCount poses {px,py,pz,qw,qx,qy,qz}.
    for (int c = 0; c < cc; c++) {
        const PSPRigClip& cl = rig.clips[c];
        int nf = cl.frameCount;
        f << "static const float afn_rig_clip" << c << "[" << (nf*bc*7) << "] = {\n";
        for (int fr = 0; fr < nf; fr++) {
            for (int b = 0; b < bc; b++) {
                const PSPRigBonePose& P = cl.frames[fr*bc + b];
                f << Flt(P.px) << "," << Flt(P.py) << "," << Flt(P.pz) << ","
                  << Flt(P.qw) << "," << Flt(P.qx) << "," << Flt(P.qy) << "," << Flt(P.qz) << ",\n";
            }
        }
        f << "};\n";
    }
    f << "static const float* const afn_rig_clip_ptrs[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << "afn_rig_clip" << c << ",";
    f << "};\n";
    f << "static const int afn_rig_clip_frames[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << rig.clips[c].frameCount << ",";
    f << "};\n";
    f << "static const unsigned char afn_rig_clip_loop[" << cc << "] = {";
    for (int c = 0; c < cc; c++) f << (rig.clips[c].loop ? 1 : 0) << ",";
    f << "};\n";

    // ---- NPC instances ----
    // Rigged sprites that aren't the player and reuse THIS rig asset. Columns:
    // { x, y, z (world px), rotY (deg), scale, default clip }. The runtime draws
    // each through the player rig pipeline at its own transform. NPCs that use a
    // DIFFERENT rig asset aren't supported yet (the export carries one rig).
    {
        std::ostringstream rows;
        int n = 0;
        for (const auto& s : sprites) {
            if (s.spriteType == 1) continue;                 // the player
            if (s.riggedMeshIdx != playerRigIdx) continue;   // no rig / different rig
            int clip = (s.rigAnimIdx >= 0 && s.rigAnimIdx < cc) ? s.rigAnimIdx : 0;
            rows << "    { " << Flt(WX(s.x)) << ", " << Flt(WY(s.y)) << ", " << Flt(WX(s.z))
                 << ", " << Flt(s.rotation) << ", " << Flt(s.scale) << ", " << clip << " },\n";
            n++;
        }
        f << "#define AFN_NPC_COUNT " << n << "\n";
        f << "static const float afn_npc_inst[" << (n > 0 ? n : 1) << "][6] = {\n";
        f << (n > 0 ? rows.str() : std::string("    {0,0,0,0,0,0},\n"));
        f << "};\n";
    }

    return true;
}

// ---- sky panorama (psp_sky.h) ---------------------------------------------
static bool GeneratePSPSky(const std::string& runtimeDir, const char* hdrPrefix,
                           const std::vector<GBASkyFrameExport>& skyFrames,
                           std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "sky.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PSP sky — GENERATED by Export PSP. Do not edit.\n#pragma once\n\n";
    if (skyFrames.empty() || !skyFrames[0].pixels || skyFrames[0].w <= 0 || skyFrames[0].h <= 0) {
        f << "// (no sky in this scene)\n";
        return true;
    }
    const auto& sk = skyFrames[0];
    int w = sk.w, h = sk.h, n = w * h;
    f << "#define AFN_HAS_SKY 1\n#define AFN_SKY_W " << w << "\n#define AFN_SKY_H " << h << "\n";
    f << "static const unsigned int __attribute__((aligned(16))) afn_sky_tex[" << n << "] = {";
    const unsigned char* p = sk.pixels;
    for (int i = 0; i < n; i++) {
        if (i % 8 == 0) f << "\n  ";
        unsigned r = p[i*4+0], g = p[i*4+1], b = p[i*4+2], a = p[i*4+3];
        f << "0x" << std::hex << ((a<<24)|(b<<16)|(g<<8)|r) << std::dec << "u,";
    }
    f << "\n};\n";
    return true;
}

// ---- sprite billboards (psp_sprites.h) ------------------------------------
// Animated camera-facing quads for non-mesh, non-rig sprites (pickups/NPCs).
// Frame pixels are pixels[py*64+px] = palette index (0 = transparent); palette
// is RGBA8 (same byte order the rig palettes use). Directional/8-facing sprites
// are simplified to their default anim here (facing-pick is a follow-up).
static bool GeneratePSPSprites(const std::string& runtimeDir, const char* hdrPrefix,
                               const std::vector<GBASpriteExport>& sprites,
                               const std::vector<GBASpriteAssetExport>& assets,
                               std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "sprites.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PSP sprite billboards — GENERATED by Export PSP. Do not edit.\n#pragma once\n\n";

    struct Inst { float x, y, z, scale; int asset, anim, sprite;
                  int parent; float offX, offY, offZ; int bone; };
    std::vector<Inst> inst;
    for (size_t i = 0; i < sprites.size(); i++) {
        const auto& s = sprites[i];
        if (s.assetIdx >= 0 && s.assetIdx < (int)assets.size() &&
            s.meshIdx < 0 && s.riggedMeshIdx < 0 && !assets[s.assetIdx].frames.empty())
            inst.push_back({ WX(s.x), WY(s.y), WX(s.z), s.scale, s.assetIdx, s.animIdx, (int)i,
                             s.parentIdx, WL(s.offsetX), WL(s.offsetY), WL(s.offsetZ), s.boneIdx });
    }

    // World anchors: every attached (sub-)sprite — WITH OR WITHOUT an asset —
    // exports {parent editor index, local offset}. An asset-less attached
    // sprite is a pure invisible anchor: the PSV runtime projects parent live
    // position + offset for HUD anchoring even though no billboard draws.
    {
        std::ostringstream rows; int n = 0;
        for (const auto& s : sprites)
            if (s.parentIdx >= 0) {
                rows << "  { " << s.parentIdx << ", " << Flt(WL(s.offsetX)) << ", "
                     << Flt(WL(s.offsetY)) << ", " << Flt(WL(s.offsetZ)) << " },\n";
                n++;
            }
        if (n > 0) {
            f << "#define AFN_HAS_WORLD_ANCHORS 1\n";
            f << "#define AFN_ANCHOR_COUNT " << n << "\n";
            f << "// {parent sprite idx, offX, offY, offZ (world px)}\n";
            f << "static const float afn_anchors[" << n << "][4] = {\n" << rows.str() << "};\n";
        }
    }

    if (inst.empty()) { f << "// (no billboard sprites in this scene)\n"; return true; }

    // Global frame texture table; dedup by asset (each used asset emits its frames once).
    std::vector<int> assetFrame0(assets.size(), -1);
    int globalFrames = 0;
    auto emitFrameTex = [&](int ai, int fi) {
        const auto& a = assets[ai];
        const auto& fr = a.frames[fi];
        int w = fr.width > 0 ? fr.width : a.baseSize;
        int h = fr.height > 0 ? fr.height : a.baseSize;
        int gi = globalFrames++;
        f << "static const unsigned int __attribute__((aligned(16))) afn_spr_f" << gi << "[" << (w*h) << "] = {";
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                if ((py*w+px) % 8 == 0) f << "\n  ";
                unsigned char idx = fr.pixels[py * kExportMaxFrameSize + px];
                unsigned c = (idx == 0) ? 0u : a.palette[idx & 15];
                f << "0x" << std::hex << c << std::dec << "u,";
            }
        f << "\n};\n";
        return std::pair<int,int>{w, h};
    };

    // Directional sprites (8-facing: N,NE,E,SE,S,SW,W,NW) keep their real art in
    // dirAnimSets[set].dirImages[d] as RGBA8 — NOT in the palettized a.frames
    // (which is blank for them). Emit each direction as an RGBA8 frame; an empty
    // direction falls back to the first filled one so a partially-authored sprite
    // (e.g. only N drawn) still shows from every angle. The runtime picks
    // base + dirIdx by camera facing instead of animating.
    auto emitDirFrame = [&](const GBASpriteAssetExport::DirAnimSetExport& set, int d) -> std::pair<int,int> {
        int use = set.dirImages[d].pixels ? d : -1;
        if (use < 0) for (int k = 0; k < 8; k++) if (set.dirImages[k].pixels) { use = k; break; }
        int gi = globalFrames++;
        if (use < 0) { f << "static const unsigned int afn_spr_f" << gi << "[1] = {0};\n"; return {1,1}; }
        const auto& img = set.dirImages[use];
        int w = img.width > 0 ? img.width : 1, h = img.height > 0 ? img.height : 1;
        f << "static const unsigned int __attribute__((aligned(16))) afn_spr_f" << gi << "[" << (w*h) << "] = {";
        for (int py = 0; py < h; py++)
            for (int px = 0; px < w; px++) {
                if ((py*w+px) % 8 == 0) f << "\n  ";
                const unsigned char* p = &img.pixels[(py*w+px)*4];   // RGBA8 bytes
                unsigned c = (p[3] < 128) ? 0u
                    : (((unsigned)p[3]<<24)|((unsigned)p[2]<<16)|((unsigned)p[1]<<8)|(unsigned)p[0]);
                f << "0x" << std::hex << c << std::dec << "u,";
            }
        f << "\n};\n";
        return {w, h};
    };

    std::vector<int> assetIsDir(assets.size(), 0);   // 1 = emitted as 8-dir blocks
    std::vector<int> assetSets(assets.size(), 0);    // # of dir-sets (for base calc)
    std::vector<std::pair<int,int>> frameWH;
    for (auto& in : inst) {
        if (assetFrame0[in.asset] >= 0) continue;
        assetFrame0[in.asset] = globalFrames;
        const auto& a = assets[in.asset];
        if (a.hasDirections && !a.dirAnimSets.empty()) {
            assetIsDir[in.asset] = 1;
            assetSets[in.asset]  = (int)a.dirAnimSets.size();
            for (const auto& set : a.dirAnimSets)
                for (int d = 0; d < 8; d++)
                    frameWH.push_back(emitDirFrame(set, d));
        } else {
            for (int fi = 0; fi < (int)a.frames.size(); fi++)
                frameWH.push_back(emitFrameTex(in.asset, fi));
        }
    }

    // Frame pointer + size tables.
    f << "static const unsigned int* const afn_spr_frame_ptrs[" << globalFrames << "] = {";
    for (int i = 0; i < globalFrames; i++) f << "afn_spr_f" << i << ",";
    f << "};\n";
    f << "static const short afn_spr_frame_w[" << globalFrames << "] = {";
    for (auto& wh : frameWH) f << wh.first << ",";
    f << "};\n";
    f << "static const short afn_spr_frame_h[" << globalFrames << "] = {";
    for (auto& wh : frameWH) f << wh.second << ",";
    f << "};\n\n";

    // Per-instance data. Resolve anim -> global frame range + fps.
    f << "#define AFN_SPR_INST_COUNT " << inst.size() << "\n";
    auto col = [&](const char* nm, auto fn) {
        f << "static const float afn_spr_" << nm << "[" << inst.size() << "] = {";
        for (auto& in : inst) f << Flt(fn(in)) << ","; f << "};\n";
    };
    col("x",     [](const Inst& i){ return i.x; });
    col("y",     [](const Inst& i){ return i.y; });
    col("z",     [](const Inst& i){ return i.z; });
    col("scale", [](const Inst& i){ return i.scale; });
    // Editor sprite index per billboard — lets SetVisible target it.
    f << "static const int afn_spr_editor_idx[" << inst.size() << "] = {";
    for (auto& in : inst) f << in.sprite << ","; f << "};\n";
    // Attached (sub-)sprites: parent's editor sprite index (-1 = standalone)
    // + local offset in world px. The PSV runtime re-anchors these to the
    // parent's LIVE position each frame (nav/follow-moved NPCs), so a marker
    // attached to an NPC tracks it instead of sitting at the authored spot.
    f << "#define AFN_HAS_SPR_PARENT 1\n";
    f << "static const int afn_spr_parent[" << inst.size() << "] = {";
    for (auto& in : inst) f << in.parent << ","; f << "};\n";
    // Bone attach: -1 = anchor to parent origin, >=0 = ride that bone of the
    // parent rig. The runtime samples the live bone matrix (player rig only).
    {
        bool anyBone = false;
        for (auto& in : inst) if (in.bone >= 0) { anyBone = true; break; }
        if (anyBone) {
            f << "#define AFN_HAS_SPR_BONE 1\n";
            f << "static const int afn_spr_bone[" << inst.size() << "] = {";
            for (auto& in : inst) f << in.bone << ","; f << "};\n";
        }
    }
    col("poff_x", [](const Inst& i){ return i.offX; });
    col("poff_y", [](const Inst& i){ return i.offY; });
    col("poff_z", [](const Inst& i){ return i.offZ; });
    auto icol = [&](const char* nm, auto fn) {
        f << "static const int afn_spr_" << nm << "[" << inst.size() << "] = {";
        for (auto& in : inst) f << fn(in) << ","; f << "};\n";
    };
    // Anim ranges are clamped to the asset's ACTUAL emitted frame count: an
    // authored anim can name an endFrame past the last real frame (e.g. a
    // 2-frame anim on a 1-frame asset), and the runtime indexes the global frame
    // texture table directly — an out-of-range index reads a garbage texture
    // (the "flickering texture map" bug). Clamp to [0, frames-1].
    icol("fstart", [&](const Inst& in){
        const auto& a = assets[in.asset];
        if (assetIsDir[in.asset]) return assetFrame0[in.asset];   // unused for directional
        int nf = (int)a.frames.size(); if (nf < 1) nf = 1;
        int s = 0;
        if (in.anim >= 0 && in.anim < (int)a.anims.size()) s = a.anims[in.anim].startFrame;
        else if (a.defaultAnim >= 0 && a.defaultAnim < (int)a.anims.size()) s = a.anims[a.defaultAnim].startFrame;
        if (s < 0) s = 0; if (s > nf - 1) s = nf - 1;
        return assetFrame0[in.asset] + s;
    });
    icol("fend", [&](const Inst& in){
        const auto& a = assets[in.asset];
        if (assetIsDir[in.asset]) return assetFrame0[in.asset];   // unused for directional
        int nf = (int)a.frames.size(); if (nf < 1) nf = 1;
        int e = nf - 1;
        if (in.anim >= 0 && in.anim < (int)a.anims.size()) e = a.anims[in.anim].endFrame;
        else if (a.defaultAnim >= 0 && a.defaultAnim < (int)a.anims.size()) e = a.anims[a.defaultAnim].endFrame;
        if (e < 0) e = 0; if (e > nf - 1) e = nf - 1;
        return assetFrame0[in.asset] + e;
    });
    // Directional flag + base frame: for an 8-facing sprite the runtime picks
    // afn_spr_dir_base + dirIdx (by camera angle) instead of fps animation.
    icol("directional", [&](const Inst& in){ return assetIsDir[in.asset]; });
    icol("dir_base", [&](const Inst& in){
        const auto& a = assets[in.asset];
        if (!assetIsDir[in.asset]) return assetFrame0[in.asset];
        int set = 0;
        if (in.anim >= 0 && in.anim < (int)a.anims.size()) set = a.anims[in.anim].startFrame;
        else if (a.defaultAnim >= 0 && a.defaultAnim < (int)a.anims.size()) set = a.anims[a.defaultAnim].startFrame;
        int ns = assetSets[in.asset]; if (ns < 1) ns = 1;
        if (set < 0) set = 0; if (set > ns - 1) set = ns - 1;
        return assetFrame0[in.asset] + set * 8;
    });
    icol("fps", [&](const Inst& in){
        const auto& a = assets[in.asset];
        int fps = 8;
        if (in.anim >= 0 && in.anim < (int)a.anims.size()) fps = a.anims[in.anim].fps;
        else if (a.defaultAnim >= 0 && a.defaultAnim < (int)a.anims.size()) fps = a.anims[a.defaultAnim].fps;
        return fps > 0 ? fps : 8;
    });
    icol("basesize", [&](const Inst& in){ return assets[in.asset].baseSize > 0 ? assets[in.asset].baseSize : 16; });
    f << "#define AFN_HAS_SPRITES 1\n";
    return true;
}

// ---- sound (psp_sound.h) --------------------------------------------------
// PCM samples + sequenced-note tables + per-instance metadata, consumed by the
// PSP software mixer (source/audio.c). Mirrors the NDS emit (nds_package.cpp)
// but with plain C types (NDS s8/s16/u8 -> signed char/short/unsigned char) and
// adds per-instance SFX routing (afn_snd_is_sfx/sfx_sample/sfx_gain/sfx_fifo) so
// afn_play_sound() on an SFX-type instance fires the one-shot sample directly.
static bool GeneratePSPSound(const std::string& runtimeDir, const char* hdrPrefix,
                             const std::vector<GBASoundSampleExport>& soundSamples,
                             const std::vector<GBASoundInstanceExport>& soundInstances,
                             std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "sound.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PSP sound — GENERATED by Export PSP. Do not edit.\n#pragma once\n\n";

    if (soundSamples.empty()) {
        f << "#define AFN_SOUND_SAMPLE_COUNT 0\n";
        f << "#define AFN_SOUND_INSTANCE_COUNT 0\n";
        f << "// (no audio in this project)\n";
        return true;
    }

    f << "#define AFN_SOUND_SAMPLE_COUNT " << soundSamples.size() << "\n";
    f << "#define AFN_SOUND_INSTANCE_COUNT " << soundInstances.size() << "\n\n";

    // ---- PCM sample data ----
    for (int i = 0; i < (int)soundSamples.size(); i++) {
        auto& smp = soundSamples[i];
        bool use16 = !smp.data16.empty() && (int)smp.data16.size() == (int)smp.data.size();
        // PCM emitted as a byte STRING LITERAL (octal-escaped), not a {..,..}
        // initializer list: GCC parses a multi-MB string literal far faster than
        // that many integer constant-expressions, which is what made long audio
        // crawl the build. afn_pcm_ptrs is void* + a 16-bit flag and the runtime
        // casts (short*/signed char*), so the stored type is transparent; 16-bit
        // bytes are little-endian to match the target. aligned(4) keeps short
        // access aligned.
        auto emitByte = [&](unsigned v) { v &= 0xFF; f << '\\' << ((v>>6)&7) << ((v>>3)&7) << (v&7); };
        if (use16) {
            f << "static const unsigned char afn_pcm_" << i << "[] __attribute__((aligned(4))) =\n  \"";
            int run = 0;
            for (int j = 0; j < (int)smp.data16.size(); j++) {
                int s = smp.data16[j]; emitByte(s & 0xFF); emitByte((s >> 8) & 0xFF);
                if (++run >= 64) { f << "\"\n  \""; run = 0; }
            }
            int padVal = 0;
            if (smp.loop && smp.loopStart >= 0 && smp.loopStart < (int)smp.data16.size())
                padVal = (int)smp.data16[smp.loopStart];
            emitByte(padVal & 0xFF); emitByte((padVal >> 8) & 0xFF);
            f << "\";\n";
        } else {
            f << "static const unsigned char afn_pcm_" << i << "[] __attribute__((aligned(4))) =\n  \"";
            int run = 0;
            for (int j = 0; j < (int)smp.data.size(); j++) {
                emitByte((unsigned)(unsigned char)smp.data[j]);
                if (++run >= 96) { f << "\"\n  \""; run = 0; }
            }
            int padVal = 0;
            if (smp.loop && smp.loopStart >= 0 && smp.loopStart < (int)smp.data.size())
                padVal = (int)smp.data[smp.loopStart];
            emitByte((unsigned)(unsigned char)padVal);
            f << "\";\n";
        }
        f << "#define AFN_PCM_" << i << "_LEN " << smp.data.size() << "\n";
        f << "#define AFN_PCM_" << i << "_RATE " << smp.sampleRate << "\n";
        f << "#define AFN_PCM_" << i << "_16BIT " << (use16 ? 1 : 0) << "\n\n";
    }

    auto u8tab = [&](const char* name, auto fn) {
        f << "static const unsigned char " << name << "[" << soundSamples.size() << "] = {\n    ";
        for (int i = 0; i < (int)soundSamples.size(); i++) f << fn(i) << ",";
        f << "\n};\n";
    };
    auto itab = [&](const char* name, auto fn) {
        f << "static const int " << name << "[" << soundSamples.size() << "] = {\n    ";
        for (int i = 0; i < (int)soundSamples.size(); i++) f << fn(i) << ",";
        f << "\n};\n";
    };

    f << "static const void* const afn_pcm_ptrs[" << soundSamples.size() << "] = {\n";
    for (int i = 0; i < (int)soundSamples.size(); i++) f << "    afn_pcm_" << i << ",\n";
    f << "};\n";
    u8tab("afn_pcm_is16", [&](int i){
        return (!soundSamples[i].data16.empty() &&
                (int)soundSamples[i].data16.size() == (int)soundSamples[i].data.size()) ? 1 : 0; });
    itab("afn_pcm_lens",       [&](int i){ return (int)soundSamples[i].data.size(); });
    itab("afn_pcm_rates",      [&](int i){ return soundSamples[i].sampleRate; });
    u8tab("afn_pcm_loop",      [&](int i){ return soundSamples[i].loop ? 1 : 0; });
    itab("afn_pcm_loop_start", [&](int i){ return soundSamples[i].loopStart; });
    itab("afn_pcm_loop_end",   [&](int i){
        return soundSamples[i].loopEnd > 0 ? soundSamples[i].loopEnd : (int)soundSamples[i].data.size(); });
    u8tab("afn_pcm_vol_scale", [&](int i){ return soundSamples[i].volScale > 255 ? 255 : soundSamples[i].volScale; });
    itab("afn_pcm_release_ms",   [&](int i){ return soundSamples[i].releaseMs; });
    itab("afn_pcm_decay_pct",    [&](int i){ return soundSamples[i].decayPct; });
    itab("afn_pcm_decay_min_ms", [&](int i){ return soundSamples[i].decayMinMs; });

    // fineTune already baked into AFN_PCM_*_RATE; emit unity factors so audio.c's
    // multiplier path is a no-op (and AFN_HAS_FINE_FACTOR keeps it linking).
    f << "#define AFN_HAS_FINE_FACTOR 1\n";
    f << "#define AFN_MIDI_MASTER_VOL_FIX 256\n";
    f << "static const unsigned int afn_pcm_fine_factor[" << soundSamples.size() << "] = {\n    ";
    for (int i = 0; i < (int)soundSamples.size(); i++) f << "65536,";
    f << "\n};\n\n";

    // ---- Sequenced notes ----
    f << "typedef struct { int tick; unsigned char note; unsigned char vel; unsigned char smpIdx; unsigned char channel; int dur; } AfnSndNote;\n\n";
    for (int i = 0; i < (int)soundInstances.size(); i++) {
        auto& inst = soundInstances[i];
        if (inst.notes.empty()) continue;
        f << "// Instance " << i << ": " << inst.name << "\n";
        f << "static const AfnSndNote afn_snd_notes_" << i << "[] = {\n";
        for (auto& n : inst.notes)
            f << "    {" << n.tick << "," << n.note << "," << n.velocity << "," << n.sampleIdx << "," << n.channel << "," << n.duration << "},\n";
        f << "};\n";
    }

    int ninst = (int)soundInstances.size();
    f << "static const AfnSndNote* const afn_snd_note_ptrs[" << ninst << "] = {\n";
    for (int i = 0; i < ninst; i++)
        f << (soundInstances[i].notes.empty() ? "    0,\n" : ("    afn_snd_notes_" + std::to_string(i) + ",\n"));
    f << "};\n";

    auto iinst = [&](const char* name, auto fn) {
        f << "static const int " << name << "[" << ninst << "] = {\n    ";
        for (int i = 0; i < ninst; i++) f << fn(i) << ",";
        f << "\n};\n";
    };
    auto u8inst = [&](const char* name, auto fn) {
        f << "static const unsigned char " << name << "[" << ninst << "] = {\n    ";
        for (int i = 0; i < ninst; i++) f << fn(i) << ",";
        f << "\n};\n";
    };

    iinst("afn_snd_note_counts", [&](int i){ return (int)soundInstances[i].notes.size(); });
    iinst("afn_snd_tpf", [&](int i){
        int tpf = soundInstances[i].tempo * soundInstances[i].ticksPerBeat * 256 / 3600;
        return tpf < 1 ? 1 : tpf; });
    u8inst("afn_snd_voices",    [&](int i){ return soundInstances[i].voiceCount; });
    u8inst("afn_snd_soft_fade", [&](int i){ return soundInstances[i].softFadeA ? 1 : 0; });
    u8inst("afn_snd_loop",      [&](int i){ return soundInstances[i].loop ? 1 : 0; });
    iinst("afn_snd_loop_start", [&](int i){ return soundInstances[i].loopStartTick; });
    iinst("afn_snd_loop_end",   [&](int i){
        int le = soundInstances[i].loopEndTick;
        if (le <= 0 && !soundInstances[i].notes.empty())
            le = soundInstances[i].notes.back().tick + soundInstances[i].notes.back().duration;
        return le; });

    // ---- SFX routing: afn_play_sound() on an SFX instance fires the one-shot ----
    u8inst("afn_snd_is_sfx",    [&](int i){ return soundInstances[i].isSfx ? 1 : 0; });
    iinst("afn_snd_sfx_sample", [&](int i){ return soundInstances[i].sfxSampleIdx; });
    iinst("afn_snd_sfx_gain",   [&](int i){ return soundInstances[i].mixerGain; });
    iinst("afn_snd_sfx_fifo",   [&](int i){ return soundInstances[i].fifoChannel; });

    f << "\n#define AFN_HAS_SOUND 1\n";
    return true;
}

// ---- player collision proxy box (psp_player.h) ----------------------------
// The rig asset's authored collision box (collisionType/colCenter/colExtents,
// half-extents in rig-local space) baked to WORLD units so the runtime collision
// (source/collision.c) uses it instead of the hardcoded 6x24 cylinder. World =
// model * (spriteScale/4), matching the rig's render scale (AFN_PLAYER_RIG_SCALE).
// Horizontal radius = the larger XZ half-extent; vertical band = center +/- Y
// half-extent (offset from the floor-snapped player Y). Always written so
// collision.c can #include it; emits nothing when there's no box.
static bool GeneratePSPPlayerCol(const std::string& runtimeDir, const char* hdrPrefix,
                                 const std::vector<PSPRigExport>& pspRigs,
                                 int playerRigIdx,
                                 const std::vector<GBASpriteExport>& sprites,
                                 std::string& errorMsg) {
    std::string path = runtimeDir + "\\include\\" + hdrPrefix + "player.h";
    std::ofstream f(path);
    if (!f) { errorMsg = "Could not open " + path + " for writing"; return false; }
    f << "// Affinity PSP player collision — GENERATED by Export PSP. Do not edit.\n#pragma once\n\n";

    if (playerRigIdx < 0 || playerRigIdx >= (int)pspRigs.size() ||
        pspRigs[playerRigIdx].collisionType != 1) {
        f << "// (no custom collision box — runtime uses its default cylinder)\n";
        return true;
    }
    const PSPRigExport& rig = pspRigs[playerRigIdx];

    float pscale = 1.0f;
    for (const auto& s : sprites)
        if (s.spriteType == 1 && s.riggedMeshIdx == playerRigIdx) { pscale = s.scale; break; }
    float S = pscale * 0.25f;

    float exx = rig.colExtents[0], eyy = rig.colExtents[1], ezz = rig.colExtents[2];
    float radius = (exx > ezz ? exx : ezz) * S;
    float bottom = (rig.colCenter[1] - eyy) * S;
    float top    = (rig.colCenter[1] + eyy) * S;

    char b[96];
    f << "#define AFN_HAS_PLAYER_COL 1\n";
    snprintf(b, sizeof(b), "#define AFN_PLAYER_COL_RADIUS (%.4ff)\n", radius); f << b;
    snprintf(b, sizeof(b), "#define AFN_PLAYER_COL_BOTTOM (%.4ff)\n", bottom); f << b;
    snprintf(b, sizeof(b), "#define AFN_PLAYER_COL_TOP (%.4ff)\n",    top);    f << b;
    return true;
}

// ---- public entry ---------------------------------------------------------
// Write the full set of runtime headers (mapdata/rig/sky/sprites/sound/player)
// for a PSP-data-layout runtime. `hdrPrefix` selects the filename prefix
// ("psp_" or "psv_") and `dataInclude` the scene-data contract header
// (affinity_psp.h / affinity_psv.h). The PSP and PS Vita runtimes share an
// identical data layout, so both targets reuse this same generator — only the
// downstream build step (pspdev vs vitasdk) differs.
bool GenerateAffinityHeaders(const std::string& runtimeDir,
                             const char* hdrPrefix, const char* dataInclude,
                             const std::vector<GBASpriteExport>& sprites,
                             const std::vector<GBASpriteAssetExport>& assets,
                             const GBACameraExport& camera,
                             const std::vector<GBAMeshExport>& meshes,
                             float orbitDist,
                             const std::vector<GBASoundSampleExport>& soundSamples,
                             const std::vector<GBASoundInstanceExport>& soundInstances,
                             const std::vector<GBASkyFrameExport>& skyFrames,
                             const std::vector<PSPRigExport>& pspRigs,
                             int playerRigIdx,
                             std::string& errorMsg,
                             bool emitRig) {
    if (!GeneratePSPMapData(runtimeDir, hdrPrefix, dataInclude, sprites, camera, meshes, orbitDist, errorMsg))
        return false;
    if (emitRig && !GeneratePSPRigData(runtimeDir, hdrPrefix, pspRigs, playerRigIdx, sprites, errorMsg))
        return false;
    if (!GeneratePSPSky(runtimeDir, hdrPrefix, skyFrames, errorMsg))
        return false;
    if (!GeneratePSPSprites(runtimeDir, hdrPrefix, sprites, assets, errorMsg))
        return false;
    if (!GeneratePSPSound(runtimeDir, hdrPrefix, soundSamples, soundInstances, errorMsg))
        return false;
    if (!GeneratePSPPlayerCol(runtimeDir, hdrPrefix, pspRigs, playerRigIdx, sprites, errorMsg))
        return false;
    return true;
}

bool PackagePSP(const std::string& runtimeDir,
                const std::string& outputPath,
                const std::vector<GBASpriteExport>& sprites,
                const std::vector<GBASpriteAssetExport>& assets,
                const GBACameraExport& camera,
                const std::vector<GBAMeshExport>& meshes,
                float orbitDist,
                const std::vector<GBASoundSampleExport>& soundSamples,
                const std::vector<GBASoundInstanceExport>& soundInstances,
                const std::vector<GBASkyFrameExport>& skyFrames,
                const GBAScriptExport& /*script*/,
                const std::vector<GBABlueprintExport>& /*blueprints*/,
                const std::vector<GBABlueprintInstanceExport>& /*bpInstances*/,
                const std::vector<GBAHudElementExport>& /*hudElements*/,
                const std::vector<GBATmSceneExport>& /*tmScenes*/,
                int /*startMode*/,
                float /*midiMasterDb*/,
                const std::vector<GBARiggedMeshExport>& /*rigs*/,
                const std::vector<PSPRigExport>& pspRigs,
                int playerRigIdx,
                std::string& errorMsg) {
    if (!GenerateAffinityHeaders(runtimeDir, "psp_", "affinity_psp.h",
                                 sprites, assets, camera, meshes, orbitDist,
                                 soundSamples, soundInstances, skyFrames,
                                 pspRigs, playerRigIdx, errorMsg))
        return false;

    // Build EBOOT.PBP via WSL/pspdev.
    std::string wslDir = ToWslPath(runtimeDir);
    // `make clean` first: only psp_mapdata.h changes between exports, and the
    // pspsdk Makefile doesn't track header deps, so a plain `make` would leave
    // mapdata.o (and the EBOOT) stale. Use `;` not `&&` so the build still runs
    // if clean trips on a locked EBOOT (e.g. PPSSPP holding it open).
    std::string buildCmd = "cd '" + wslDir + "' && make clean; make 2>&1";
    std::string out;
    int rc = RunWslCommand(buildCmd, out);
    if (rc != 0) {
        errorMsg = "psp_mapdata.h generated, but the PSP build failed (rc=" + std::to_string(rc) + "):\n" + out
                 + "\n\nIf pspdev/WSL isn't set up yet, the header is still written — build manually with `make` in psp_runtime.";
        return false;
    }

    // Copy EBOOT.PBP to the requested output if a path was given.
    if (!outputPath.empty()) {
        std::string src = wslDir + "/EBOOT.PBP";
        std::string dst = ToWslPath(outputPath);
        std::string cp = "cp '" + src + "' '" + dst + "' 2>&1";
        std::string cpout;
        RunWslCommand(cp, cpout);   // best-effort
    }
    return true;
}

} // namespace Affinity
