# jasperengine-noise-golden (T2 R41, R72, N17): a Release build of only noise, graph and the golden
# table, with no engine, so it links in seconds under any toolchain and a person can run it where this
# repository's other builds do not reach.
cmake_minimum_required(VERSION 3.16)
project(jasperengine-noise-golden CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(ENGINE_CORE "${REPO_ROOT}/engine/core")
option(JASPERENGINE_NOISE_ALL_BACKENDS "Build the Scalar back end beside Native" ON)
option(JASPERENGINE_NOISE_RED_CONTRACT "Red control of cross_check.sh: FMA with contraction on, which must fail" OFF)
option(JASPERENGINE_NOISE_STATIC "Link statically (the Android rows)" OFF)

# Embed the presets, as the editor does (T2 R34)
include(EmbedPresets.cmake)
jasperengine_embed_noise_presets("${REPO_ROOT}/editor/resources/noisegraphs" "${CMAKE_CURRENT_BINARY_DIR}/noise_presets.gen.h")

add_executable(jasperengine-noise-golden
    main.cpp
    ${REPO_ROOT}/engine/tests/core/noise/NoiseGoldenCases.cpp
    ${ENGINE_CORE}/noise/NoiseKernels.cpp
    ${ENGINE_CORE}/noise/NoiseNodes.cpp
    ${ENGINE_CORE}/noise/NoiseCompiler.cpp
    ${ENGINE_CORE}/noise/NoiseRanges.cpp
    ${ENGINE_CORE}/noise/NoiseEval.cpp
    ${ENGINE_CORE}/noise/NoiseCompat.cpp
    ${ENGINE_CORE}/terrain/HeightfieldErosion.cpp
    ${ENGINE_CORE}/graph/Graph.cpp
    ${ENGINE_CORE}/graph/GraphRegistry.cpp
)
target_include_directories(jasperengine-noise-golden PRIVATE
    ${ENGINE_CORE}
    ${REPO_ROOT}/engine/libs/json
    ${REPO_ROOT}/engine/tests/core/noise
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_definitions(jasperengine-noise-golden PRIVATE JASPERENGINE_NOISE_SERIAL JASPERENGINE_GOLDEN_TOOL
    $<$<BOOL:${JASPERENGINE_NOISE_ALL_BACKENDS}>:JASPERENGINE_NOISE_ALL_BACKENDS>)
if(MSVC)
    target_compile_options(jasperengine-noise-golden PRIVATE /fp:precise /utf-8)
elseif(JASPERENGINE_NOISE_RED_CONTRACT)
    target_compile_options(jasperengine-noise-golden PRIVATE -ffp-contract=fast -mfma)
else()
    target_compile_options(jasperengine-noise-golden PRIVATE -ffp-contract=off)
endif()
if(JASPERENGINE_NOISE_STATIC)
    target_link_options(jasperengine-noise-golden PRIVATE -static)
endif()
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(jasperengine-noise-golden PRIVATE Threads::Threads)
endif()
