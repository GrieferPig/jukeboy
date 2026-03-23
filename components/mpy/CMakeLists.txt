set(MPY_COMPONENT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(MPY_SOURCE_DIR ${MPY_COMPONENT_DIR}/micropython)
set(MPY_EMBED_DIR ${MPY_SOURCE_DIR}/micropython_embed)
set(MPY_BUILD_DIR ${MPY_SOURCE_DIR}/build-embed)

file(GLOB_RECURSE MPY_CORE_SOURCES
    "${MPY_EMBED_DIR}/py/*.c"
    "${MPY_EMBED_DIR}/extmod/*.c"
    "${MPY_EMBED_DIR}/shared/*.c"
    "${MPY_EMBED_DIR}/lib/*.c"
    "${MPY_EMBED_DIR}/port/*.c"
)

list(FILTER MPY_CORE_SOURCES EXCLUDE REGEX "/main\\.c$")
list(FILTER MPY_CORE_SOURCES EXCLUDE REGEX "thumb_vfp_.*\\.c$")

list(APPEND MPY_CORE_SOURCES
    "${MPY_COMPONENT_DIR}/mpy.c"
    "${MPY_BUILD_DIR}/frozen_content.c"
)

idf_component_register(
    SRCS ${MPY_CORE_SOURCES}
    INCLUDE_DIRS
        "include"
        "${MPY_COMPONENT_DIR}"
        "${MPY_SOURCE_DIR}"
        "${MPY_EMBED_DIR}"
        "${MPY_EMBED_DIR}/py"
        "${MPY_EMBED_DIR}/port"
        "${MPY_EMBED_DIR}/extmod"
        "${MPY_EMBED_DIR}/shared"
        "${MPY_EMBED_DIR}/lib"
        "${MPY_EMBED_DIR}/genhdr"
        "${MPY_BUILD_DIR}"
    PRIV_REQUIRES esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-maybe-uninitialized
)
