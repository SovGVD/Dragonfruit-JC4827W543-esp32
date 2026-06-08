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
    "m68k_jit_core.c"       # neutral JIT driver (dispatch, async compiler, install)
    "m68k_jit_cache.c"      # neutral block hash + presence bitmap + segmented ring
)
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    list(APPEND m68k_srcs
        "m68k_jit_backend_xtensa.c"   # 68k -> Xtensa translator
        "jit_execmem_esp32.c")        # classic-ESP32 D/IRAM executable memory
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND m68k_srcs
        "m68k_jit_backend_riscv.c"    # placeholder: no codegen yet (interpreter-only)
        "jit_execmem_riscv.c")        # placeholder: no executable cache yet
endif()

idf_component_register(
    SRCS ${m68k_srcs}
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES bus esp_hw_support
)
