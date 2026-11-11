// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R32, R33, R64, §6.9)
// - Purpose: what a bake of a `.titleart` writes and how freshness is decided, without a GPU:
//   the derived file names, the shared trimmed frame, alpha bleed, the recipe chunk of every baked
//   PNG (iTXt `jasper.titleart`), the bake sidecar `<file>.bake.json`, and the status of each
//   output (fresh / stale / missing / invalid). The baker (TitleArtBaker.h) renders and writes;
//   the Inspector, get_title, get_asset_info, check --titles, the export and Auto Bake read the
//   status from here.
// - Freshness (R33, R64): an output is fresh when its PNG's recipe names this document (GUID) and
//   output, and its source hash (SHA-256 of the canonical document text), asset hashes (SHA-256 of
//   every font, image, SVG and map file the document names) and bake version
//   (titleArtBakeVersion) equal the current ones; the PNG bytes never decide (GPU output is not
//   byte-stable, baked PNGs are committed derived files).
// - Names (§6.15): `logo.titleart` bakes `logo.titleart.png` (output `main`),
//   `logo.titleart.<id>.png` (the others) unless the output names a `file` (relative to the
//   document's directory), and `logo.titleart.bake.json`.
// - Spaces: frames and alpha bounds are canvas pixels in image space (top-left origin, +y down);
//   the sidecar's `origin` is the document origin (the canvas centre) in frame pixels.
// - Thread-safety: pure functions, except the file hash cache (a mutex) and status, which reads
//   files.

#pragma once

