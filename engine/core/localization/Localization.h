// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOCALIZATION_H
#define LOCALIZATION_H

// Contract (docs/specs/game-ui-localization.md R5-R10, R13, R17-R20, R27, D1, D2, D8-D10;
// docs/localization.md)
// - The game language: the first of Localization.setLanguage's value (the same global as
//   TitleArt.setLanguage: TitleArtLanguage::setGlobal), the remembered choice (UserSettings
//   `jasper.language`, with remember_language), the device's preferred locales, the project default,
//   that TitleArtLanguage::match()es an available language (R6). Resolved at setManifest and on
//   every change of the global value or the device's locales (TitleArtLanguage::localesChanged).
// - A key is `<namespace>.<key>`; its text comes from the first language of the chain that has it:
//   the language, its truncations that are available, the project default, then the owner's source
//   text (R8). A needs_review translation counts. The text is formatted with the plural rules and
//   number symbols of the language that supplied it.
// - A key no table has: get() returns `fallback`, else "" in release and "⟦key⟧" in the editor and
//   Development Builds (Engine::isDevelopmentBuild); one warning per key and language.
// - Tables load lazily: the files of a namespace's chain on its first lookup after a change; only the
//   chain's languages stay resident.
// - Pseudo-locales en-XA and ar-XB (development only): the default language's text, transformed.
// - Capture (JASPERENGINE_L10N_CAPTURE: the editor, Development Builds and tests): missing
//   translations, unknown keys and stale translations shown, with where they were asked for; a ring of
//   the last 1024 produced strings tells literal text from localised text (scan()).
// - Inactive (no manifest): every call returns at once, nothing is read or allocated (R27).
// - Thread-safety: lookups are safe from any thread (one mutex; tables are immutable and swapped
//   whole); setManifest, setLanguage, the events and the listeners belong to the main thread.

#include "Export.h"
#include "localization/LocalizationArgs.h"
#include "util/FunctionSubscribe.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef JASPERENGINE_L10N_CAPTURE
#define JASPERENGINE_L10N_CAPTURE 0
#endif

namespace jasperengine {

    namespace l10n { class StringTable; }

    // one namespace of the game: where each language's texts are (l10n/manifest.json)
    struct LocalizationNamespace {
        std::string name;
        std::string sourceLanguage;
        // language -> file: a cooked .strbin, a .stringtable (the source), a .translation, or a file
        // a registered reader takes (a .dialogue)
        std::map<std::string, std::string> files;
    };

    struct LocalizationManifest {
        std::string defaultLanguage = "en";
        bool rememberLanguage = true;
        std::vector<std::string> languages;                         // available (R6), default first
        std::map<std::string, std::string> names;                   // endonyms
        std::map<std::string, std::vector<std::string>> fonts;      // localization.fonts
        // localization.default_table: the namespace a key without one reads ("": the only table, if one)
        std::string defaultTable;
        std::vector<LocalizationNamespace> namespaces;

        JASPERENGINE_API static bool fromJson(std::string_view json, LocalizationManifest& out, std::string* error = nullptr);
        JASPERENGINE_API std::string toJson() const;
    };

    class JASPERENGINE_API Localization {
    public:
        struct Miss {
            std::string key, language, kind, where;     // kind: untranslated, unknown_key, stale
            uint32_t count = 0;
        };
        struct ScanItem {
            std::string where, text;
        };
        struct ScanReport {
            std::vector<ScanItem> literal;
            std::vector<Miss> missing;
        };
        // a reader of a source format the core does not know (dialogue-graphs.md: .dialogue)
        using Reader = std::function<std::shared_ptr<const l10n::StringTable>(const std::string& path, const std::string& language,
                                                                             std::string* error)>;

        static bool isActive();
        static std::string get(std::string_view key, const LocalizationArgs& args = {}, std::string_view fallback = {});
        // a plain message's text without a copy; "" when missing or not plain
        static std::string_view getView(std::string_view key);
        static bool has(std::string_view key);
        static std::string format(std::string_view pattern, const LocalizationArgs& args = {}, std::string_view language = {});
        // a text as the pseudo-locale `code` (en-XA, ar-XB) shows it, arguments left as {name}
        // (the Localization Tables window's Pseudo Preview)
        static std::string pseudoLocalize(std::string_view text, const std::string& code, std::string_view language = "en");
        // the consumers' lookup: false (and `out` untouched) for a key no table has; `where` names the
        // element or GameObject for capture
        static bool translate(std::string_view key, const LocalizationArgs& args, std::string& out, std::string_view where = {});

        static const std::string& getLanguage();
        static const std::vector<std::string>& getLanguages();
        static bool setLanguage(const std::string& code, std::string* error = nullptr);
        static std::string getLanguageName(const std::string& code);
        static bool isRightToLeft(std::string_view code);
        static std::vector<std::string> getLanguageFonts(const std::string& code);
        // counts the changes of language and tables (a Text with a key compares it); 0 while inactive
        static uint64_t getGeneration();
        static FunctionSubscribe<void(const std::string&)>& getOnLanguageChanged() { return onLanguageChanged; }
        // Lua Localization.onLanguageChanged; cleared at Stop (Engine::clearAllSubscriptions)
        static FunctionSubscribe<void(const std::string&)> onLanguageChanged;

        // development builds and the editor (D8)
        static std::vector<Miss> getMisses();
        static void clearMisses();
        static bool wasProduced(std::string_view text);
        static ScanReport scan();
        static int addScanner(std::function<void(ScanReport&)> scanner);
        static void removeScanner(int id);

        // the engine's consumers (HTML UI, Text): told after each change, before onLanguageChanged;
        // never cleared at Stop
        static int addListener(std::function<void(const std::string&)> listener);
        static void removeListener(int id);

        // tables (the game's start, the editor's Play, hot reload, tests)
        static bool loadManifest(const std::string& path);             // "" = none: inactive
        static void setManifest(const LocalizationManifest& manifest);
        static const LocalizationManifest& getManifest();
        static void registerReader(const std::string& extension, Reader reader);
        // the editor: pseudo-locales and "⟦key⟧" outside Play too (development rules everywhere)
        static void setEditorMode(bool editor);
        static void reloadNamespace(const std::string& ns);
        static void resetForTesting();
    };

}

#endif //LOCALIZATION_H
