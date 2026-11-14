// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_H
#define LOCALIZATION_H

// Contract (docs/specs/S95-lua-runtime-gaps.md REQ-26 to REQ-28, REQ-36, D-13)
// - Purpose: the game's String Tables (StringTable) and the lookup of a key in the game's
//   language: Unity's Localization package (LocalizationSettings.SelectedLocale, String Tables,
//   Smart Strings' {0} / {name} and plurals), as the Lua `Localization`.
// - One language: TitleArtLanguage owns it (the global TitleArt.setLanguage value, the device's
//   locales, the project default), so titles and strings switch together. The current language
//   is TitleArtLanguage::resolve of S87's candidates against the project's languages (when the
//   project lists none: every language of the tables), else the project default.
// - Lookup: a key's value in the current language, else its base language ("pt-BR" -> "pt"), else
//   the project default; else the key itself, warned once per key. "Table:key" looks in one table
//   (the file name without .stringtable); a bare key in the tables in configure() order (the
//   first one that has it).
// - Formatting: {0} {1}... are the positional arguments, {name} the named ones, {{ and }} are
//   braces; a placeholder without an argument stays as written. A plural value picks its form by
//   the `count` argument (else the first positional one) under the CLDR rules of the value's
//   language; without a number it shows `other`.
// - Cost: nothing is read until the first lookup (configure only stores the list); a lookup is one
//   hash lookup and the formatting. The values of the current language are resolved once per
//   generation().
// - Remember Language Choice (start/stop): start() applies the language a previous run chose
//   (UserSettings REMEMBERED_LANGUAGE_KEY) and, while running, stores each change of the global
//   language ("" removes it).
// - Thread-safety: main thread.

#include "Export.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    struct StringTable;

    struct JASPERENGINE_API LocalizationArg{
        std::string text;
        bool isNumber = false;
        double number = 0.0;

        static LocalizationArg ofText(const std::string& text);
        // the text of a number as Lua prints it (3, 2.5)
        static LocalizationArg ofNumber(double number);
    };

    struct JASPERENGINE_API LocalizationArgs{
        std::vector<LocalizationArg> positional;
        std::vector<std::pair<std::string, LocalizationArg>> named;

        bool empty() const { return positional.empty() && named.empty(); }
        const LocalizationArg* find(const std::string& name) const;
        // the plural count: `count`, else the first positional argument, when it is a number
        bool count(double& out) const;
        // HTML UI's data-loc-args: an object (named), a list (positional) or one value; anything
        // else gives no arguments
        static LocalizationArgs fromJson(const std::string& json);
        std::string toJson() const;
    };

    struct JASPERENGINE_API LocalizationConfig{
        std::vector<std::string> languages;     // Project Settings > Localization > Languages
        bool rememberLanguage = true;           // ... > Remember Language Choice
        std::vector<std::string> tables;        // the .stringtable files, asset paths, in lookup order

        bool operator==(const LocalizationConfig& other) const {
            return languages == other.languages && rememberLanguage == other.rememberLanguage && tables == other.tables;
        }
        bool operator!=(const LocalizationConfig& other) const { return !(*this == other); }
    };

    class JASPERENGINE_API Localization{
    public:
        static constexpr const char* REMEMBERED_LANGUAGE_KEY = "jasperengine.language";

        // the project's tables and languages (a game's start, Play, the editor when they change);
        // the tables are read again at the next lookup
        static void configure(const LocalizationConfig& config);
        static const LocalizationConfig& getConfig();
        // a table file changed (the editor): read them again at the next lookup
        static void reloadTables();

        // the start of a game or a Play, and its end (Remember Language Choice)
        static void start();
        static void stop();

        // the language strings show (never "")
        static std::string getLanguage();
        // TitleArt.setLanguage: "" goes back to the device; false with `error` for an invalid tag
        static bool setLanguage(const std::string& code, std::string* error = nullptr);
        // the project's languages, else the tables' languages
        static std::vector<std::string> getLanguages();

        static bool has(const std::string& key);
        static std::string get(const std::string& key, const LocalizationArgs& args = LocalizationArgs());
        // changes when the language, the locales, the project default or the tables change
        static uint64_t generation();

        // the formatting alone (pure)
        static std::string format(const std::string& pattern, const LocalizationArgs& args);

        // tests: tables by name instead of files, and everything back to the start
        static void setTablesForTesting(const std::vector<std::pair<std::string, StringTable>>& tables);
        static void resetForTesting();
    };

}

#endif //LOCALIZATION_H