#include "texsynth/SynthImage.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    constexpr const char* TITLE_ART_RECIPE_KEYWORD = "jasper.titleart";
    constexpr const char* TITLE_ART_RECIPE_FORMAT = "jasperengine.titleart.bake";
    constexpr int TITLE_ART_RECIPE_VERSION = 1;
    constexpr const char* TITLE_ART_BAKEINFO_FORMAT = "jasperengine.titleart.bakeinfo";
    constexpr int TITLE_ART_BAKEINFO_VERSION = 1;
    constexpr const char* TITLE_ART_SIDECAR_SUFFIX = ".bake.json";
    constexpr int TITLE_ART_BLEED_PX = 8;           // three mip levels (S87's exports pass up to 64)

    // ---- names ----

    // the PNG of an output, relative to the document's directory; a translation language's set
    // (S87 R16) puts `.<code lowercase>` right after the document's name ("logo.titleart.de.png",
    // "logo.titleart.de.glow.png"; an output's own `file` gets it before its extension)
    std::string titleArtOutputFileName(const std::string& documentFileName, const titleart::Json& output, const std::string& language = {});
    std::filesystem::path titleArtOutputPath(const std::filesystem::path& document, const titleart::Json& output, const std::string& language = {});
    std::filesystem::path titleArtSidecarPath(const std::filesystem::path& document, const std::string& language = {});
    // every derived file of a document that exists (its outputs' PNGs and their metas, the
    // sidecar), for renames, moves and deletes; `doc` null reads the document from disk
    std::vector<std::filesystem::path> titleArtDerivedFiles(const std::filesystem::path& document, const titleart::TitleArtDoc* doc = nullptr);

    // ---- pixels ----

    // Fully transparent pixels within `reachPx` (Euclidean) of a pixel with alpha > 0 take the
    // colour of the nearest such pixel; farther ones are written black; alpha stays 0 and pixels
    // with alpha > 0 are not changed. RGBA images, 8- or 16-bit.
    void bleedAlpha(texsynth::Image& image, int reachPx = TITLE_ART_BLEED_PX);

    // one output's part of the shared frame
    struct TitleFrameInput {
        titleart::TitleRectI contentBounds;     // canvas pixels (image space), empty when it shows nothing
        bool trim = true;
        double paddingCanvasPx = 4.0;           // the output's padding / its scale
        int roundTo = 4;
    };
    // The union of the outputs' content bounds plus their padding, sides rounded up to the
    // largest round_to (grown right and down, then left and up at the canvas edge), clipped to
    // the canvas; the whole canvas when an output does not trim; empty when nothing shows
    titleart::TitleRectI titleArtBakeFrame(int canvasWidth, int canvasHeight, const std::vector<TitleFrameInput>& outputs);
    // an output's PNG size for the frame at `scale`, sides rounded up to `roundTo`
    void titleArtOutputSize(const titleart::TitleRectI& frame, double scale, int roundTo, int& width, int& height);

    // ---- hashes ----

    // SHA-256 (hex) of the canonical text of the document
    std::string titleArtSourceHash(const titleart::TitleArtDoc& doc);
    // SHA-256 (hex) of a file, cached by path, size and modification time; "" when unreadable
    std::string titleArtFileHash(const std::filesystem::path& path);
    // the cache of titleArtFileHash on disk (check --titles keeps it between runs)
    void titleArtHashCacheLoad(const std::filesystem::path& file);
    void titleArtHashCacheSave(const std::filesystem::path& file);

    struct TitleBakeAsset {
        std::string guid;
        std::string path;           // assets-relative
        std::string sha256;         // "" when the asset is missing
        bool operator==(const TitleBakeAsset& other) const { return guid == other.guid && path == other.path && sha256 == other.sha256; }
    };
    // every distinct asset the document names (by GUID, else path), sorted by path
    std::vector<TitleBakeAsset> titleArtBakeAssets(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets);

    // ---- the recipe chunk ----

    struct TitleBakeRecipe {
        std::string sourceGuid;
        std::string sourcePath;     // the document, assets-relative
        std::string sourceHash;
        std::vector<TitleBakeAsset> assets;
        uint32_t bakeVersion = 0;
        std::string output;
        titleart::TitleRectI frame;
        double scale = 1.0;
        int bitDepth = 8;
        std::string pathUsed;       // "rgba16f" or "rgba8"
        std::string backend;
        std::string gpu;
        std::string editorVersion;
        titleart::Json extra = titleart::Json::object();    // members of later specs, kept

        titleart::Json toJson() const;
        // false with `error` when it is not a Title Art recipe of a version this build reads
        static bool fromJson(const titleart::Json& json, TitleBakeRecipe& out, std::string& error);
    };
    // the recipe of a baked PNG; nullopt with `error` ("" for a PNG without one)
    std::optional<TitleBakeRecipe> readTitleBakeRecipe(const std::filesystem::path& png, std::string* error);
    // PNG bytes with the recipe chunk spliced in
    bool insertTitleBakeRecipe(const std::vector<uint8_t>& png, const TitleBakeRecipe& recipe, std::vector<uint8_t>& out, std::string& error);

    // ---- the sidecar ----

    struct TitleBakeInfoOutput {
        std::string id;
        std::string file;           // relative to the document's directory
        std::string content;        // all, layers, ink, glow, mask
        int width = 0, height = 0;
    };
    struct TitleBakeInfo {
        titleart::TitleRectI frame;
        double originX = 0.0, originY = 0.0;
        std::vector<TitleBakeInfoOutput> outputs;
        titleart::TitleRectI alphaBounds;
        std::string sourceHash;
        uint32_t bakeVersion = 0;
        titleart::Json extra = titleart::Json::object();    // `anchors` (S83), `languages`, `language` (S87)...

        titleart::Json toJson() const;
        std::string toText() const;     // canonical: 2-space indent, members in this order, then `extra`
        // false with `error` for text that is not a sidecar, or a newer version (naming it)
        static bool parse(const std::string& text, TitleBakeInfo& out, std::string& error);
    };

    // ---- status ----

    enum class TitleOutputStatus : uint8_t { Fresh, Stale, Missing, Invalid };
    const char* titleOutputStatusName(TitleOutputStatus status);       // "fresh", "stale", "missing", "invalid"
    const char* titleOutputStatusLabel(TitleOutputStatus status);      // "Fresh", "Stale", "Missing", "Invalid"

    struct TitleOutputState {
        std::string id;
        std::filesystem::path file;         // absolute
        std::string fileName;               // relative to the document's directory
        std::string content;
        TitleOutputStatus status = TitleOutputStatus::Missing;
        std::vector<std::string> changed;   // stale: "document", "bake_version", "asset <path>", ...
        std::string reason;                 // one line for tooltips and problems
        std::optional<TitleBakeRecipe> recipe;
        int width = 0, height = 0;          // the PNG's size when it exists

        titleart::Json toJson(const std::filesystem::path& projectPath) const;
    };

    struct TitleBakeStatus {
        std::vector<TitleOutputState> outputs;
        std::string sourceHash;
        uint32_t bakeVersion = 0;
        std::vector<TitleBakeAsset> assets;
        bool sidecarPresent = false;
        bool readOnly = false;              // a newer version: never rebaked

        bool allFresh() const;
        bool anyBaked() const;              // some output's PNG exists
        // the ids of outputs that are not fresh
        std::vector<std::string> notFresh() const;
    };
    // The status of every output of `doc` (at `document`, absolute); `assets` resolves the
    // references (the editor's or the CLI's database). A multilingual document (S87 R16): the set
    // of `language` ("" = the source), fresh when its recipes' `resolved_hash` equals the hash of
    // the document resolved for that language, so a translation stales only its own set.
    TitleBakeStatus titleArtBakeStatus(const std::filesystem::path& document, const titleart::TitleArtDoc& doc,
                                       const titleart::AssetResolver& assets, const std::string& language = {});
    // S87: SHA-256 (hex) of the canonical text of the document resolved for `language`
    std::string titleArtResolvedHash(const titleart::TitleArtDoc& doc, const std::string& language);

}
