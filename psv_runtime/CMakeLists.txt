cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(affinity_psv C CXX)
include("$ENV{VITASDK}/share/vita.cmake")
include_directories(${CMAKE_SOURCE_DIR}/include)
# Detour (navmesh queries) — C++, headers + sources vendored in detour/
include_directories(${CMAKE_SOURCE_DIR}/detour)

set(VITA_APP_NAME "Affinity")
set(VITA_TITLEID  "AFNT00002")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -O2 -fno-exceptions -fno-rtti")

add_executable(affinity_psv
  main.c
  audio.c
  nav_bridge.cpp
  detour/DetourAlloc.cpp
  detour/DetourAssert.cpp
  detour/DetourCommon.cpp
  detour/DetourNavMesh.cpp
  detour/DetourNavMeshBuilder.cpp
  detour/DetourNavMeshQuery.cpp
  detour/DetourNode.cpp
)

# vitaGL + its dependencies, plus the Sce stubs the runtime uses.
target_link_libraries(affinity_psv
  vitaGL
  vitashark
  SceShaccCg_stub
  SceShaccCgExt
  taihen_stub
  stdc++
  mathneon
  SceGxm_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  ScePower_stub
  SceLibKernel_stub
  SceAudio_stub
  taihen_stub
  m
)

vita_create_self(affinity_psv.self affinity_psv UNSAFE)
vita_create_vpk(affinity_psv.vpk ${VITA_TITLEID} affinity_psv.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME})
