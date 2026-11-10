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
#include <memory>
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
        std::string language;                       // S87: checks run per language
        std::string animation;                      // S85
        bool failOnWarn = false;                    // fail_on: "warn"
        float renderScale = 1.0f;                   // the pixel checks' render size (the library gate's 0.25)
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
        // renders a request through the caller's renderer and reads it back; null for checks run
        // without a GPU (their pixel checks report SKIP)
        std::function<bool(const titleart::RenderRequest& request, TitleCheckImage& out, std::string& error)> render;
        // the pixel checks' renders of this context, shared by its checks (TitleArtBuiltinChecks)
        std::shared_ptr<void> renders;
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

        titleart::Json toJson() const;
    };

    struct CheckDef {
        std::string id;
        TitleCheckKind kind = TitleCheckKind::Document;
        std::string owner = "S80";
        std::string group;                          // `checks` may name a group ("anim")
        std::string description;
        // appends its results (one, or one per rule)
        std::function<void(TitleCheckContext& context, std::vector<TitleCheckResult>& out)> run;
    };

    struct TitleCheckReport {
        TitleCheckStatus verdict = TitleCheckStatus::Pass;      // the worst of the checks (SKIP counts as PASS)
        std::vector<TitleCheckResult> checks;
        titleart::Json look = titleart::Json::array();          // [{crop, why}]: where a person should look

        titleart::Json toJson() const;
    };

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
