# The shader compiler the editor and the engine runtime share: GLSL (engine/shaderlib)
# to SPIR-V with glslang, then SPIR-V to the backend's language with SPIRV-Cross.
#
# Built from an explicit source list instead of the vendored projects' own CMake
# files, so it configures with the CMake every engine target uses (Android Studio
# ships 3.22; glslang's build wants 3.27) and carries only what the engine needs:
# no glslang HLSL front end, no SPIRV-Tools optimizer, no tools.
#
#   SHADERCOMPILER_HLSL  SPIRV-Cross HLSL back end (Direct3D 11)
#   SHADERCOMPILER_MSL   SPIRV-Cross MSL back end (Metal)
#
# The GLSL back end is always in: OpenGL and OpenGL ES use it, and the reflection
# step reads every language's resources through it.

option(SHADERCOMPILER_HLSL "Include the SPIRV-Cross HLSL back end" ON)
option(SHADERCOMPILER_MSL "Include the SPIRV-Cross MSL back end" ON)
option(SHADERCOMPILER_OPTIMIZE_SIZE "Optimize the compiler for size instead of speed" OFF)

set(SHADERCOMPILER_GLSLANG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/glslang)
set(SHADERCOMPILER_SPIRV_CROSS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/spirv-cross)

set(SHADERCOMPILER_SOURCES
    src/args.cc
    src/input.cc
    src/glslang.cc
    src/spirvcross.cc
)

set(SHADERCOMPILER_GLSLANG_SOURCES
    glslang/GenericCodeGen/CodeGen.cpp
    glslang/GenericCodeGen/Link.cpp
    glslang/MachineIndependent/glslang_tab.cpp
    glslang/MachineIndependent/attribute.cpp
    glslang/MachineIndependent/Constant.cpp
    glslang/MachineIndependent/iomapper.cpp
    glslang/MachineIndependent/InfoSink.cpp
    glslang/MachineIndependent/Initialize.cpp
    glslang/MachineIndependent/IntermTraverse.cpp
    glslang/MachineIndependent/Intermediate.cpp
    glslang/MachineIndependent/ParseContextBase.cpp
    glslang/MachineIndependent/ParseHelper.cpp
    glslang/MachineIndependent/PoolAlloc.cpp
    glslang/MachineIndependent/RemoveTree.cpp
    glslang/MachineIndependent/Scan.cpp
    glslang/MachineIndependent/ShaderLang.cpp
    glslang/MachineIndependent/SpirvIntrinsics.cpp
    glslang/MachineIndependent/SymbolTable.cpp
    glslang/MachineIndependent/Versions.cpp
    glslang/MachineIndependent/intermOut.cpp
    glslang/MachineIndependent/limits.cpp
    glslang/MachineIndependent/linkValidate.cpp
    glslang/MachineIndependent/parseConst.cpp
    glslang/MachineIndependent/reflection.cpp
    glslang/MachineIndependent/preprocessor/Pp.cpp
    glslang/MachineIndependent/preprocessor/PpAtom.cpp
    glslang/MachineIndependent/preprocessor/PpContext.cpp
    glslang/MachineIndependent/preprocessor/PpScanner.cpp
    glslang/MachineIndependent/preprocessor/PpTokens.cpp
    glslang/MachineIndependent/propagateNoContraction.cpp
    SPIRV/GlslangToSpv.cpp
    SPIRV/InReadableOrder.cpp
    SPIRV/Logger.cpp
    SPIRV/SpvBuilder.cpp
    SPIRV/SpvPostProcess.cpp
    SPIRV/doc.cpp
    SPIRV/SpvTools.cpp
    SPIRV/disassemble.cpp
)
if(WIN32)
    list(APPEND SHADERCOMPILER_GLSLANG_SOURCES glslang/OSDependent/Windows/ossource.cpp)
else()
    list(APPEND SHADERCOMPILER_GLSLANG_SOURCES glslang/OSDependent/Unix/ossource.cpp)
endif()
list(TRANSFORM SHADERCOMPILER_GLSLANG_SOURCES PREPEND ${SHADERCOMPILER_GLSLANG_DIR}/)

set(SHADERCOMPILER_SPIRV_CROSS_SOURCES
    spirv_cross.cpp
    spirv_parser.cpp
    spirv_cross_parsed_ir.cpp
    spirv_cfg.cpp
    spirv_glsl.cpp
)
if(SHADERCOMPILER_HLSL)
    list(APPEND SHADERCOMPILER_SPIRV_CROSS_SOURCES spirv_hlsl.cpp)
endif()
if(SHADERCOMPILER_MSL)
    list(APPEND SHADERCOMPILER_SPIRV_CROSS_SOURCES spirv_msl.cpp)
endif()
list(TRANSFORM SHADERCOMPILER_SPIRV_CROSS_SOURCES PREPEND ${SHADERCOMPILER_SPIRV_CROSS_DIR}/)

add_library(
    shadercompiler

    STATIC

    ${SHADERCOMPILER_SOURCES}
    ${SHADERCOMPILER_GLSLANG_SOURCES}
    ${SHADERCOMPILER_SPIRV_CROSS_SOURCES}
)

target_include_directories(shadercompiler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE
        # glslang/build_info.h, generated once from glslang's CHANGES.md
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SHADERCOMPILER_GLSLANG_DIR}
        ${SHADERCOMPILER_SPIRV_CROSS_DIR}
)

target_compile_definitions(shadercompiler
    PRIVATE
        ENABLE_OPT=0
        $<$<BOOL:${SHADERCOMPILER_HLSL}>:SHADERCOMPILER_HLSL>
        $<$<BOOL:${SHADERCOMPILER_MSL}>:SHADERCOMPILER_MSL>
)

find_package(Threads REQUIRED)
target_link_libraries(shadercompiler PRIVATE Threads::Threads)

set_target_properties(shadercompiler PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Third-party code: its warnings are not ours to fix
if(MSVC)
    target_compile_options(shadercompiler PRIVATE /w)
else()
    target_compile_options(shadercompiler PRIVATE -w)
endif()

# A game compiles a few variants now and then: optimized for size, the compiler costs a
# game about 40% less (a compile takes a few milliseconds longer)
if(SHADERCOMPILER_OPTIMIZE_SIZE)
    if(MSVC)
        target_compile_options(shadercompiler PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O1>)
    else()
        target_compile_options(shadercompiler PRIVATE $<$<NOT:$<CONFIG:Debug>>:-Os>)
    endif()
endif()
