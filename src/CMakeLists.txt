# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Exclude example files from build
list(FILTER app_sources EXCLUDE REGEX ".*_example\\.c$")

idf_component_register(SRCS ${app_sources})

set(ulp_app_name ulp_main)
set(ulp_s_sources "../ulp/ulp.S")
set(ulp_exp_dep_srcs "*/**")

ulp_embed_binary(${ulp_app_name} "${ulp_s_sources}" "${ulp_exp_dep_srcs}")