# jasperengine-core-tests: unit tests of the engine (engine/core, engine/renders) against the
# static engine, built by the standalone engine tree with -DJASPERENGINE_TESTS=ON
# (tools/build.sh unit-tests). docs/testing.md has the layout and naming rules: <area>/ mirrors
# engine/core/<area>/, files are <Subject>Test.cpp, fixtures/ and golden/ hold test data,
# support/ holds headers shared by this binary's tests.
# CPU suites build and run on Linux, Windows and macOS; *Gpu suites run on Linux (Xvfb) and
# macOS (Metal).
#
# je-bench-jobs and je-bench-memory are the job system and allocator benchmarks (S06 R23-R26),
# je-bench-diag the crash and hang diagnostics' (S28 R26), je-bench-picking the triangle picking
# budgets (S26 R28-R31, docs/scene-view-picking.md), je-bench-particles the particle system's
# (S20 R20-R26, docs/perf/particles.md), je-bench-nav navigation's (S19 R25-R30,
# docs/perf/navigation.md), je-bench-splines the splines' (S18 R18-R21, docs/perf/splines.md),
# je-bench-anim the Animator's (S21 R19-R20, docs/perf/animator.md), je-bench-audio the loudness
# meter's and the limiter's (S38 R16-R17)
# tools/perf/check_bench.py gates on, from their JSON output (docs/jobs-memory.md).

file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)

# The allocation counter of the zero-allocation tests and the benches. It counts at malloc,
# wrapped by GNU ld (it replaces the plain operator new with malloc), so it counts on Linux;
# elsewhere those tests skip.
set(CORE_TEST_ALLOCATION_COUNTER ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp)
set(CORE_TEST_COUNTS_ALLOCATIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CORE_TEST_COUNTS_ALLOCATIONS ON)
endif()

# the process the crash-diagnostics tests start (engine/tests/crash/CrashTestMain.cpp): it
# installs the crash handler on a report root of the test's own, then crashes, hangs or reports
find_package(Threads REQUIRED)
add_executable(jasperengine-crash-test
    ${CMAKE_CURRENT_SOURCE_DIR}/../crash/CrashTestMain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../crash/SystemSignalHeader.c)
target_link_libraries(jasperengine-crash-test PRIVATE jasperengine Threads::Threads)
jasperengine_apply_cxx_standard(jasperengine-crash-test)
jasperengine_target_warnings(jasperengine-crash-test)
set_target_properties(jasperengine-crash-test PROPERTIES FOLDER "tests" ENABLE_EXPORTS ON)
if(WIN32)
    jasperengine_add_windows_manifest(jasperengine-crash-test)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(JASPER_CRASH_TEST_FLAGS "")
if(JASPERENGINE_SANITIZE_ADDRESS)
    list(APPEND JASPER_CRASH_TEST_FLAGS --expect-asan)
endif()
add_test(NAME native-crash-reports COMMAND ${Python3_EXECUTABLE} -I
    ${CMAKE_CURRENT_SOURCE_DIR}/../crash/test_crash.py $<TARGET_FILE:jasperengine-crash-test>
    --symbolicate ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/crash/symbolicate.py ${JASPER_CRASH_TEST_FLAGS})
set_tests_properties(native-crash-reports PROPERTIES TIMEOUT 180)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT JASPERENGINE_UNITY_BUILD)
    add_test(NAME posix-crash-signal-safety COMMAND ${Python3_EXECUTABLE} -I
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/crash/check_posix_symbols.py
        ${CMAKE_BINARY_DIR}/CMakeFiles/jasperengine.dir/core/diag/CrashHandlerPosix.cpp.o)
endif()

# The terrain goldens hash what TerrainGoldenSequences.h computes in the test's own unit: no
# contraction there either, or arm64 would seed its maps differently (docs/specs/T1-terrain-authoring.md R8)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainGoldenTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainStoreTest.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

