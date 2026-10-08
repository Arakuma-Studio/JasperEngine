# Embeds the user-agent files of the HTML UI compatibility layer (this directory: the
# standard-CSS user-agent stylesheet and its internal SVGs) into a C++ header:
#
#   cmake -DUA_DIR=<engine/core/htmlui/compat/ua> -DOUTPUT=<htmlui_ua.h> -P embed_ua.cmake
#
# The editor's Apple export writes the same header from C++
# (htmlui::compat::UserAgentStyle::buildEmbeddedHeader); keep the two formats in step.

if(NOT UA_DIR OR NOT OUTPUT)
    message(FATAL_ERROR "embed_ua.cmake needs UA_DIR and OUTPUT")
endif()

file(GLOB ua_files RELATIVE "${UA_DIR}" "${UA_DIR}/*.css" "${UA_DIR}/*.svg")
list(SORT ua_files)

set(content "// Generated from engine/core/htmlui/compat/ua. Do not edit.\n\n")
string(APPEND content "#pragma once\n\n#include <cstddef>\n\n")
string(APPEND content "namespace jasperengine { namespace htmlui { namespace compat { namespace embedded {\n\n")
string(APPEND content "struct UaFile { const char* name; const unsigned char* data; size_t size; };\n\n")

set(index 0)
set(table "")
foreach(ua_file IN LISTS ua_files)
    file(READ "${UA_DIR}/${ua_file}" ua_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ua_bytes "${ua_hex}")
    # CRLF -> LF, so a CRLF checkout embeds the same bytes
    string(REPLACE "0x0d,0x0a," "0x0a," ua_bytes "${ua_bytes}")
    string(LENGTH "${ua_bytes}" bytes_length)
    math(EXPR byte_count "${bytes_length} / 5")
    string(APPEND content "static const unsigned char uaFile${index}[] = {${ua_bytes}0x00};\n\n")
    string(APPEND table "    { \"${ua_file}\", uaFile${index}, ${byte_count} },\n")
    math(EXPR index "${index} + 1")
endforeach()

if(index EQUAL 0)
    string(APPEND content "static const UaFile uaFiles[1] = { { nullptr, nullptr, 0 } };\n")
else()
    string(APPEND content "static const UaFile uaFiles[] = {\n${table}};\n")
endif()
string(APPEND content "static const size_t uaFileCount = ${index};\n\n} } } }\n")

# Rewritten only when the files changed, so an unchanged directory rebuilds nothing
file(WRITE "${OUTPUT}.tmp" "${content}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
