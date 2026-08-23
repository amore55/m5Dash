# WHOLE_ARCHIVE is load-bearing, not caution.
#
# LVGL calls lv_malloc_core() and friends, and LVGL is a DEPENDENCY of the components that use
# it — so lvgl's archive appears BEFORE ours on the link line. A static library cannot resolve
# symbols from an archive the linker has already walked past, so without WHOLE_ARCHIVE the
# build fails with "undefined reference to lv_malloc_core" even though this component compiles
# perfectly. Forcing every object in sidesteps link order entirely.
idf_component_register(
    SRCS
        "src/lvgl_heap.c"
    REQUIRES
        lvgl__lvgl
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
