idf_component_register(
    SRCS "h8s.c"
    INCLUDE_DIRS "include"
)
# The interpreter's hot loop benefits from -O2 even in size-optimised builds.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
