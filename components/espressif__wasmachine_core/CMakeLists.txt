
set(srcs  "src/wm_wamr.c")
set(include_dir "include")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dir}
                       REQUIRES "wasm-micro-runtime")
