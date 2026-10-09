// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_LITERALCOMPARE_H
#define COMPAT_LITERALCOMPARE_H

// Internal to the compat layer's .cpp files (never included by a header): `text == "literal"`
// on a std::string calls the out-of-line basic_string::compare(const char*) of libstdc++.so
// (strlen + memcmp); the rewriters and transforms compare names with literals everywhere, so
// these overloads compare through string_view instead: the literal's length is a constant and
// a size mismatch returns at once. The results are the same. Found by unqualified lookup from
// code in (or using) the compat namespace, preferred over std's templates as non-templates.

#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    inline bool operator==(const std::string& a, const char* b) noexcept { return std::string_view(a) == std::string_view(b); }
    inline bool operator==(const char* a, const std::string& b) noexcept { return std::string_view(a) == std::string_view(b); }
    inline bool operator!=(const std::string& a, const char* b) noexcept { return std::string_view(a) != std::string_view(b); }
    inline bool operator!=(const char* a, const std::string& b) noexcept { return std::string_view(a) != std::string_view(b); }

}

#endif //COMPAT_LITERALCOMPARE_H
