idf_component_register(
    SRCS "rom_loader.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES m5_sd bus
)
