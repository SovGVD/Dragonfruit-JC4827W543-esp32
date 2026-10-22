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

# The generated sdkconfig keeps its old values when the tracked defaults
# change, so it is started again from the defaults whenever they are newer
# (a pull that changes them). menuconfig changes made after that stay.
if(DEFINED SDKCONFIG)
    set(_dragonfruit_sdkconfig "${SDKCONFIG}")
else()
    set(_dragonfruit_sdkconfig "sdkconfig")
endif()
get_filename_component(_dragonfruit_sdkconfig "${_dragonfruit_sdkconfig}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
foreach(_dragonfruit_defaults ${SDKCONFIG_DEFAULTS})
    get_filename_component(_dragonfruit_defaults "${_dragonfruit_defaults}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dragonfruit_defaults}")
    if(EXISTS "${_dragonfruit_sdkconfig}" AND "${_dragonfruit_defaults}" IS_NEWER_THAN "${_dragonfruit_sdkconfig}")
        message(STATUS "${_dragonfruit_defaults} changed: regenerating ${_dragonfruit_sdkconfig}")
        file(REMOVE "${_dragonfruit_sdkconfig}")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonfruit)
