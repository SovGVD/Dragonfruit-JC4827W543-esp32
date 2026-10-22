idf_component_register(
    SRCS "cybiko.c" "cy_periph.c" "cy_devices.c" "cy_dma.c" "cy_disk.c" "cy_hle.c" "cy_rf.c"
    INCLUDE_DIRS "include"
    REQUIRES h8s
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
