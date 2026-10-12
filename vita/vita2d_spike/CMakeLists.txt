cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(vita2d_spike C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "vita2d-spike")
set(VITA_TITLEID "PSNIN0004")
set(VITA_VERSION "00.01")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

set(ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../PsnineTrophies4Vita)

add_executable(${PROJECT_NAME} main.c)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -O2)
target_link_libraries(${PROJECT_NAME}
  vita2d
  freetype png bz2 z m
  SceCtrl_stub SceTouch_stub SceDisplay_stub SceGxm_stub SceSysmem_stub
  SceSysmodule_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
)

# bundle only the textures + font the spike loads, plus the LiveArea/icon assets
set(SPIKE_ASSETS
  bg.rgba rowbg.rgba sel.rgba
  cup26_0.rgba cup26_1.rgba cup26_2.rgba cup26_3.rgba cup26_4.rgba)
set(ASSET_FILES "")
foreach(a ${SPIKE_ASSETS})
  list(APPEND ASSET_FILES FILE ${ASSET_DIR}/assets/${a} assets/${a})
endforeach()

vita_create_self(eboot.bin ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${ASSET_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${ASSET_DIR}/sce_sys/pic0.png sce_sys/pic0.png
  FILE ${ASSET_DIR}/assets/font.ttf font.ttf
  ${ASSET_FILES}
)
