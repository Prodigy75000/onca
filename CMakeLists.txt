# Onca - from-scratch Atari Jaguar libretro core
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.15)
project(onca C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

set(CORE_SRC
  src/m68k.c
  src/memory.c
  src/blitter.c
  src/op.c
  src/gpu.c
  src/bios.c
)

# libretro core (shared library: onca_libretro.so / .dll / .dylib)
add_library(onca_libretro SHARED ${CORE_SRC} src/libretro_core.c)
set_target_properties(onca_libretro PROPERTIES PREFIX "")

# boot trace harness
add_executable(onca_trace ${CORE_SRC} tools/trace.c)

# fake-bios generator (for pipeline testing without a real image)
add_executable(onca_makefakebios tools/makefakebios.c)

# object-list analysis + headless Object Processor preview
add_executable(onca_opdump ${CORE_SRC} tools/opdump.c)
add_executable(onca_render ${CORE_SRC} tools/render.c)

# headless libretro frontend (drives the core like RetroArch, for verification)
add_executable(onca_host ${CORE_SRC} src/libretro_core.c tools/libretro_host.c)
target_compile_definitions(onca_host PRIVATE RETRO_API=)

# unit tests
add_executable(onca_test_m68k src/m68k.c tests/test_m68k.c)
add_executable(onca_test_op src/m68k.c src/memory.c src/blitter.c src/op.c src/gpu.c tests/test_op.c)
add_executable(onca_test_gpu src/memory.c src/blitter.c src/gpu.c tests/test_gpu.c)
enable_testing()
add_test(NAME m68k COMMAND onca_test_m68k)
add_test(NAME op COMMAND onca_test_op)
add_test(NAME gpu COMMAND onca_test_gpu)
