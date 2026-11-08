// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R1-R7, R16, R50, D2, D5, §6.1)
// - Purpose: a title in several languages. The schema members S87 adds (the document's
//   `source_language`, `languages`, `string_namespace`, `dpi`, `import_notes`; every layer's
//   `only_languages`; a text layer's `text_key`, `translate`, `max_chars`, `translator_note`,
//   `fit`, `translations`; image and vector layers' `translations`), their normalize rules, and
//   the resolution of a document in one language: what the renderer, the bakes, the checks and
//   the Designer draw for that language.
// - Translations are sparse (D5): a Translation holds `text`, `status` and `source_hash`, then
//   only the overrides that are present; an absent override inherits the layer's member (font
//   and fallback fonts first from the document's Language entry), so a later edit of the source
//   reaches every language that does not override it. A Translation whose `text` is "" has no
//   text: the language shows the source text and the checks report it.
// - Resolution (resolveTitleLanguage) writes the language's values into a copy of the document:
//   text and overrides, `size` x `size_scale`, `offset` added to the position, the shaping
//   language (the override, else the code; the source: the layer's, else `source_language`),
//   per-language image and SVG assets, and layers outside their `only_languages` hidden. The
//   copy is marked resolved: its `translations` and `languages` are cleared and
//   `source_language` is the resolved code, so resolving it again changes nothing and its
//   canonical text is that language's alone. Fit (TitleFit.h) runs after, on the final text.
// - A monolingual document (no `source_language`, no `languages`) is never touched: resolution
//   returns it unchanged and no S87 code runs for it (R50).
// - Thread-safety: pure functions; registration runs with the schema's build.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleArtSchema;
    struct NormalizeContext;

    constexpr size_t TITLE_MAX_LANGUAGES = 64;
    constexpr int TITLE_DEFAULT_DPI = 72;

    // the members of §6.1 (one call from registerProgramMembers)
    JASPERENGINE_API void registerTitleLanguageMembers(TitleArtSchema& schema);
    // the document rules normalize applies after the members (TitleArtIO's document pass): codes
    // in canonical case, unique, not the source, at most 64; Translation keys canonical (a
    // duplicate after canonicalising is dropped with a warning); text keys unique; an output id
    // equal to a language code renamed (output_id_is_language)
    JASPERENGINE_API void normalizeTitleLanguages(Json& root, NormalizeContext& context);

    // a source language or a translation language: every S87 path runs
    JASPERENGINE_API bool titleIsMultilingual(const Json& root);
    JASPERENGINE_API std::string titleSourceLanguage(const Json& root);
    // the source (when set), then each Language's code
    JASPERENGINE_API std::vector<std::string> titleLanguageCodes(const Json& root);
    // the Language entry of `code` (case-insensitive), null when none
    JASPERENGINE_API const Json* findTitleLanguage(const Json& root, const std::string& code);
    // the layer's Translation for `code` (case-insensitive key), null when none
    JASPERENGINE_API const Json* findTranslation(const Json& layer, const std::string& code);
    JASPERENGINE_API Json* findTranslation(Json& layer, const std::string& code);
    // a Translation that gives a text ("" is no text)
    JASPERENGINE_API bool translationHasText(const Json* translation);
    // `only_languages` lets the layer show in `code` (the source code for the source)
    JASPERENGINE_API bool layerShownInLanguage(const Json& layer, const std::string& code);

    // `source_hash` of a Translation: FNV-1a 32 of the source text's UTF-8, 8 lowercase hex
    JASPERENGINE_API std::string translationSourceHash(const std::string& sourceText);
    // the Translation members after `text`, `status` and `source_hash`, in table order
    JASPERENGINE_API const std::vector<std::string>& translationOverrideKeys();

    // `code` names the document's source (or is empty): the source renders as stored
    JASPERENGINE_API bool isSourceLanguage(const Json& root, const std::string& code);
    // the document as it renders in `code` ("" or the source: the source); `problems` gets
    // language_unknown when the document has no such language (the source is used)
    JASPERENGINE_API TitleArtDoc resolveTitleLanguage(const TitleArtDoc& doc, const std::string& code,
                                                      std::vector<TitleProblem>* problems = nullptr);
    // R16: the language renders differently from the source (a translated text, an override, a
    // Language font, an `only_languages` difference, a per-language asset)
    JASPERENGINE_API bool titleLanguageDiffers(const TitleArtDoc& doc, const std::string& code);

}
