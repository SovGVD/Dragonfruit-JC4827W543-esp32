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
    ../components/dragonball/rtc.c
    ../components/installer/installer.c
    ../components/layout/layout.c
    ../components/display/silkscreen.c
    ../components/display/surface.c
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
)

# Use our shim instead of real ESP headers
target_compile_definitions(dragonfruit PRIVATE
    HOST_BUILD=1
)

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
