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
# Title Art's tests (titleart/) need its module
if(NOT JASPERENGINE_TITLE_ART)
    list(FILTER CORE_TEST_SOURCES EXCLUDE REGEX "/titleart/")
endif()
# 3D titles' tests (titleart/three_d/) need its sub-options (S86)
if(NOT JASPERENGINE_TITLE_ART_3D OR NOT JASPERENGINE_TITLE_ART_MESHER)
    list(FILTER CORE_TEST_SOURCES EXCLUDE REGEX "/titleart/three_d/")
endif()

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
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageRulesTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageScatterTest.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/navigation/NavBakeFoliageTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/navigation/TerrainWorldNavTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/TerrainCollisionRefreshTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/FoliageGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/TerrainHolesGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/VoxelRenderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/script/LuaTerrainMaterialsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shader/VoxelShaderTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageCullTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageLuaTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliagePhysicsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageRulesTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageScatterTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/FoliageSystemCoreTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TreeStoreTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainBatchTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainEditTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainFilterTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainGoldenTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainHolesConsumersTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainHolesTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsDepsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsKernelsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainOpsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainPlacementTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainStoreTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/TerrainSurfaceTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorHumanoidTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorPropertyThreadTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorRetargetTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorStateMachineTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimatorSystemTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/ClipIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/ClipMemoryTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/ProfilerAnimationTest.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/VehicleBench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/physics/VehicleTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/HumanoidMapIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseFileTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseIOTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/pose/PoseMetricsTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/ObjectRenderShaderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/ParticlesGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/RenderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/SpriteSheetRenderGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/render/WaterFeaturesGpuTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/spline/SplineCornerTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/support/AllocationCounter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/util/FunctionSubscribeReentrancyTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/util/PostColorLutTest.cpp)
jasperengine_skip_unity_build(
    REASON "Xlib.h defines None, Bool, Status, Always... as macros, which break every later source of the batch"
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/X11VisibilityTest.cpp)

