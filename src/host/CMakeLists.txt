cmake_minimum_required(VERSION 3.16)
project(dragonfruit_host C)

set(CMAKE_C_STANDARD 11)

# All emulator source files (no it8951/input/rom_loader — ESP-only). The layout
# engine + the platform-neutral display modules (silkscreen synthesis, the
# sink-agnostic surface sampler) compile unchanged on the host, so the host UI
# renders through the SAME layout/silkscreen/surface code the device uses.
# The m68k core is built as a single unity TU (m68k_all.c #includes the rest)
# so it matches the firmware build exactly and so the force-inlined hot opcode
# handlers (static inline, called from m68k_execute's switch) resolve.
set(EMU_SOURCES
    host_main.c
    host_ui.c
    ../components/m68k/m68k_all.c
    ../components/bus/bus.c
    ../components/dragonball/dragonball.c
    ../components/dragonball/interrupt.c
    ../components/dragonball/lcd.c
    ../components/dragonball/timer.c
    ../components/dragonball/gpio.c
    ../components/dragonball/spi.c
    ../components/dragonball/uart.c
    ../components/dragonball/pwm.c
    ../components/dragonball/rtc.c
    ../components/installer/installer.c
    ../components/layout/layout.c
    ../components/display/silkscreen.c
    ../components/display/surface.c
    ../components/ram_state/ram_state.c
    ../components/palm_card/palm_card.c
)

add_executable(dragonfruit ${EMU_SOURCES})

target_include_directories(dragonfruit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # esp_shim.h
    ../components/m68k/include
    ../components/bus/include
    ../components/dragonball/include
    ../components/installer/include
    ../components/layout/include
    ../components/display                  # silkscreen.h, surface.h
    ../components/ram_state/include
    ../components/palm_card/include
)

# Use our shim instead of real ESP headers
target_compile_definitions(dragonfruit PRIVATE
    HOST_BUILD=1
)
set(PALM_RAM_MB "2" CACHE STRING "Emulated Palm RAM in MB (2 or 4)")
if(NOT PALM_RAM_MB MATCHES "^[24]$")
    message(FATAL_ERROR "PALM_RAM_MB must be 2 or 4")
endif()
math(EXPR PALM_RAM_BYTES "${PALM_RAM_MB} * 1024 * 1024")
target_compile_definitions(dragonfruit PRIVATE BUS_RAM_SIZE=${PALM_RAM_BYTES})

if(MSVC)
    target_compile_options(dragonfruit PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        /W3 /O2
    )
else()
    target_compile_options(dragonfruit PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
        -O2 -g
    )
endif()

# --- Optional interactive UI (SDL3) -------------------------------------
# OFF by default so `cmake -S src/host -B build-host` needs no network and links a
# headless dragonfruit. With -DHOST_UI=ON, SDL3 is fetched + built from source and
# the --ui window (host_ui.c) is compiled in.
option(HOST_UI "Build the interactive SDL3 UI (fetches SDL3 from source)" OFF)
if(HOST_UI)
    include(FetchContent)
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS  OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.x
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)

    target_compile_definitions(dragonfruit PRIVATE HOST_UI=1)
    target_link_libraries(dragonfruit PRIVATE SDL3::SDL3)

    # SDL3.dll must sit next to the exe on Windows.
    if(WIN32)
        add_custom_command(TARGET dragonfruit POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:dragonfruit>)
    endif()
endif()

# --- Layout transform self-test (host-only) -----------------------------
# The layout module is pure C with no ESP deps, so it compiles unchanged on
# the host. This target asserts the default layout reproduces the stock
# geometry and that the transforms round-trip across rotations/presets.
add_executable(test_layout
    test_layout.c
    ../components/layout/layout.c
)
target_include_directories(test_layout PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # esp_log.h -> esp_shim.h stub
    ../components/layout/include
)
if(MSVC)
    target_compile_options(test_layout PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h /W3
    )
