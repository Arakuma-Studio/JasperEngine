# Embeds the engine shader library (engine/shaderlib) into a C++ header, the sources
# the shader compiler builds variants from at runtime:
#
#   cmake -DSHADERLIB_DIR=<engine/shaderlib> -DOUTPUT=<shaderlib.h> -P embed_shaderlib.cmake
#
# The editor's Apple export writes the same header from C++
# (ShaderHeaderBuilder::buildShaderLibHeader); keep the two formats in step.

if(NOT SHADERLIB_DIR OR NOT OUTPUT)
    message(FATAL_ERROR "embed_shaderlib.cmake needs SHADERLIB_DIR and OUTPUT")
endif()

file(GLOB_RECURSE shaderlib_files RELATIVE "${SHADERLIB_DIR}"
    "${SHADERLIB_DIR}/*.glsl" "${SHADERLIB_DIR}/*.vert" "${SHADERLIB_DIR}/*.frag")
list(SORT shaderlib_files)

set(content "// Generated from engine/shaderlib. Do not edit.\n\n")
string(APPEND content "#pragma once\n\n#include <cstddef>\n\n")
string(APPEND content "namespace jasperengine { namespace shaderlib {\n\n")
string(APPEND content "struct File { const char* name; const char* source; };\n\n")

set(index 0)
set(table "")
foreach(shader_file IN LISTS shaderlib_files)
    file(TO_CMAKE_PATH "${shader_file}" shader_name)
    file(READ "${SHADERLIB_DIR}/${shader_file}" shader_source)
    # One string literal piece per line keeps every piece within compiler limits;
    # carriage returns are dropped so a CRLF checkout embeds the same text
    string(REPLACE "\r" "" shader_source "${shader_source}")
    string(REPLACE "\\" "\\\\" shader_source "${shader_source}")
    string(REPLACE "\"" "\\\"" shader_source "${shader_source}")
    string(REPLACE "\n" "\\n\"\n    \"" shader_source "${shader_source}")
    string(APPEND content "static const char source${index}[] =\n    \"${shader_source}\";\n\n")
    string(APPEND table "    { \"${shader_name}\", source${index} },\n")
    math(EXPR index "${index} + 1")
endforeach()

if(index EQUAL 0)
    string(APPEND content "static const File files[1] = { { nullptr, nullptr } };\n")
else()
    string(APPEND content "static const File files[] = {\n${table}};\n")
endif()
string(APPEND content "static const size_t fileCount = ${index};\n\n} }\n")

# Rewritten only when the library changed, so an unchanged one rebuilds nothing
file(WRITE "${OUTPUT}.tmp" "${content}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
