idf_component_register(
    SRCS "cybiko.c" "cy_periph.c" "cy_devices.c" "cy_dma.c" "cy_disk.c" "cy_hle.c" "cy_rf.c" "cy_cart.c"
    INCLUDE_DIRS "include"
    REQUIRES h8s
    LDFRAGMENTS "linker.lf"
)
# Jump tables for the register decode switches; see src/components/h8s/CMakeLists.txt.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables -ftree-switch-conversion)