else()
    target_compile_options(test_layout PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        -Wall -Wextra -Wno-unused-parameter -O2
    )
endif()

# --- digitizer.cfg / GT911 config self-test (host-only) -----------------
# touch_config.c is pure C (no ESP/I2C deps), so it builds standalone. This
# target locks the GT911 checksum algorithm against the real factory block and
# exercises the parse/serialize + lenient/hysteresis behavior.
add_executable(test_touch_config
    test_touch_config.c
    ../components/input/touch_config.c
)
target_include_directories(test_touch_config PRIVATE
    ../components/input/include
)
if(MSVC)
    target_compile_options(test_touch_config PRIVATE /W3)
else()
    target_compile_options(test_touch_config PRIVATE
        -Wall -Wextra -Wno-unused-parameter -O2
    )
endif()

# --- JIT neutral-cache self-test (host-only) ----------------------------
# The architecture-neutral block cache (m68k_jit_cache.c) is pure C with no
# FreeRTOS/ISA dependencies, so it builds standalone via the esp_shim stubs.
# Exercises the segmented-ring eviction + hash/presence rebuild.
add_executable(test_jit_cache
    test_jit_cache.c
    ../components/m68k/m68k_jit_cache.c
)
target_include_directories(test_jit_cache PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # esp_shim.h (esp_heap_caps/esp_log stubs)
    ../components/m68k                     # m68k_jit_cache.h
    ../components/m68k/include             # m68k.h
    ../components/bus/include              # bus.h
)
if(MSVC)
    target_compile_options(test_jit_cache PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h /W3
    )
else()
    target_compile_options(test_jit_cache PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        -Wall -Wextra -Wno-unused-parameter -O2
    )
endif()

# --- JIT codegen fingerprint (host-only) --------------------------------
# Runs the Xtensa translator over a real ROM on the host (bytes are generated and
# hashed, never executed) — demonstrating the code generator is architecture-
# independent C — and FNV-1a's the output as a regression fingerprint. Links only
# the backend; everything it targets is stubbed in the test. Pass a ROM path.
add_executable(test_jit_codegen
    test_jit_codegen.c
    ../components/m68k/m68k_jit_backend_xtensa.c
)
target_include_directories(test_jit_codegen PRIVATE
    ../components/m68k                     # m68k_jit_backend.h, jit_execmem.h
    ../components/m68k/include             # m68k.h
    ../components/bus/include              # bus.h
)
if(MSVC)
    # /wd4505 unreferenced static fn (the emitter exposes the full ISA; codegen
    # uses a subset), /wd4068 unknown #pragma GCC diagnostic.
    target_compile_options(test_jit_codegen PRIVATE /W3 /wd4505 /wd4068)
else()
    target_compile_options(test_jit_codegen PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O2
    )
endif()

# SD-ROM loader: actual file I/O and validation, fake mount/PSRAM only.
add_executable(test_rom_loader test_rom_loader.c ../components/rom_loader/rom_loader.c)
target_include_directories(test_rom_loader PRIVATE
    rom_test_support ../components/rom_loader/include ../components/bus/include)
