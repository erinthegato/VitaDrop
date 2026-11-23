cmake_minimum_required(VERSION 3.16)

# Use the VitaSDK compiler instead of your computer's normal one.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please install VitaSDK and set the VITASDK environment variable (see README.md).")
  endif()
endif()

project(VitaDrop C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaDrop")
set(VITA_TITLEID  "VDRP00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")

# Download the QR code library (Nayuki's "QR Code generator", MIT licence) at build time.
include(FetchContent)
FetchContent_Declare(qrcodegen
  URL https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(qrcodegen)

add_executable(${PROJECT_NAME}
  src/main.c
  src/server.c
  ${qrcodegen_SOURCE_DIR}/c/qrcodegen.c
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${qrcodegen_SOURCE_DIR}/c)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceNet_stub
  SceNetCtl_stub
  SceLibKernel_stub
  freetype
  png
  jpeg
  z
  m
  c
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
