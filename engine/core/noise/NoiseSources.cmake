# engine/core/noise (T2 R38, R42): the noise library and Noise Graphs, the JASPERENGINE_NOISE module,
# with the flags that keep every target's bits the same: no floating-point contraction, and on the
# web simd128 unless JASPERENGINE_WASM_SIMD=OFF (the particle flags, ParticleSources.cmake). Kernel
# headers are included only from these sources. terrain/HeightfieldErosion.cpp is in TerrainSources.cmake,
# which gives it no contraction; it uses only the one-lane back end, and is empty without the module.

set(JASPERENGINE_NOISE_SRCS
    core/noise/NoiseKernels.cpp
    core/noise/NoiseNodes.cpp
    core/noise/NoiseCompiler.cpp
    core/noise/NoiseRanges.cpp
    core/noise/NoiseEval.cpp
)

# Outside the module: the brushes and fill_noise call it in every player (N13)
set(JASPERENGINE_NOISE_COMPAT_SRCS
    core/noise/NoiseCompat.cpp
)

set(JASPERENGINE_NOISE_FLAGS "")
if(MSVC)
    list(APPEND JASPERENGINE_NOISE_FLAGS /fp:precise)
else()
    list(APPEND JASPERENGINE_NOISE_FLAGS -ffp-contract=off)
endif()
set(JASPERENGINE_NOISE_COMPAT_FLAGS ${JASPERENGINE_NOISE_FLAGS})
if(EMSCRIPTEN)
    if(JASPERENGINE_WASM_SIMD)
        list(APPEND JASPERENGINE_NOISE_FLAGS -msimd128)
    else()
        set_property(SOURCE ${JASPERENGINE_NOISE_SRCS} APPEND PROPERTY COMPILE_DEFINITIONS JASPERENGINE_SIMD_DISABLE)
    endif()
endif()
# A player carries Native alone (R51); the editor and the tests compare it with Scalar
if(JASPERENGINE_NOISE_ALL_BACKENDS OR JASPERENGINE_TESTS)
    set_property(SOURCE ${JASPERENGINE_NOISE_SRCS} APPEND PROPERTY COMPILE_DEFINITIONS JASPERENGINE_NOISE_ALL_BACKENDS)
endif()
set_source_files_properties(${JASPERENGINE_NOISE_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_NOISE_FLAGS}")
set_source_files_properties(${JASPERENGINE_NOISE_COMPAT_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_NOISE_COMPAT_FLAGS}")