target_compile_definitions(test_rom_loader PRIVATE
    TEST_SD_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-sd")
# Keep test assertions active even in Release builds.
if(NOT MSVC)
    target_compile_options(test_rom_loader PRIVATE -Wall -Wextra -UNDEBUG)
endif()

enable_testing()
add_executable(test_h8s test_h8s.c ../components/h8s/h8s.c)
target_include_directories(test_h8s PRIVATE ../components/h8s/include)
add_test(NAME h8s COMMAND test_h8s)

set(CYBIKO_SOURCES
    ../components/cybiko/cybiko.c
    ../components/cybiko/cy_periph.c
    ../components/cybiko/cy_devices.c
    ../components/cybiko/cy_dma.c
    ../components/cybiko/cy_disk.c
    ../components/cybiko/cy_hle.c
    ../components/cybiko/cy_rf.c
    ../components/h8s/h8s.c
)
add_executable(test_cybiko test_cybiko.c ${CYBIKO_SOURCES})
target_include_directories(test_cybiko PRIVATE ../components/cybiko ../components/cybiko/include ../components/h8s/include)
add_test(NAME cybiko COMMAND test_cybiko)

# Headless runner for the real Cybiko Xtreme ROM/flash dumps (see cybiko_host.c)
add_executable(cybiko_host cybiko_host.c ${CYBIKO_SOURCES})
target_include_directories(cybiko_host PRIVATE ../components/cybiko/include ../components/h8s/include)
if(NOT MSVC)
    target_compile_options(test_cybiko PRIVATE -O2 -Wall -Wextra)
    target_compile_options(cybiko_host PRIVATE -O2 -Wall -Wextra)
endif()

add_test(NAME layout COMMAND test_layout)
add_test(NAME touch_config COMMAND test_touch_config)
add_test(NAME jit_cache COMMAND test_jit_cache)
add_test(NAME rom_loader COMMAND test_rom_loader)

add_executable(test_sd_mount test_sd_mount.c ../components/m5_sd/m5_sd.c)
target_include_directories(test_sd_mount PRIVATE sd_test_support
    ../components/m5_sd/include rom_test_support ../components/board/include)
if(MSVC)
    target_compile_options(test_sd_mount PRIVATE /UNDEBUG)
    target_compile_options(test_rom_loader PRIVATE /UNDEBUG)
else()
    target_compile_options(test_sd_mount PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME sd_mount COMMAND test_sd_mount)

add_executable(test_resistive_touch test_resistive_touch.c
    ../components/input/resistive_touch.c ../components/layout/layout.c)
target_include_directories(test_resistive_touch PRIVATE
    rom_test_support ../components/input/include ../components/layout/include)
if(MSVC)
    target_compile_options(test_resistive_touch PRIVATE /UNDEBUG)
else()
    target_compile_options(test_resistive_touch PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME resistive_touch COMMAND test_resistive_touch)

# ADS7846 command framing and SPI interrupt behavior, without a Palm ROM.
add_executable(test_spi test_spi.c ../components/dragonball/spi.c)
target_include_directories(test_spi PRIVATE ../components/dragonball/include)
if(MSVC)
    target_compile_options(test_spi PRIVATE /UNDEBUG)
else()
    target_compile_options(test_spi PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME spi COMMAND test_spi)

add_executable(test_ram_state test_ram_state.c ../components/ram_state/ram_state.c)
target_include_directories(test_ram_state PRIVATE ../components/ram_state/include)
if(MSVC)
    target_compile_options(test_ram_state PRIVATE /UNDEBUG)
else()
    target_compile_options(test_ram_state PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME ram_state COMMAND test_ram_state)

# Dual-mode LCD controller (DragonBall EZ + Epson SED1375) and color surface sampler.
add_executable(test_sed1375
    test_sed1375.c
    ../components/dragonball/lcd.c
    ../components/layout/layout.c
    ../components/display/surface.c
)
target_include_directories(test_sed1375 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../components/dragonball/include
    ../components/layout/include
    ../components/display
)
if(MSVC)
    target_compile_options(test_sed1375 PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h /W3 /UNDEBUG
    )
else()
    target_compile_options(test_sed1375 PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        -Wall -Wextra -Wno-unused-parameter -UNDEBUG -O2
    )
endif()
add_test(NAME sed1375 COMMAND test_sed1375)

# Exercise the actual 68k TRAP #15 ABI and directory-backed VFS. With a local
# ROM argument this also boots Palm and tests native database/handle interop.
set(CARD_TEST_SOURCES ${EMU_SOURCES})
list(REMOVE_ITEM CARD_TEST_SOURCES host_main.c)
add_executable(test_palm_card test_palm_card.c ${CARD_TEST_SOURCES})
foreach(prop INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS)
    get_target_property(card_${prop} dragonfruit ${prop})
    set_property(TARGET test_palm_card PROPERTY ${prop} "${card_${prop}}")
endforeach()
if(MSVC)
    target_compile_options(test_palm_card PRIVATE /UNDEBUG)
else()
    target_compile_options(test_palm_card PRIVATE -UNDEBUG)
endif()
if(HOST_UI)
    target_link_libraries(test_palm_card PRIVATE SDL3::SDL3)
endif()
add_test(NAME palm_card COMMAND test_palm_card "${CMAKE_CURRENT_BINARY_DIR}/test-card")

add_executable(test_portal_protocol test_portal_protocol.c
    ../components/wifi_gateway/portal_protocol.c)
target_include_directories(test_portal_protocol PRIVATE ../components/wifi_gateway/include)
add_executable(test_uart test_uart.c ../components/dragonball/uart.c)
target_include_directories(test_uart PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ../components/dragonball/include)
add_executable(test_pwm test_pwm.c ../components/dragonball/pwm.c)
target_include_directories(test_pwm PRIVATE ../components/dragonball/include)
if(NOT MSVC)
    target_link_libraries(test_pwm PRIVATE m)
endif()
foreach(target test_portal_protocol test_uart test_pwm)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -UNDEBUG)
    endif()
