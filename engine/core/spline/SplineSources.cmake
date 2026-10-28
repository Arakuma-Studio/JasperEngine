# Splines (docs/specs/S18-splines.md): the CPU spline core, the swept geometry, the fill meshers
# and the layouts along a spline. They depend on core/math and the STL only (S18 D1). Built
# without floating-point contraction (an FMA rounds once where a separate multiply and add round
# twice) so the generated vertices and poses are bit-identical across desktop compilers (S18 R16).

file(GLOB JASPERENGINE_SPLINE_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(JASPERENGINE_SPLINE_FLAGS "")
if(MSVC)
    # /fp:precise (the default) does not contract
    list(APPEND JASPERENGINE_SPLINE_FLAGS /fp:precise)
else()
    # -fno-math-errno: sqrt compiles to the instruction alone (no errno branch); it changes no
    # result, so the output stays bit-identical
    list(APPEND JASPERENGINE_SPLINE_FLAGS -ffp-contract=off -fno-math-errno)
endif()
if(JASPERENGINE_SPLINE_SRCS)
    set_source_files_properties(${JASPERENGINE_SPLINE_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_SPLINE_FLAGS}")
endif()
