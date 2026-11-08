// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

// Text the editor writes into generated CMake files
namespace jasperengine::editor::cmaketext {

    // The body of a C string literal holding value's bytes: letters, digits, space, '_', '.' and
    // '-' as they are, every other byte as a three-digit octal escape ("\073" for ';'). CMake
    // documents ; # , % & | ^ < > and \" as broken in some generators' command lines, and the
    // bytes of a UTF-8 title would otherwise depend on the compiler's character sets.
    std::string cStringLiteralBody(std::string_view value);

    // value inside a CMake quoted argument: \ " and $ escaped, so nothing expands
    std::string cmakeQuoted(std::string_view value);

    // What set(VAR "...") or add_definitions("-DNAME=\"...\"") needs for a C string macro
    // holding value: cmakeQuoted(cStringLiteralBody(value))
    std::string cStringDefinitionValue(std::string_view value);

}
