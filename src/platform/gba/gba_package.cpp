#include "gba_package.h"

#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Affinity
{

// Run a command via devkitPro's MSYS2 bash using CreateProcess
// so we bypass cmd.exe entirely.
static int RunDevkitBash(const std::string& cmd, std::string& output)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Create pipe for stdout/stderr
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    // Build command line: bash.exe -lc '<cmd>'
    std::string cmdLine = "C:\\devkitPro\\msys2\\usr\\bin\\bash.exe -lc '" + cmd + "'";

    BOOL ok = CreateProcessA(
        nullptr,
        (LPSTR)cmdLine.c_str(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    CloseHandle(hWritePipe);

    if (!ok)
    {
        CloseHandle(hReadPipe);
        output = "Failed to launch devkitPro bash";
        return -1;
    }

    // Read output
    output.clear();
    char buf[512];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
    {
        buf[bytesRead] = '\0';
        output += buf;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return (int)exitCode;
}

// Convert Windows path to MSYS2 path: C:\foo\bar -> /c/foo/bar
static std::string ToMsysPath(const std::string& winPath)
{
    std::string p = winPath;
    for (auto& c : p) if (c == '\\') c = '/';
    if (p.size() >= 2 && p[1] == ':')
    {
        char drive = (char)tolower(p[0]);
        p = "/" + std::string(1, drive) + p.substr(2);
    }
    return p;
}

bool PackageGBA(const std::string& runtimeDir,
                const std::string& outputPath,
                std::string& errorMsg)
{
    std::string msysDir = ToMsysPath(runtimeDir);

    // --- Step 1: Clean previous build ---
    std::string cleanOut;
    RunDevkitBash("cd " + msysDir + " && make clean", cleanOut);

    // --- Step 2: Build ---
    std::string buildOut;
    int ret = RunDevkitBash("cd " + msysDir + " && make", buildOut);

    if (ret != 0)
    {
        errorMsg = "GBA build failed:\n" + buildOut;
        return false;
    }

    // --- Step 3: Verify .gba exists ---
    fs::path srcPath = fs::path(runtimeDir) / "affinity.gba";
    fs::path dstPath(outputPath);

    if (!fs::exists(srcPath))
    {
        errorMsg = "Build succeeded but affinity.gba not found at: " + srcPath.string();
        return false;
    }

    // If output path differs from source, copy it
    std::error_code ec;
    fs::path srcCanon = fs::canonical(srcPath, ec);
    fs::path dstCanon = fs::canonical(dstPath, ec);
    if (srcCanon != dstCanon)
    {
        ec.clear();
        fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMsg = "Failed to copy ROM: " + ec.message();
            return false;
        }
    }

    // Return build log on success
    errorMsg = buildOut;
    return true;
}

} // namespace Affinity
