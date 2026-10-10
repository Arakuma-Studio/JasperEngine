# Embeds the Lucide icon pack (lucide.pack, built by tools/htmlui/build_lucide_pack.py) into
# a C++ header for htmlui::compat::LucideIcons (phase-2 §3.11.8):
#
#   cmake -DPACK=<engine/libs/lucide/lucide.pack> -DOUTPUT=<htmlui_lucide.h> -P embed_lucide.cmake
#
# The editor's Apple export writes the same header from C++
# (htmlui::compat::LucideIcons::buildEmbeddedHeader); keep the two formats in step.

if(NOT PACK OR NOT OUTPUT)
    message(FATAL_ERROR "embed_lucide.cmake needs PACK and OUTPUT")
endif()

file(READ "${PACK}" pack_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," pack_bytes "${pack_hex}")
string(LENGTH "${pack_hex}" hex_length)
math(EXPR byte_count "${hex_length} / 2")

set(content "// Generated from engine/libs/lucide/lucide.pack. Do not edit.\n\n")
string(APPEND content "#pragma once\n\n#include <cstddef>\n\n")
string(APPEND content "namespace jasperengine { namespace htmlui { namespace compat { namespace embedded {\n\n")
string(APPEND content "static const unsigned char lucidePack[] = {${pack_bytes}0x00};\n")
string(APPEND content "static const size_t lucidePackSize = ${byte_count};\n\n} } } }\n")

# Rewritten only when the pack changed, so an unchanged pack rebuilds nothing
file(WRITE "${OUTPUT}.tmp" "${content}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.tmp" "${OUTPUT}")
file(REMOVE "${OUTPUT}.tmp")
