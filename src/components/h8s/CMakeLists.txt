idf_component_register(
    SRCS "h8s.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
)
# The interpreter's hot loop benefits from -O2 even in size-optimised builds.
# ESP-IDF builds with -fno-jump-tables so IRAM code never reads a table from
# flash; that turns the opcode switch into a long chain of compares. Here the
# tables are always next to the code: in internal DRAM when linker.lf puts the
# core in IRAM (CYBIKO_CORE_IRAM), in flash otherwise, where the code is too.
# The core never runs with the flash cache off.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
