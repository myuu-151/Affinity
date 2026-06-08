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

#include <string>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace Affinity {

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
            if (ReadFile(hRead, buf, toRead, &n, nullptr) && n > 0) { buf[n] = 0; output += buf; }
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

static void EmitRigArrays(std::ofstream& f, int ru, const PSPRigExport& rig) {
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

        const PSPRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : PSPRigMaterial{};
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
        const PSPRigMaterial& M = (g < (int)rig.materials.size()) ? rig.materials[g] : PSPRigMaterial{};
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
        const PSPRigClip& cl = rig.clips[c];
        int nf = cl.frameCount;
        f << "static const float " << S << "clip" << c << "[" << (nf*bc*7) << "] = {\n";
        for (int fr = 0; fr < nf; fr++)
            for (int b = 0; b < bc; b++) {
                const PSPRigBonePose& P = cl.frames[fr*bc + b];
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
                               const std::vector<PSPRigExport>& rigs,
                               int playerRigIdx,
                               const std::vector<GBASpriteExport>& sprites,
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

    float psx=0, psy=0, psz=0, pscale=1.0f; int pclip=0;
    for (const auto& s : sprites)
        if (s.spriteType == 1 && s.riggedMeshIdx == playerRigIdx) {
            psx=PVX(s.x); psy=PVY(s.y); psz=PVX(s.z); pscale=s.scale; pclip=s.rigAnimIdx; break;
        }

    struct Inst { float x,y,z,rot,scale; int clip, slot; };
    std::vector<Inst> npcs;
    for (const auto& s : sprites) {
        if (s.spriteType == 1) continue;
        int slot = useRig(s.riggedMeshIdx);
        if (slot < 0) continue;
        int clip = (s.rigAnimIdx >= 0 && s.rigAnimIdx < (int)rigs[s.riggedMeshIdx].clips.size()) ? s.rigAnimIdx : 0;
        npcs.push_back({ PVX(s.x), PVY(s.y), PVX(s.z), s.rotation, s.scale, clip, slot });
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
    }

    // NPC instances: { x, y, z (world px), rotY (deg), scale, clip, rig slot }.
    f << "#define AFN_NPC_COUNT " << npcs.size() << "\n";
    f << "static const float afn_npc_inst[" << (npcs.empty()?1:npcs.size()) << "][7] = {\n";
    for (const auto& n : npcs)
        f << "  { " << PFlt(n.x) << "," << PFlt(n.y) << "," << PFlt(n.z) << "," << PFlt(n.rot)
          << "," << PFlt(n.scale) << "," << n.clip << "," << n.slot << " },\n";
    if (npcs.empty()) f << "  {0,0,0,0,0,0,0},\n";
    f << "};\n";

    f.close();
    return true;
}

bool PackagePSV(const std::string& runtimeDir,
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
    // 1) Regenerate the shared headers (mapdata/sky/sprites/sound/player), but
    //    SKIP the single-rig generator (emitRig=false) — PSV emits its own
    //    multi-rig psv_rig.h so NPCs/enemies render their own models.
    if (!GenerateAffinityHeaders(runtimeDir, "psv_", "affinity_psv.h",
                                 sprites, assets, camera, meshes, orbitDist,
                                 soundSamples, soundInstances, skyFrames,
                                 pspRigs, playerRigIdx, errorMsg, /*emitRig=*/false))
        return false;
    if (!GeneratePSVRigData(runtimeDir, pspRigs, playerRigIdx, sprites, errorMsg))
        return false;

    // 2) Build affinity_psv.vpk via the VitaSDK CMake toolchain. cmake re-runs
    //    are cheap on an already-configured build dir, and CMake tracks header
    //    deps, so a regenerated psv_mapdata.h forces main.c to recompile.
    std::string msysDir = ToMsysPath(runtimeDir);
    std::string buildCmd =
        "export VITASDK=/c/vitasdk; export PATH=\\\"$VITASDK/bin:$PATH\\\"; "
        "cd '" + msysDir + "' && mkdir -p build && cd build && "
        "cmake .. && make 2>&1";
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
