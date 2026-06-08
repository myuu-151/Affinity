cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/editor/rigged_gltf.cpp
    src/viewport/mode7_preview.cpp
    src/platform/gba/gba_package.cpp
    src/platform/nds/nds_package.cpp
    src/platform/nds/dsma_emit.cpp
    src/platform/psp/psp_package.cpp
    src/platform/psv/psv_package.cpp
    src/platform/common/node_script_emit.cpp
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

# Bump stack reserve to 256 MB. LoadProject's frame is large (32 KB line
# buffer + VsNode locals), but the real driver is the many stack-local
# MapScene values (each embeds FloorSprite[256], and FloorSprite now holds
# RailPoint[512] = ~2.2 MB per MapScene). Several of those in the giant frame
# function overflow an 8 MB stack. The reserve is virtual address space on
# x64 (committed on demand), so a big number is essentially free.
if(MSVC)
    target_link_options(AffinityEditor PRIVATE /STACK:268435456)
endif()