# Unity build: these tests keep file-local fixtures and helpers (TempDir, Marker, makeScene, ...)
# named as in another test source (docs/specs/S33-build-speed.md M8)
jasperengine_skip_unity_build(
    REASON "file-local fixtures or helpers named as in another test source"
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorBlendTreeTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainBatchTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainEditTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainFilterTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainGoldenTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainHolesTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsDepsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsKernelsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainPlacementTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainStoreTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainSurfaceTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorHumanoidTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorRetargetTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorStateMachineTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorSystemTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/ClipIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/HumanoidModelTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/KeyframeSamplingTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/SubClipPoolTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ecs/SignatureTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/input/InputFrameLoopGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/io/JPakTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleBudgetTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleCollisionTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleDeterminismTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleEffectCodecTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleRenderTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleSeekTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleSubEmittersTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleUpstreamBehaviourTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/particles/ParticleUpstreamModulesTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/CharacterController2DTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/CharacterControllerBenchTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/CharacterControllerTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/HumanoidMapIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseFileTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseMetricsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/ObjectRenderShaderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/ParticlesGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/RenderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/WaterFeaturesGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spline/SplineCornerTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/util/FunctionSubscribeReentrancyTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/util/PostColorLutTest.cpp)

jasperengine_add_test_target(jasperengine-core-tests
    SOURCES ${CORE_TEST_SOURCES} ${CORE_TEST_ALLOCATION_COUNTER}
        # the animation clip tests' asset root (a temporary directory around each of them)
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimTestEnvironment.cpp
        # the vertex animation tests' asset root, and their asset and GLB fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/vat/VatTestEnvironment.cpp
    # (platform/android: the soft keyboard's text conversions, plain C++, AndroidTextInputTest)
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support
        ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/android
    DEFINITIONS
        # the repository root: the fixture projects of tools/fixtures (pose and clip tests' models)
        JASPER_TEST_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    # the KTX2 tests write Zstandard supercompressed files (games link the decoder only)
    LINK zstd
    DEFINITIONS
        JASPER_TEST_CRASH_TEST_BINARY="$<TARGET_FILE:jasperengine-crash-test>"
)
add_dependencies(jasperengine-core-tests jasperengine-crash-test)
# tools/crash/symbolicate.py names the crash-test binary's frames (ctest; docs/testing.md)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND NOT WIN32)
    add_test(NAME crash.symbolicate
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/crash/test_symbolicate.py
                     $<TARGET_FILE:jasperengine-crash-test>)
    set_tests_properties(crash.symbolicate PROPERTIES LABELS unit)
endif()

# HexTilingTest compiles the hex lattice block of the shader library as C++: the lines between
# HEX_LATTICE_BEGIN and HEX_LATTICE_END of includes/hex_tiling.glsl, extracted at configure time
# (a change of the shader configures again)
set(HEX_TILING_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/../../shaderlib/includes/hex_tiling.glsl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HEX_TILING_GLSL})
file(READ ${HEX_TILING_GLSL} HEX_TILING_SOURCE)
string(FIND "${HEX_TILING_SOURCE}" "// HEX_LATTICE_BEGIN" HEX_LATTICE_BEGIN)
string(FIND "${HEX_TILING_SOURCE}" "// HEX_LATTICE_END" HEX_LATTICE_END)
if(HEX_LATTICE_BEGIN EQUAL -1 OR HEX_LATTICE_END EQUAL -1)
    message(FATAL_ERROR "includes/hex_tiling.glsl lost its HEX_LATTICE_BEGIN/END markers (HexTilingTest)")
endif()
math(EXPR HEX_LATTICE_LENGTH "${HEX_LATTICE_END} - ${HEX_LATTICE_BEGIN}")
string(SUBSTRING "${HEX_TILING_SOURCE}" ${HEX_LATTICE_BEGIN} ${HEX_LATTICE_LENGTH} HEX_LATTICE_BLOCK)
set(HEX_LATTICE_INL ${CMAKE_CURRENT_BINARY_DIR}/generated/hex_lattice_block.inl)
set(HEX_LATTICE_OLD "")
if(EXISTS ${HEX_LATTICE_INL})
    file(READ ${HEX_LATTICE_INL} HEX_LATTICE_OLD)
endif()
if(NOT HEX_LATTICE_OLD STREQUAL HEX_LATTICE_BLOCK)
    file(WRITE ${HEX_LATTICE_INL} "${HEX_LATTICE_BLOCK}")
endif()
target_include_directories(jasperengine-core-tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# WaterWavesTest.GlslTwinPinned: the SHA-256 of waterWaves in library/water.glsl (from its
# signature to its closing brace, line ends as \n), whose C++ twin is engine/core/util/WaterWaves.h
set(WATER_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/../../shaderlib/library/water.glsl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WATER_GLSL})
file(READ ${WATER_GLSL} WATER_GLSL_SOURCE)
string(REPLACE "\r" "" WATER_GLSL_SOURCE "${WATER_GLSL_SOURCE}")
string(FIND "${WATER_GLSL_SOURCE}" "vec3 waterWaves(" WATER_WAVES_BEGIN)
if(WATER_WAVES_BEGIN EQUAL -1)
    message(FATAL_ERROR "library/water.glsl has no waterWaves (WaterWavesTest, engine/core/util/WaterWaves.h)")
