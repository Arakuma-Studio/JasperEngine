// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_MESSAGEFORMAT_H
#define LOCALIZATION_MESSAGEFORMAT_H

// Contract (docs/specs/game-ui-localization.md R3, R5, D4)
// - The ICU MessageFormat subset of the String Tables: `{name}`, `{n, number}` with the styles
//   `integer` and `percent`, `{n, plural, offset:k =0 {...} one {# item} other {# items}}`,
//   `{g, select, a {...} other {...}}` (both need `other`), up to 4 nested plural/select levels,
//   apostrophe quoting ('' is one apostrophe; a quote before {, }, # or | starts a literal run).
// - `#` is the nearest enclosing plural's number (minus its offset), formatted like `{n, number}`.
// - A pattern that does not parse formats to its own text, verbatim: format() returns false and the
//   caller logs once. Formatting never throws and never reads past the pattern.
// - Pure: safe from any thread.

#include "Export.h"
#include "localization/LocalizationArgs.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace jasperengine::l10n {

    struct MessageError {
        size_t offset = 0;          // byte offset in the pattern
        std::string message;
    };

    // the text of a key named by a "@ns.key" argument; false when there is none
    using KeyResolver = bool (*)(void* context, std::string_view key, std::string& text, std::string& language);
    // the pseudo-locales' transform of literal text (arguments and syntax stay as they are)
    using LiteralTransform = void (*)(std::string_view literal, std::string& out);

    struct FormatOptions {
        std::string_view language;              // plural rules and number symbols
        const LocalizationArgs* args = nullptr;
        bool showMissing = true;                // a missing argument prints "{name}" (else nothing)
        KeyResolver resolveKey = nullptr;
        void* resolveContext = nullptr;
        LiteralTransform literal = nullptr;
    };

    class JASPERENGINE_API MessageFormat {
    public:
        // no syntax at all: the text is the message (the cooked tables' "plain" flag)
        static bool isPlain(std::string_view pattern);
        // false with the first problem (and its offset) when the pattern does not parse
        static bool validate(std::string_view pattern, MessageError* error = nullptr);
        // appends the formatted message to `out`; a pattern that does not parse appends itself and
        // returns false
        static bool format(std::string_view pattern, const FormatOptions& options, std::string& out);
        // the code point column (1-based) of a byte offset, for editor messages
        static size_t column(std::string_view pattern, size_t offset);
    };

}

#endif //LOCALIZATION_MESSAGEFORMAT_H