endforeach()
add_test(NAME portal_protocol COMMAND test_portal_protocol)
add_test(NAME uart COMMAND test_uart)
add_test(NAME pwm COMMAND test_pwm)

add_executable(test_web_proxy test_web_proxy.c ../components/wifi_gateway/web_proxy.c)
target_include_directories(test_web_proxy PRIVATE ../components/wifi_gateway/include)
if(MSVC)
    target_compile_options(test_web_proxy PRIVATE /UNDEBUG)
else()
    target_compile_options(test_web_proxy PRIVATE -Wall -Wextra -UNDEBUG)
endif()
add_test(NAME web_proxy COMMAND test_web_proxy)

if(UNIX)
    add_executable(test_web_proxy_http test_web_proxy_http.c
        ../components/wifi_gateway/web_proxy_http.c ../components/wifi_gateway/web_proxy.c
        ../components/wifi_gateway/portal_protocol.c)
    target_include_directories(test_web_proxy_http PRIVATE proxy_test_support
        ../components/wifi_gateway/include)
    target_compile_options(test_web_proxy_http PRIVATE -Wall -Wextra -UNDEBUG)
    add_test(NAME web_proxy_http COMMAND test_web_proxy_http)
endif()


# --- 68k emulator benchmark (host-only) ---------------------------------
# Runs the bundled 68000 workload (src/bench) through the same unity-built
# interpreter as the firmware and checks its checksum. Use it as a correctness
# net and a relative speed check for core changes.
add_executable(bench_m68k
    bench_m68k.c
    ../bench/bench_core.c
    ../components/m68k/m68k_all.c
    ../components/bus/bus.c
    ../components/dragonball/dragonball.c
    ../components/dragonball/interrupt.c
    ../components/dragonball/lcd.c
    ../components/dragonball/timer.c
    ../components/dragonball/gpio.c
    ../components/dragonball/spi.c
    ../components/dragonball/uart.c
    ../components/dragonball/rtc.c
    ../components/dragonball/pwm.c
)
target_include_directories(bench_m68k PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../bench
    ../components/m68k/include
    ../components/bus/include
    ../components/dragonball/include
)
target_compile_definitions(bench_m68k PRIVATE HOST_BUILD=1 BUS_RAM_SIZE=${PALM_RAM_BYTES})
if(NOT MSVC)
    target_compile_options(bench_m68k PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/esp_shim.h
        -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -O2)
endif()
add_test(NAME bench_m68k COMMAND bench_m68k 1)
