# Windows 98 SE userland on pcrecomp's native32 (32-bit host, MSVC x86).
#   py -3 tools/lift.py calc work/sys/calc.exe   (README)
#   build.cmd
cmake_minimum_required(VERSION 3.20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(win98 C)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 OR NOT MSVC)
  message(FATAL_ERROR "the host must be 32-bit MSVC: use build.cmd")
endif()
set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp-w98" CACHE PATH "pcrecomp checkout")
include(${PCRECOMP}/cmake/pcrecomp_gen.cmake)

set(N32 ${PCRECOMP}/runtime/native32)
function(w98_host name main)
  set(gen ${name})
  if(W98_GEN_${name})
    set(gen ${W98_GEN_${name}})   # another host's lift
  endif()
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/gen/${gen}/recomp_dispatch.c)
    message(STATUS "${name}: not lifted, skipped")
    return()
  endif()
  add_executable(${name} ${main} ${N32}/native32.c ${N32}/modules32.c ${N32}/regbox32.c
                 ${PCRECOMP}/runtime/recomp32/image_loader.c ${PCRECOMP}/runtime/recomp32/recomp_trace.c ${ARGN})
  pcrecomp_gen_sources(${name} DIR ${CMAKE_SOURCE_DIR}/src/gen/${gen})
  target_include_directories(${name} PRIVATE src src/gen/${gen} ${N32} ${PCRECOMP}/runtime/recomp32)
  target_compile_definitions(${name} PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                             RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
  target_link_options(${name} PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO /NXCOMPAT
                      /LARGEADDRESSAWARE /STACK:0x40000 /MANIFESTUAC:NO)
  target_link_libraries(${name} PRIVATE user32 gdi32 kernel32 winmm advapi32)
endfunction()

# Stage 2: Windows 98's PE accessories on the host's DLLs
w98_host(calc src/apphost.c)
if(TARGET calc)
  target_compile_definitions(calc PRIVATE APP_EXE="calc.exe" APP_BASE=0x01000000u)
endif()
w98_host(notepad src/apphost.c)
if(TARGET notepad)
  target_compile_definitions(notepad PRIVATE APP_EXE="notepad.exe" APP_BASE=0x00400000u)
endif()

# Milestone 1: xcopy32 on Windows 98's own lifted kernel32, VxD calls answered by vxd32.c
w98_host(xcopy src/k32host.c ${N32}/vxd32.c)
if(TARGET xcopy)
  target_link_libraries(xcopy PRIVATE dbghelp)
  target_compile_definitions(xcopy PRIVATE APP_EXE="xcopy32.mod" APP_NAME="xcopy" APP_BASE=0x00400000u RECOMP_TRACE=1)
endif()

# Milestone 2: USER.EXE, GDI.EXE, KRNL386.EXE lifted by ne_link (NE_IDENTITY_SEL=1)
# into work/w16, built for Win16 identity mode (cpu.h RECOMP_W16_IDENTITY):
# 16-bit selectors are native32 LDT entries, cpu->mem is 0.
#   set NE_IDENTITY_SEL=1 & py -3 ../pcrecomp-w98/tools/ne/ne_link.py --work work/w16 work/sys/KRNL386.EXE work/sys/GDI.EXE work/sys/USER.EXE
file(GLOB W16_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/work/w16/src/*.c)
if(W16_SOURCES)
  add_library(w16 STATIC ${W16_SOURCES})
  target_include_directories(w16 PUBLIC ${CMAKE_SOURCE_DIR}/work/w16/runtime ${PCRECOMP}/runtime/win16)
  target_compile_definitions(w16 PUBLIC RECOMP_W16_IDENTITY _CRT_SECURE_NO_WARNINGS)
  target_compile_options(w16 PRIVATE /w /bigobj)
endif()

# Milestone 2, chunk 1: kernel32's QT_Thunk into lifted USER.EXE (src/w16test.c).
# Reuses xcopy's lift (kernel32 + a program to boot with).
if(TARGET w16 AND EXISTS ${CMAKE_SOURCE_DIR}/src/gen/xcopy/recomp_dispatch.c)
  set(W98_GEN_w16test xcopy)
  w98_host(w16test src/k32host.c ${N32}/vxd32.c src/w16test.c ${PCRECOMP}/runtime/win16/identity/identity.c)
  set_source_files_properties(src/w16test.c ${PCRECOMP}/runtime/win16/identity/identity.c PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/work/w16/runtime;${PCRECOMP}/runtime/win16"
    COMPILE_DEFINITIONS "RECOMP_W16_IDENTITY;W16_IMAGE=\"${CMAKE_SOURCE_DIR}/work/w16/mem_image.bin\"")
  target_compile_definitions(w16test PRIVATE APP_EXE="xcopy32.mod" APP_NAME="xcopy" APP_BASE=0x00400000u RECOMP_TRACE=1
                             W16_IMAGE="${CMAKE_SOURCE_DIR}/work/w16/mem_image.bin")
  target_link_libraries(w16test PRIVATE dbghelp w16)
  # Win16 identity holds 0x10000-0x10FFFF in its relaunch (native32_hold_range):
  # with /DYNAMICBASE:NO the suspended child already has its process
  # parameters and heaps bottom-up there; an ASLR image moves them.
  target_link_options(w16test PRIVATE /DYNAMICBASE)
endif()