endif()
string(SUBSTRING "${WATER_GLSL_SOURCE}" ${WATER_WAVES_BEGIN} -1 WATER_WAVES_REST)
string(FIND "${WATER_WAVES_REST}" "\n}\n" WATER_WAVES_END)
math(EXPR WATER_WAVES_LENGTH "${WATER_WAVES_END} + 2")
string(SUBSTRING "${WATER_WAVES_REST}" 0 ${WATER_WAVES_LENGTH} WATER_WAVES_FUNCTION)
string(SHA256 WATER_WAVES_GLSL_SHA256 "${WATER_WAVES_FUNCTION}")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/util/WaterWavesTest.cpp PROPERTIES
    COMPILE_DEFINITIONS "WATER_WAVES_GLSL_SHA256=\"${WATER_WAVES_GLSL_SHA256}\"")
# a unit of its own: it redefines JASPERENGINE_PROFILER to 0 before the profiling macros
jasperengine_skip_unity_build(
    REASON "ProfilerCompileOutTest compiles the profiling macros with the profiler off"
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/profiler/ProfilerCompileOutTest.cpp)

# the logger's backtraces name the test functions (LoggerTest.ErrorCarriesBacktrace): dladdr
# sees the executable's symbols
set_target_properties(jasperengine-core-tests PROPERTIES ENABLE_EXPORTS ON)

find_package(Threads REQUIRED)
add_executable(je-bench-jobs ${CMAKE_CURRENT_SOURCE_DIR}/bench/JobsBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-memory ${CMAKE_CURRENT_SOURCE_DIR}/bench/MemoryBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-picking ${CMAKE_CURRENT_SOURCE_DIR}/bench/PickingBench.cpp)
add_executable(je-bench-diag ${CMAKE_CURRENT_SOURCE_DIR}/bench/DiagBench.cpp)
add_executable(je-bench-particles ${CMAKE_CURRENT_SOURCE_DIR}/bench/ParticlesBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-nav ${CMAKE_CURRENT_SOURCE_DIR}/bench/NavBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
# je-bench-splines: the spline budgets (S18 R18-R21, docs/perf/splines.md)
add_executable(je-bench-splines ${CMAKE_CURRENT_SOURCE_DIR}/bench/SplineBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
add_executable(je-bench-anim ${CMAKE_CURRENT_SOURCE_DIR}/bench/AnimBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
# je-bench-audio: the loudness budgets (S38 R16-R17)
add_executable(je-bench-audio ${CMAKE_CURRENT_SOURCE_DIR}/bench/AudioMeterBench.cpp)
foreach(target je-bench-jobs je-bench-memory je-bench-picking je-bench-diag je-bench-particles je-bench-nav je-bench-splines je-bench-anim je-bench-audio)
    target_link_libraries(${target} PRIVATE jasperengine Threads::Threads)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    jasperengine_apply_cxx_standard(${target})
    jasperengine_target_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "tests")
endforeach()

foreach(target jasperengine-core-tests je-bench-jobs je-bench-memory je-bench-particles je-bench-nav je-bench-splines je-bench-anim)
    if(CORE_TEST_COUNTS_ALLOCATIONS)
        target_compile_definitions(${target} PRIVATE JE_TEST_COUNTS_ALLOCATIONS)
        target_link_options(${target} PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")
    endif()
endforeach()
# The spline tests build their inputs (grounds, stations) and expected values with the spline
# core's floating-point flags (core/spline/SplineSources.cmake, S18 R16): with contraction, as
# clang does on arm64, the bowl ground's heights round differently from x86-64's, and the bit-exact
# checks (SplineFillMesherTests seams and zero-area triangles) compared other inputs
file(GLOB CORE_TEST_SPLINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spline/*.cpp)
if(CORE_TEST_SPLINE_SOURCES AND JASPERENGINE_SPLINE_FLAGS)
    set_source_files_properties(${CORE_TEST_SPLINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_SPLINE_FLAGS}")
endif()
