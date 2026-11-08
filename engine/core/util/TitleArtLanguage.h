// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_LANGUAGE_H
#define TITLE_ART_LANGUAGE_H

// Contract (docs/specs/S87-title-localisation-interchange.md R12-R14, R24-R26, D4, §6.4, §6.6)
// - Purpose: which language a Title Art Renderer shows, without the `titleart` module: tag
//   checks and canonical case (LocaleRules::normalize plus the legacy codes iw, in, ji), the
//   RFC 4647 lookup with the Chinese script rule (match), the candidate order (component, the
//   global TitleArt.setLanguage value, the device's preferred locales, the project default, the
//   document's source) and the bake sidecar's `languages` table. Baked titles switch with this
//   and S81's TitleArtBakeInfo alone, so it lives in core beside it (R26).
// - match(candidate, available): (1) the same tag, case-insensitive; (2) the same language and
//   script ignoring region, the script of `zh` inferred (TW, HK, MO: Hant; else Hans; other
//   languages: none), a tag without region preferred; (3) the same language when either side has
//   no script, the bare language first, then the first `lang-*` in `available` order; else "".
//   zh-Hans never matches zh-Hant, sr-Latn never sr-Cyrl.
// - The global language is runtime state only (never saved); listeners hear each change once.
// - Thread-safety: main thread (the global value and listeners); the pure functions are safe
//   from any thread.

#include "Export.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    // the sidecar's `languages` member (only the source sidecar has one, §6.6)
    struct JASPERENGINE_API TitleArtLanguageTable{
        bool present = false;
        std::string source;                 // the document's source language
        std::string defaultLanguage;        // the project default when the document has it, else the source
        // per translation language its sidecar file (relative to the document's directory), ""
        // when it reuses the source set (`null` in the file); in the file's order
        std::vector<std::pair<std::string, std::string>> sets;

        // the source first, then every set's code
        std::vector<std::string> available() const;
        // the sidecar file of `code` ("" for the source set or an unknown code)
        std::string setFile(const std::string& code) const;
    };

    class JASPERENGINE_API TitleArtLanguage{
    public:
        // BCP 47 canonical case of a tag with the legacy codes replaced ("iw-il" -> "he-IL",
        // "zh_hant_tw" -> "zh-Hant-TW"); "" stays ""
        static std::string canonicalize(const std::string& tag);
        // a well-formed tag: a 2-3 letter (or 4-8 letter) language, then subtags of 1-8 letters
        // or digits; false with `error`, the one sentence Lua and ops show
        static bool isValid(const std::string& tag, std::string* error = nullptr);

        // the entry of `available` that `candidate` selects (R13), "" when none
        static std::string match(const std::string& candidate, const std::vector<std::string>& available);
        // the first candidate that matches; "" when none does (an empty candidate is skipped)
        static std::string resolve(const std::vector<std::string>& candidates, const std::vector<std::string>& available);
        // R12's order: the component's language, the global one, `preferred` in order (the
        // device's locales, the editor's simulated language in Play), the project default, the
        // document's source; empty entries are left out
        static std::vector<std::string> candidates(const std::string& component, const std::vector<std::string>& preferred,
                                                   const std::string& source);

        // TitleArt.setLanguage: "" goes back to the device; false with `error` for an invalid tag
        // (nothing changes); a change bumps generation() and calls the listeners once
        static bool setGlobal(const std::string& code, std::string* error = nullptr);
        static const std::string& global();
        // counts the changes of the global language and of the project default (renderers compare it)
        static uint64_t generation();
        using Listener = std::function<void(const std::string& code)>;
        static int addListener(Listener listener);
        static void removeListener(int id);

        // the project's `localization.default_language` (the start values of a game, the editor's
        // project settings); "" is the first of `localization.languages`, else "en" (R64), which
        // the caller resolves before passing it
        static void setProjectDefault(const std::string& code);
        static const std::string& projectDefault();

        // the sidecar's `languages` table from its JSON text; false when the member is absent or
        // malformed (the runtime then uses the source set, R65)
        static bool parseTable(const std::string& sidecarText, TitleArtLanguageTable& out);

        // tests: the global value, listeners and project default back to their start
        static void resetForTesting();
    };

}

#endif //TITLE_ART_LANGUAGE_H
