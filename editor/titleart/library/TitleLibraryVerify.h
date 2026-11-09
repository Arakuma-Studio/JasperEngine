// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R32, R36, R38, R46, R47, §6.13, §6.14.2)
// - Purpose: what `title-library verify`, `gen-docs` and the thumbnail recipes share, with no GPU:
//   - verifyLibrary: every built-in item loads canonically with no warning; its metadata is
//     complete (both names, >= 3 tags, a valid family, font pack ids that exist, a provenance, a
//     review record for ported items); every member it carries is one the schema or the
//     definition knows (a typo is an `unknown_member` finding, not a silently kept member); no
//     denied term anywhere; its thumbnails' recipes are fresh. Findings carry the item and a code.
//   - thumbnailRecipe: R32's SHA-256 of (the item's canonical bytes, the font files it uses,
//     TITLE_ART_BAKE_VERSION of its sample document, the text, the size), the `tEXt` value
//     `jasper.titleart.thumb` of its PNG.
//   - libraryDocsMarkdown: docs/mcp/guide/title-library.md (R47), one `###` section per family.
// - Thread-safety: main thread.

#pragma once

#include "TitleLibrary.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    struct VerifyFinding {
        std::string item;           // ref ("" for the library)
        std::string code;           // invalid, not_canonical, unknown_member, missing_name, few_tags, bad_family, missing_font,
                                    // no_provenance, no_review, denied_term, thumbnail_stale, thumbnail_missing, unavailable
        std::string message;
        bool warning = false;       // warnings do not fail verify
    };
    struct VerifyOptions {
        bool thumbnails = true;     // check the staged thumbnails' recipes (packDir()/thumbnails)
        std::filesystem::path reviewFile;   // tools/titleart/pachislop-port/review.json ("" skips review records)
    };
    std::vector<VerifyFinding> verifyLibrary(const VerifyOptions& options);
    // members a sparse object carries that its kind (or effect / generator definition) lacks
    void unknownMembers(const Json& value, const std::string& kind, const std::string& path, std::vector<std::string>& out);

    // the sample texts of R32 (a look may override them with thumbnail_texts)
    struct ThumbnailSample {
        std::string key;            // "latin", "kanji", "digits", or "en" / "ja" for templates
        std::string text;
    };
    std::vector<ThumbnailSample> thumbnailSamples(const LibraryItemInfo& item);
    constexpr int THUMBNAIL_WIDTH = 240, THUMBNAIL_HEIGHT = 135;
    constexpr const char* THUMBNAIL_TEXT_KEY = "jasper.titleart.thumb";
    // the document a thumbnail renders (preview applied, nothing imported) and the pack files it reads
    bool thumbnailDocument(const LibraryItemInfo& item, const ThumbnailSample& sample, titleart::TitleArtDoc& out,
                           std::map<std::string, std::string>& transient, std::string& error);
    std::string thumbnailRecipe(const LibraryItemInfo& item, const ThumbnailSample& sample);
    // the recipe a PNG carries ("" when none)
    std::string readThumbnailRecipe(const std::filesystem::path& png);

    std::string libraryDocsMarkdown();

}
