cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(PsnineTrophies4Vita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# clangd (.vscode/settings.json --compile-commands-dir) needs this JSON to resolve cross-compile headers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VITA_APP_NAME "PsnineTrophies4Vita")
set(VITA_TITLEID "PSNIN0003")
set(VITA_VERSION "00.90")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

# -O3 must be given explicitly: the vitasdk toolchain only adds optimization when
# CMAKE_BUILD_TYPE=Release; left empty GCC defaults to -O0 and per-pixel compositing
# measured 385ms/frame on real hardware
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O3 -DNDEBUG")

set(GUMBO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gumbo-parser)
file(GLOB GUMBO_SOURCES ${GUMBO_DIR}/src/*.c)
set_source_files_properties(${GUMBO_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

add_executable(${PROJECT_NAME}
  src/main.c
  src/render.c
  src/data.c
  src/parse.c
  src/net.c
  ${GUMBO_SOURCES}
)
target_include_directories(${PROJECT_NAME} PRIVATE
  ${VITASDK}/arm-vita-eabi/include/freetype2
  ${GUMBO_DIR}/src
)
target_link_libraries(${PROJECT_NAME}
  freetype png jpeg bz2 z m
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmem_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceTouch_stub
  SceCommonDialog_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME})

# app0 assets: assets/*.rgba are pre-generated offline and checked in; new files are picked up by re-running cmake
file(GLOB APP_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets/*.rgba)
set(ASSET_FILES "")
foreach(a ${APP_ASSETS})
  get_filename_component(an ${a} NAME)
  list(APPEND ASSET_FILES FILE ${a} assets/${an})
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE assets/font.ttf font.ttf
  ${ASSET_FILES}
)
