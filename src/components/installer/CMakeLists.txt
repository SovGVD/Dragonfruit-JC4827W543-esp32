idf_component_register(
    SRCS "installer.c"
    INCLUDE_DIRS "include"
    REQUIRES m68k                       # installer.h exposes m68k_cpu_t
    PRIV_REQUIRES bus dragonball m5_sd
)
