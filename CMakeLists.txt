cmake_minimum_required(VERSION 3.16)
project(AffinityEditor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------- GLFW ----------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/glfw)

# ---------- ImGui (built as object library) ----------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# ---------- Affinity Editor ----------
add_executable(AffinityEditor WIN32
    src/editor/main.cpp
    src/editor/frame_loop.cpp
    src/viewport/mode7_preview.cpp
    src/platform/gba/gba_package.cpp
    src/platform/nds/nds_package.cpp
    affinity.rc
)

target_include_directories(AffinityEditor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/thirdparty
)

target_link_libraries(AffinityEditor PRIVATE
    imgui
    glfw
    opengl32
    gdi32
    user32
    shell32
    comdlg32
)

# ImGui docking branch define
target_compile_definitions(AffinityEditor PRIVATE IMGUI_ENABLE_DOCKING=1)
