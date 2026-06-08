cmake_minimum_required(VERSION 3.16)

# Source lives under src/ (separated from build output / ROM / IDA db at the
# repo root). The ESP-IDF *project root* stays here at the repo root, so build/,
# sdkconfig, partitions.csv and the Docker mount are unaffected; we only point
# the component search at the relocated tree. "src/components" is the parent of
# all real components; "src/main" is the main component dir itself (it declares
# explicit REQUIRES, so it needs no special root placement). src/host is the
# non-IDF host harness and is intentionally NOT listed here.
set(EXTRA_COMPONENT_DIRS "src/components" "src/main")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonfruit)
