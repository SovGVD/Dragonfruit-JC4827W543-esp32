idf_component_register(
    SRCS "cy_mp3.c"
    INCLUDE_DIRS "include"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
