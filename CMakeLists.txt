cmake_minimum_required(VERSION 3.16)

# Source lives under src/ (separated from build output / ROM / IDA db at the
# repo root). The ESP-IDF *project root* stays here at the repo root, so build/,
# sdkconfig, partitions.csv and the Docker mount are unaffected; we only point
# the component search at the relocated tree. "src/components" is the parent of
# all real components; "src/main" is the main component dir itself (it declares
# explicit REQUIRES, so it needs no special root placement). src/host is the
# non-IDF host harness and is intentionally NOT listed here.
set(EXTRA_COMPONENT_DIRS "src/components" "src/main")

# This checkout defaults to the JC4827W543. Select M5Paper explicitly with
# -DIDF_TARGET=esp32 -DSDKCONFIG_DEFAULTS=sdkconfig.defaults in a fresh build.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults.jc4827w543")
endif()
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET "esp32s3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonfruit)
