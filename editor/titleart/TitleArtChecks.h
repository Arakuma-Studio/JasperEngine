// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R56, §6.12)
// - Purpose: the registry of check_title's checks. Each check is a row (CheckDef: id, kind,
//   owner, group, run), so later specs add theirs (S81 glyph_atlas, characters; S83 fx_*; S84
//   style_lint, color_scheme_match...; S85 the anim_* group; S86 3d_*; S87 languages_complete...)
//   without touching the tool. No two specs register the same id (registerCheck refuses).
// - A check measures and reports: PASS / WARN / FAIL (or SKIP when it does not apply) with the
//   measured value, the threshold used, crops of where to look, the method, and a ready `fix`
//   (ops for edit_title) where there is one. A check never edits, and the report always carries
//   the guidance that passing is not aesthetic approval.
// - Kinds: `document` checks read the document (no GPU; S82 runs them 300 ms after a commit,
//   check --titles runs them headless), `pixels` checks render through the context's render
//   function, `document_pixels` do both, `estimate` read cost models (S85).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
    struct RenderRequest;
}

namespace jasperengine::editor {

    enum class TitleCheckKind : uint8_t { Document, Pixels, DocumentPixels, Estimate };
    enum class TitleCheckStatus : uint8_t { Pass, Warn, Fail, Skip };
    const char* titleCheckKindName(TitleCheckKind kind);       // "document", "pixels", "document_pixels", "estimate"
    const char* titleCheckStatusName(TitleCheckStatus status); // "PASS", "WARN", "FAIL", "SKIP"

    // a render a pixel check reads: straight-alpha RGBA8, rows top first, and its frame
    struct TitleCheckImage {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
        titleart::TitleRectI frame;         // canvas pixels, image space
    };

    struct TitleCheckOptions {
        std::vector<std::string> backgrounds = {"black", "white"};
        std::optional<int> displayHeightPx;         // S82's Device view: legibility at that height
        std::string baseline;                       // png path or "rev:<n>" (the golden check)
        std::string language;                       // S87: the language a per-language run is for ("" = the source)
        std::vector<std::string> languages;         // S87: the languages to check (empty: every language of the document)
        std::vector<std::string> projectLanguages;  // S87: the project's localization.languages
        std::string animation;                      // S85
        bool failOnWarn = false;                    // fail_on: "warn"
    };

    struct TitleCheckContext {
        TitleCheckContext(const titleart::TitleArtDoc& doc, std::string path) : doc(doc), path(std::move(path)) {}

        const titleart::TitleArtDoc& doc;
        std::string path;                           // project-relative
        std::filesystem::path file;                 // absolute (bake_fresh reads the outputs beside it)
        const titleart::AssetResolver* assets = nullptr;
        // what loading the file reported (document_valid): normalize warnings, and the version
        // error of a newer file (then `doc` is empty)
        std::vector<titleart::TitleWarning> loadWarnings;
        std::string versionError;
        TitleCheckOptions options;
        // the project's Title Art Renderers that show the document in Realtime or Glyph Atlas
        // ("scene Main: Logo (realtime)"), when the caller read the project (check_title, check
        // --titles); unset when it did not: S83's runtime checks then take it as a runtime title
        std::optional<std::vector<std::string>> runtimeInstances;
        // S87: in a per-language run, the document as stored (`doc` is the one resolved for
        // options.language); null otherwise
        const titleart::TitleArtDoc* source = nullptr;
        // renders a request through the caller's renderer and reads it back; null for checks run
        // without a GPU (their pixel checks report SKIP)
        std::function<bool(const titleart::RenderRequest& request, TitleCheckImage& out, std::string& error)> render;
        // S85: an animation's frame at a time as the companion shows it (`as`: exact, baked,
        // realtime, glyph_atlas; `companion` false leaves the title-level values out), through
        // the caller's project; null without a GPU (the animation pixel checks report SKIP)
        std::function<bool(const std::string& animation, double time, const std::string& as, bool companion, TitleCheckImage& out,
                           std::string& error)> renderAnimation;
        // S85: the Render Mode the checks judge (a component's; unset: every mode)
        std::optional<int> renderMode;
        // S85: whether a project asset (assets-relative path, GUID) exists (marker sounds); null
        // without a project
        std::function<bool(const std::string& path, const std::string& guid)> assetExists;
    };

    struct TitleCheckResult {
        std::string id;                             // the check's id, or "<id>.<rule>" for one of its results
        TitleCheckKind kind = TitleCheckKind::Document;
        TitleCheckStatus status = TitleCheckStatus::Pass;
        titleart::Json measured;
        titleart::Json threshold;
        std::vector<titleart::TitleRectI> at;       // crops, image space of the canvas
        std::string message;
        titleart::Json fix;                         // {ops: [...]} ready for edit_title, or null
        std::string method;                         // how it measured
        std::string language;                       // S87: the language it is about ("" = none)

        titleart::Json toJson() const;
    };

    struct CheckDef {
        std::string id;
        TitleCheckKind kind = TitleCheckKind::Document;
        std::string owner = "S80";
        std::string group;                          // `checks` may name a group ("anim")
        std::string description;
        // S87: runs again for each translation language of a multilingual document, on the
        // document resolved for it (glyphs_present, bake_fresh); results carry the language
        bool perLanguage = false;
        // appends its results (one, or one per rule)
        std::function<void(TitleCheckContext& context, std::vector<TitleCheckResult>& out)> run;
    };

    struct TitleCheckReport {
        TitleCheckStatus verdict = TitleCheckStatus::Pass;      // the worst of the checks (SKIP counts as PASS)
        std::vector<TitleCheckResult> checks;
        titleart::Json look = titleart::Json::array();          // [{crop, why}]: where a person should look

        titleart::Json toJson() const;
    };

    // S87: the document's languages a check run covers (options.languages, else every one)
    std::vector<std::string> titleCheckLanguages(const TitleCheckContext& context);

    class TitleArtChecks {
    public:
        // false when the id is taken (or the definition has no id or run)
        static bool registerCheck(CheckDef def);
        static bool unregisterCheck(const std::string& id);
        static const CheckDef* find(const std::string& id);
        // in registration order
        static std::vector<const CheckDef*> all();
        // the checks `names` select (ids or group names; empty: every check); false with `error`
        // and the valid names for an unknown one
        static bool select(const std::vector<std::string>& names, std::vector<const CheckDef*>& out, std::string& error,
                           std::vector<std::string>& valid);
        static TitleCheckReport run(const std::vector<const CheckDef*>& checks, TitleCheckContext& context);
        // "These are measurements; passing them does not mean the title looks good. Show the render to a person."
        static const char* guidance();
    };

}
