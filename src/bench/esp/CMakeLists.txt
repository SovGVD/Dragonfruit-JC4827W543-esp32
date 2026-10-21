# Stand-alone ESP-IDF app that benchmarks the 68k core (interpreter and JIT) on
# the board, or under QEMU, with the bundled workload. It links only the m68k,
# bus and dragonball components from the main firmware, so the numbers isolate
# CPU emulation speed from display, touch, SD and Wi-Fi.
#
#   idf.py -C src/bench/esp -B build-bench set-target esp32s3 build flash monitor
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/m68k"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bus"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/dragonball")
set(COMPONENTS main)
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET "esp32s3")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m68k_bench)
