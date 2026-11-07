// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CMakeText.h"

namespace jasperengine::editor::cmaketext {

    std::string cStringLiteralBody(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            const unsigned char byte = static_cast<unsigned char>(c);
            const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                               byte == ' ' || byte == '_' || byte == '.' || byte == '-';
            if (plain) {
                out += c;
                continue;
            }
            // always three digits, so a digit after the escape can't extend it
            out += '\\';
            out += static_cast<char>('0' + ((byte >> 6) & 7));
            out += static_cast<char>('0' + ((byte >> 3) & 7));
            out += static_cast<char>('0' + (byte & 7));
        }
        return out;
    }

    std::string cmakeQuoted(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (c == '\\' || c == '"' || c == '$') out += '\\';
            out += c;
        }
        return out;
    }

    std::string cStringDefinitionValue(std::string_view value) {
        return cmakeQuoted(cStringLiteralBody(value));
    }

}
