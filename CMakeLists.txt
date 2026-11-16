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
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/gen/${name}/recomp_dispatch.c)
    message(STATUS "${name}: not lifted, skipped")
    return()
  endif()
  add_executable(${name} ${main} ${N32}/native32.c ${N32}/modules32.c ${N32}/regbox32.c
                 ${PCRECOMP}/runtime/recomp32/image_loader.c ${PCRECOMP}/runtime/recomp32/recomp_trace.c ${ARGN})
  pcrecomp_gen_sources(${name} DIR ${CMAKE_SOURCE_DIR}/src/gen/${name})
  target_include_directories(${name} PRIVATE src src/gen/${name} ${N32} ${PCRECOMP}/runtime/recomp32)
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
