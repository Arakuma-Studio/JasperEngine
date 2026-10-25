# A project with one C++ library at the engine standard and one at C++17, for
# jasperengine_check_cxx_standards() (engine/cmake/JasperEngineToolchain.cmake). Configuring it
# must fail and name only legacy17, and pass with -DJASPERENGINE_CXX_STANDARD_EXCEPTIONS=legacy17.
# run.cmake configures it both ways.
cmake_minimum_required(VERSION 3.20)
project(jasperengine-cxx-standard-check CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/cmake/JasperEngineToolchain.cmake)
set(JASPERENGINE_CXX_STANDARD 20)

add_library(current STATIC current.cpp)
jasperengine_apply_cxx_standard(current)

# a vendored library on a standard of its own: what the check is there to find
add_library(legacy17 STATIC legacy17.cpp)
set_target_properties(legacy17 PROPERTIES CXX_STANDARD 17)

# C only: not a C++ target, so not checked
enable_language(C)
add_library(plainc STATIC plain.c)

jasperengine_check_cxx_standards()
