// Affinity GBA Mode 7 Engine — Editor
// ImGui + GLFW + OpenGL2

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <GLFW/glfw3.h>
#include <cstdio>

#include "frame_loop.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
#else
int main(int, char**)
#endif
{
    // ---- GLFW init ----
    if (!glfwInit())
    {
        fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Affinity — GBA Mode 7 Engine", nullptr, nullptr);
    if (!window)
    {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    // ---- Set window icon from embedded resource ----
#ifdef _WIN32
    {
        HWND hwnd = glfwGetWin32Window(window);
        HICON iconBig   = (HICON)LoadImageA(hInst, "IDI_ICON1", IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR);
        HICON iconSmall = (HICON)LoadImageA(hInst, "IDI_ICON1", IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        if (iconBig)   SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)iconBig);
        if (iconSmall) SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSmall);

        // Dark title bar
        BOOL useDark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    }
#endif

    // ---- ImGui init ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "affinity_imgui.ini";

    ImGui::StyleColorsDark();
    // Tweak style for retro feel
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 2.0f;
    style.FrameRounding     = 2.0f;
    style.GrabRounding      = 2.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    // ---- Editor init ----
    Affinity::FrameInit();

    // ---- Main loop ----
    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f; // clamp

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        Affinity::FrameTick(dt);

        ImGui::Render();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

        // Render 3D viewport on top of ImGui (after ImGui draw, before swap)
        Affinity::Render3DViewport();

        glfwSwapBuffers(window);
    }

    // ---- Cleanup ----
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
