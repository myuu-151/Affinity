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
    // 1) Regenerate the runtime headers (same data as PSP; "psv_" prefix +
    //    "affinity_psv.h" data contract).
    if (!GenerateAffinityHeaders(runtimeDir, "psv_", "affinity_psv.h",
                                 sprites, assets, camera, meshes, orbitDist,
                                 soundSamples, soundInstances, skyFrames,
                                 pspRigs, playerRigIdx, errorMsg))
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
