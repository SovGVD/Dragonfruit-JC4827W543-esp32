# The Xtensa backend runs on the classic ESP32 (LX6) and the ESP32-S3 (LX7): both
# cores share the windowed ABI and the base ISA the translator emits, and both
# expose internal SRAM through an instruction-bus alias for the code cache. The
# S3 additionally needs CONFIG_ESP_SYSTEM_MEMPROT off so heap memory can be
# executable (see sdkconfig.defaults.jc4827w543). RISC-V targets use the
# interpreter until their backend is written.
# The JIT is split into an architecture-neutral core (driver + block cache) and a
# per-ISA code-generation backend + chip-specific executable-memory unit, selected
# at build time by the target arch. Adding a RISC-V target (e.g. ESP32-P4) means
# supplying m68k_jit_backend_riscv.c + jit_execmem_riscv.c under the RISCV branch;
# the neutral core (m68k_jit_core.c / m68k_jit_cache.c) is shared unchanged.
set(m68k_srcs
    # Unity build: m68k_all.c #includes every interpreter m68k_*.c so the whole
    # interpreter compiles as one translation unit, letting -O2 inline the
    # per-instruction EA/flag call chain into the opcode handlers (see m68k_all.c).
    "m68k_all.c"
    "m68k_palm_hle.c"       # native Palm OS ROM code (its own TU, kept in flash)
    "m68k_jit_core.c"       # neutral JIT driver (dispatch, async compiler, install)
    "m68k_jit_cache.c"      # neutral block hash + presence bitmap + segmented ring
)
if(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND m68k_srcs "m68k_jit_backend_riscv.c" "jit_execmem_riscv.c")
elseif(NOT CONFIG_M68K_JIT)
    # JIT turned off in menuconfig, or a target without an Xtensa backend.
    list(APPEND m68k_srcs
        "m68k_jit_backend_disabled.c"    # placeholder: no codegen (interpreter-only)
        "jit_execmem_disabled.c")        # placeholder: no executable cache
elseif(CONFIG_IDF_TARGET_ESP32S3 AND CONFIG_ESP_SYSTEM_MEMPROT)
    # An sdkconfig generated before memory protection was turned off in the
    # defaults keeps it on; build interpreter-only rather than fail.
    message(WARNING "m68k JIT disabled: set CONFIG_ESP_SYSTEM_MEMPROT=n (or delete "
                    "the generated sdkconfig) to enable it on the ESP32-S3")
    list(APPEND m68k_srcs "m68k_jit_backend_disabled.c" "jit_execmem_disabled.c")
else()
    list(APPEND m68k_srcs
        "m68k_jit_backend_xtensa.c"   # 68k -> Xtensa translator
        "jit_execmem_esp32.c")        # ESP32 / ESP32-S3 D/IRAM executable memory
endif()

idf_component_register(
    SRCS ${m68k_srcs}
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES bus esp_hw_support
)

# ESP-IDF compiles everything with -fno-jump-tables -fno-tree-switch-conversion
# (so IRAM code never indexes a table in flash). That turns every switch in the
# interpreter — the per-opcode group dispatch, EA modes, condition codes — into
# a chain of compares and branches. Here it is safe to undo: linker.lf places
# this whole component "noflash", which puts its .rodata (where GCC emits the
# tables) in internal DRAM next to the IRAM code. These flags come after the
# global ones on the command line, so they win.
target_compile_options(${COMPONENT_LIB} PRIVATE -fjump-tables -ftree-switch-conversion)