# The noise suites compile kernels' arithmetic in their own units and compare it with the library's, so
# they take the same flag. The golden cases are shared with tools/noise/golden, and the presets are
# embedded as the tool embeds them (T2 R41).
set(CORE_TEST_NOISE_SOURCES "")
if(JASPERENGINE_NOISE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/noise/golden/EmbedPresets.cmake)
    jasperengine_embed_noise_presets("${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/resources/noisegraphs" "${CMAKE_CURRENT_BINARY_DIR}/noise_presets.gen.h")
    set(CORE_TEST_NOISE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/noise/NoiseGoldenCases.cpp)
    file(GLOB CORE_TEST_NOISE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/noise/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/terrain/HeightfieldErosion*Test.cpp)
    if(MSVC)
        set_source_files_properties(${CORE_TEST_NOISE_FILES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
    else()
        set_source_files_properties(${CORE_TEST_NOISE_FILES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()
    set_property(SOURCE ${CORE_TEST_NOISE_FILES} PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
else()
    list(FILTER CORE_TEST_SOURCES EXCLUDE REGEX "/noise/|/terrain/HeightfieldErosion")
endif()

# S90: the Procedural Material evaluator is the editor's, but its GPU suites need the headless device
# only this tree has (procmat/*GpuTest.cpp). It uses the engine's render layer and nothing of the editor.
set(CORE_TEST_PROCMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/NodeLibrary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProcMatExpander.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralMaterialKind.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/PassPlanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralGpuEvaluator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralCpuEvaluator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralTieredEvaluator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProcMatThumbnails.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/ProceduralDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/SpirvInterpreter.cpp)
jasperengine_skip_unity_build(
    REASON "the editor's own sources: its unity units do not hold them"
    SOURCES ${CORE_TEST_PROCMAT_SOURCES})
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../../../editor/procmat/SpirvInterpreter.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/shadercompiler/libs/glslang)

jasperengine_add_test_target(jasperengine-core-tests
    SOURCES ${CORE_TEST_SOURCES} ${CORE_TEST_ALLOCATION_COUNTER} ${CORE_TEST_NOISE_SOURCES} ${CORE_TEST_PROCMAT_SOURCES}
        # the animation clip tests' asset root (a temporary directory around each of them)
        ${CMAKE_CURRENT_SOURCE_DIR}/animation/AnimTestEnvironment.cpp
        # the vertex animation tests' asset root, and their asset and GLB fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/vat/VatTestEnvironment.cpp
    # (platform/android: the soft keyboard's text conversions, plain C++, AndroidTextInputTest)
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support ${CMAKE_CURRENT_SOURCE_DIR}/noise ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/android
        # WebKeyMap.h, the web platform's key map (plain C++)
        ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/emscripten
    DEFINITIONS
        # the repository root: the fixture projects of tools/fixtures (pose and clip tests' models)
        JASPER_TEST_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    # the KTX2 tests write Zstandard supercompressed files (games link the decoder only)
    LINK zstd
    DEFINITIONS
        JASPER_TEST_CRASH_TEST_BINARY="$<TARGET_FILE:jasperengine-crash-test>"
)
add_dependencies(jasperengine-core-tests jasperengine-crash-test)
# the navigation tests call into Recast/Detour directly (the vendored patches, the tile builds'
# rcContext: docs/specs/S74-navigation-fixes.md AC1-AC3, AC7)
if(TARGET recastnavigation)
    target_link_libraries(jasperengine-core-tests PRIVATE recastnavigation)
endif()
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
# and of waterGerstnerWave with waterGerstner (GERSTNER), from the first's signature to the
# second's closing brace
string(FIND "${WATER_GLSL_SOURCE}" "void waterGerstnerWave(" WATER_GERSTNER_BEGIN)
string(FIND "${WATER_GLSL_SOURCE}" "vec3 waterGerstner(" WATER_GERSTNER_LAST)
if(WATER_GERSTNER_BEGIN EQUAL -1 OR WATER_GERSTNER_LAST EQUAL -1)
    message(FATAL_ERROR "library/water.glsl has no waterGerstnerWave / waterGerstner (WaterWavesTest, engine/core/util/WaterWaves.h)")
endif()
string(SUBSTRING "${WATER_GLSL_SOURCE}" ${WATER_GERSTNER_LAST} -1 WATER_GERSTNER_REST)
string(FIND "${WATER_GERSTNER_REST}" "\n}\n" WATER_GERSTNER_REST_END)
math(EXPR WATER_GERSTNER_LENGTH "${WATER_GERSTNER_LAST} - ${WATER_GERSTNER_BEGIN} + ${WATER_GERSTNER_REST_END} + 2")
string(SUBSTRING "${WATER_GLSL_SOURCE}" ${WATER_GERSTNER_BEGIN} ${WATER_GERSTNER_LENGTH} WATER_GERSTNER_FUNCTIONS)
string(SHA256 WATER_GERSTNER_GLSL_SHA256 "${WATER_GERSTNER_FUNCTIONS}")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/util/WaterWavesTest.cpp PROPERTIES
    COMPILE_DEFINITIONS "WATER_WAVES_GLSL_SHA256=\"${WATER_WAVES_GLSL_SHA256}\";WATER_GERSTNER_GLSL_SHA256=\"${WATER_GERSTNER_GLSL_SHA256}\"")
# a unit of its own: it redefines JASPERENGINE_PROFILER to 0 before the profiling macros
jasperengine_skip_unity_build(
    REASON "ProfilerCompileOutTest compiles the profiling macros with the profiler off"
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/profiler/ProfilerCompileOutTest.cpp)

# PixelColorDeterminism.UnoptimisedBuildMatches (S43 AC5): the pixel-art colour maths built a
# second time without optimisation, in a namespace of its own, against the engine's optimised copy
set(PIXELART_O0_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/pixelart/PixelColorO0.cpp)
target_sources(jasperengine-core-tests PRIVATE ${PIXELART_O0_SOURCE})
if(MSVC)
    set_source_files_properties(${PIXELART_O0_SOURCE} PROPERTIES COMPILE_OPTIONS "/Od;/fp:precise")
else()
    set_source_files_properties(${PIXELART_O0_SOURCE} PROPERTIES COMPILE_OPTIONS "-O0;-ffp-contract=off")
endif()
jasperengine_skip_unity_build(
    REASON "PixelColorO0.cpp compiles engine/core/pixelart/PixelColor.cpp again in a renamed namespace"
    SOURCES ${PIXELART_O0_SOURCE})

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
target_compile_definitions(je-bench-anim PRIVATE JASPER_TEST_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../..")
# je-bench-audio: the loudness budgets (S38 R16-R17)
add_executable(je-bench-audio ${CMAKE_CURRENT_SOURCE_DIR}/bench/AudioMeterBench.cpp)
# je-bench-foliage: foliage's budgets (T5 R41, R42, R45, docs/perf/foliage.md)
add_executable(je-bench-foliage ${CMAKE_CURRENT_SOURCE_DIR}/bench/FoliageBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
# je-bench-terrain: the terrain engine's budgets (T1 R47, R48, R50-R52, docs/perf/terrain.md)
add_executable(je-bench-terrain ${CMAKE_CURRENT_SOURCE_DIR}/bench/TerrainBench.cpp)
set(CORE_BENCH_TARGETS je-bench-jobs je-bench-memory je-bench-picking je-bench-diag je-bench-particles je-bench-nav je-bench-splines je-bench-anim je-bench-audio je-bench-foliage je-bench-terrain)
# je-bench-voxel: the Voxel Volume budgets (T6 R55, R56)
if(JASPERENGINE_VOXEL)
    add_executable(je-bench-voxel ${CMAKE_CURRENT_SOURCE_DIR}/bench/VoxelBench.cpp ${CORE_TEST_ALLOCATION_COUNTER})
    list(APPEND CORE_BENCH_TARGETS je-bench-voxel)
endif()
# je-bench-noise: the noise budgets (T2 R46-R49, docs/perf/noise.md)
if(JASPERENGINE_NOISE)
    add_executable(je-bench-noise ${CMAKE_CURRENT_SOURCE_DIR}/bench/NoiseBench.cpp)
    target_compile_definitions(je-bench-noise PRIVATE JASPER_TEST_REPO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../..")
    list(APPEND CORE_BENCH_TARGETS je-bench-noise)
endif()
foreach(target ${CORE_BENCH_TARGETS})
    target_link_libraries(${target} PRIVATE jasperengine Threads::Threads)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    jasperengine_apply_cxx_standard(${target})
    jasperengine_target_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "tests")
endforeach()

# je-bench-titlemesh: the 3D title geometry budgets (S86 R46, AC24, docs/perf/title3d.md); it reads
# the title art test fonts through the tests' asset resolver
if(JASPERENGINE_TITLE_ART_3D AND JASPERENGINE_TITLE_ART_MESHER)
    add_executable(je-bench-titlemesh ${CMAKE_CURRENT_SOURCE_DIR}/bench/TitleMeshBench.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../support/TestPaths.cpp)
    target_link_libraries(je-bench-titlemesh PRIVATE jasperengine Threads::Threads)
    target_include_directories(je-bench-titlemesh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support ${CMAKE_CURRENT_SOURCE_DIR}
                                                          ${CMAKE_CURRENT_SOURCE_DIR}/../support)
    target_compile_definitions(je-bench-titlemesh PRIVATE JE_TITLE_BENCH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    jasperengine_apply_cxx_standard(je-bench-titlemesh)
    jasperengine_target_warnings(je-bench-titlemesh)
    set_target_properties(je-bench-titlemesh PROPERTIES FOLDER "tests")
endif()

foreach(target jasperengine-core-tests je-bench-jobs je-bench-memory je-bench-particles je-bench-nav je-bench-splines je-bench-anim je-bench-voxel)
    if(CORE_TEST_COUNTS_ALLOCATIONS)
        target_compile_definitions(${target} PRIVATE JE_TEST_COUNTS_ALLOCATIONS)
        target_link_options(${target} PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc" "-Wl,--wrap=realloc" "-rdynamic")
    endif()
endforeach()
# The spline tests build their inputs (grounds, stations) and expected values with the spline
# core's floating-point flags (core/spline/SplineSources.cmake, S18 R16): with contraction, as
# clang does on arm64, the bowl ground's heights round differently from x86-64's, and the bit-exact
# checks (SplineFillMesherTests seams and zero-area triangles) compared other inputs
# The voxel tests compute inputs and expected hashes in their own units: no contraction there either (T6 R28)
file(GLOB CORE_TEST_VOXEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/voxel/*.cpp)
if(CORE_TEST_VOXEL_SOURCES AND JASPERENGINE_VOXEL_FLAGS)
    set_source_files_properties(${CORE_TEST_VOXEL_SOURCES} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_VOXEL_FLAGS}")
endif()
file(GLOB CORE_TEST_SPLINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spline/*.cpp)
if(CORE_TEST_SPLINE_SOURCES AND JASPERENGINE_SPLINE_FLAGS)
    set_source_files_properties(${CORE_TEST_SPLINE_SOURCES} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_SPLINE_FLAGS}")
endif()
