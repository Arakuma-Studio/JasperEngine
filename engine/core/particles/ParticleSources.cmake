# The particle system (docs/specs/S20-particles.md): its sources, and the flags that keep the
# SIMD kernels and their scalar fallback bit-identical: no floating-point contraction (an FMA
# rounds once where the scalar code rounds twice). WebAssembly builds them with simd128
# (JASPERENGINE_WASM_SIMD, default ON; OFF builds the scalar kernels only).

set(JASPERENGINE_PARTICLE_SRCS
    core/math/noise/CurlNoiseBatch.cpp
    core/math/noise/CurlNoiseAvx2.cpp
    core/particles/EmitterRuntime.cpp
    core/particles/LegacyParticleConverter.cpp
    core/particles/ParticleChannels.cpp
    core/particles/ParticleCompiler.cpp
    core/particles/ParticleEffectCodec.cpp
    core/particles/ParticleEffectLibrary.cpp
    core/particles/ParticleKernels.cpp
    core/particles/ParticleMeshes.cpp
    core/particles/AnalyticParticles.cpp
    core/particles/ParticleModules.cpp
    core/particles/ParticlePixel.cpp
    core/particles/ParticlePreview.cpp
    core/particles/ParticleRender.cpp
    core/particles/ParticleTrails.cpp
    core/particles/ParticleValues.cpp
    core/particles/ParticleWorld.cpp
)

# the engine's side: the subsystem that runs the scene's particle systems and their drawing
set(JASPERENGINE_PARTICLE_SYSTEM_SRCS
    core/object/ParticleSystem.cpp
    core/subsystem/ParticleSubsystem.cpp
    core/subsystem/RenderSystemParticles.cpp
    core/particles/PixelParticles.cpp
)

option(JASPERENGINE_WASM_SIMD "Build the WebAssembly particle kernels with simd128" ON)

set(JASPERENGINE_PARTICLE_FLAGS "")
if(MSVC)
    # /fp:precise (the default) does not contract
    list(APPEND JASPERENGINE_PARTICLE_FLAGS /fp:precise)
else()
    list(APPEND JASPERENGINE_PARTICLE_FLAGS -ffp-contract=off)
endif()
if(EMSCRIPTEN)
    if(JASPERENGINE_WASM_SIMD)
        list(APPEND JASPERENGINE_PARTICLE_FLAGS -msimd128)
    else()
        set_source_files_properties(${JASPERENGINE_PARTICLE_SRCS} PROPERTIES COMPILE_DEFINITIONS JASPERENGINE_SIMD_DISABLE)
    endif()
endif()
set_source_files_properties(${JASPERENGINE_PARTICLE_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_PARTICLE_FLAGS}")

# the 8-wide curl noise (x86-64): its one file built for AVX2, picked at run time
set(JASPERENGINE_PARTICLE_AVX2_FLAGS "")
if(NOT EMSCRIPTEN AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    if(MSVC)
        set(JASPERENGINE_PARTICLE_AVX2_FLAGS /arch:AVX2)
    else()
        set(JASPERENGINE_PARTICLE_AVX2_FLAGS -mavx2)
    endif()
endif()
set_source_files_properties(core/math/noise/CurlNoiseAvx2.cpp PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_PARTICLE_FLAGS};${JASPERENGINE_PARTICLE_AVX2_FLAGS}")
