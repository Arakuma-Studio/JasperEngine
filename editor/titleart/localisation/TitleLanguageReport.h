// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R19, R51, R54, R56)
// - Purpose: the state of a title's languages as every surface shows it: per language how many
//   texts are translated, missing, stale and overflowing, and its bake set's status (get_title's
//   `languages`, the Designer's switcher and Translations panel, the Inspector's summary line),
//   and a text layer's strings metadata.
// - Overflow is measured by geometry (TitleFit), so it is cached per (layer, language, text) by
//   TitleFit and cheap to ask again.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleFit.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
}

namespace jasperengine::editor {

    class Project;

    struct TitleLanguageState {
        std::string code;
        std::string name;               // the Language's name, else the built-in English name
        bool source = false;
        int translated = 0;
        int missing = 0;
        int stale = 0;
        int overflow = 0;
        std::string bake;               // fresh | stale | missing | source (reuses the source set) | off (bake false) | "" (not asked)
    };

    // `bakeStatus`: read the language sets' bake status beside `document` (needs `project`)
    std::vector<TitleLanguageState> titleLanguageStates(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets,
                                                        Project* project = nullptr, const std::filesystem::path& document = {});
    // get_title's `languages`: {source, list: [{code, name, translated, missing, stale, overflow, bake}]}
    titleart::Json titleLanguagesJson(const std::vector<TitleLanguageState>& states, const titleart::TitleArtDoc& doc);
    // a text layer's {text_key, translate, max_chars, translator_note, translated: [codes]}
    titleart::Json titleLayerStringsJson(const titleart::Json& layer);
    // a fit result as get_title shows it: {scale, h_scale, lines, fits, overflow_px, extent, box}
    titleart::Json titleFitJson(const titleart::TitleFitResult& fit);
    // a render sheet row's label: "de · fit 0.82 · overflow 1" (the smallest fit scale of its texts)
    std::string titleLanguageRowLabel(const titleart::TitleArtDoc& doc, const std::string& code, const titleart::AssetResolver* assets);
    // "Languages: en (source), de, ja · 1 missing" (the Inspector's summary)
    std::string titleLanguagesSummaryLine(const std::vector<TitleLanguageState>& states);

}
