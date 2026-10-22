idf_component_register(
    SRCS "bench_main.c" "../../bench_core.c"
    INCLUDE_DIRS "../.."
    REQUIRES m68k bus dragonball esp_timer esp_psram
)

# idf.py -DBENCH_QEMU=1 ...: QEMU does not alias the S3's IRAM and DRAM windows,
# so have the JIT write its code cache through the instruction address.
if(BENCH_QEMU)
    idf_component_get_property(m68k_lib m68k COMPONENT_LIB)
    target_compile_definitions(${m68k_lib} PRIVATE M68K_JIT_EXEC_WRITE_DIRECT=1)
endif()
