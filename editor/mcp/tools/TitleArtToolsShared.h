// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.11)
// - Purpose: what the `title_art` tools share (TitleArtTools.cpp: list, get, edit, export;
//   TitleArtToolsRender.cpp: render, check): document paths, the JSON flavours (the tools'
//   nlohmann::json and the documents' ordered_json), PNG encoding, the render sheets, the kept
//   revision renders `compare: "rev:<n>"` reads, and the per-frame job runner.
// - Thread-safety: main thread.

#pragma once

#include "mcp/McpCall.h"
#include "mcp/McpJobs.h"
#include "mcp/McpJson.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtRenderHost.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
    class TitleArtDocument;
}

namespace jasperengine::editor::mcp::titletools {

    titleart::Json toOrdered(const Json& value);
    Json fromOrdered(const titleart::Json& value);

    // a `.titleart` argument (project-relative, or assets-relative): its absolute and
    // project-relative paths; false with `error`
    bool resolveTitlePath(Project* project, const std::string& given, bool mustExist, std::filesystem::path& absolute,
                          std::string& relative, std::string& error);
    // the document of a path (shared with the Title Designer); null with `error`
    std::shared_ptr<TitleArtDocument> openTitle(Project* project, const std::filesystem::path& absolute, std::string& error);

    // PNG bytes of an RGBA (or grey) image, base64 for a ToolResult image
    bool pngBase64(const texsynth::Image& image, std::string& base64);
    ToolImage toolImage(const texsynth::Image& image);
    // box-filtered down so the width is at most `maxWidth` (never up)
    texsynth::Image downscaleTo(const texsynth::Image& image, int maxWidth);

    // a background of a sheet: "transparent", "checker", "black", "white", "#rrggbb" or an image
    // path; false with `error` naming the valid forms
    struct SheetBackground {
        std::string name;
        enum class Kind { Transparent, Checker, Color, Image } kind = Kind::Checker;
        uint8_t color[3] = {0, 0, 0};
        texsynth::Image image;
    };
    bool parseBackgrounds(Project* project, const Json& list, std::vector<SheetBackground>& out, std::string& error);
    // `image` (straight RGBA8) over a background (the result keeps alpha only for transparent)
    texsynth::Image composite(const texsynth::Image& image, const SheetBackground& background);
    // cells side by side in `columns` columns with a 4 px gap; each cell's box
    struct SheetCell {
        int n = 0;
        std::string label;
        std::string background;
        int x = 0, y = 0, width = 0, height = 0;
    };
    texsynth::Image sheet(const std::vector<texsynth::Image>& cells, int columns, std::vector<SheetCell>& boxes);

    // ---- the kept revision renders (.jasperengine/cache/titleart/renders/<guid>/rev<n>.png) ----

    std::filesystem::path revisionRenderPath(Project* project, const std::string& guid, uint64_t rev);
    // keeps the render of a revision (the 16 newest per document; the folder is emptied the
    // first time a document renders in an editor session, when its revs start again)
    void keepRevisionRender(Project* project, const std::string& guid, uint64_t rev, const texsynth::Image& image);
    // the kept revisions of a document, newest first
    std::vector<uint64_t> keptRevisions(Project* project, const std::string& guid);

    // ---- metrics of a render (render_title, edit_title's render) ----

    // {size, alpha_bounds {x, y, w, h, margins}, coverage_pct, flat} of a straight RGBA8 render of
    // `frame` (canvas pixels)
    Json renderMetrics(const texsynth::Image& image, const titleart::TitleRectI& frame, int canvasWidth, int canvasHeight);

    // ---- per-frame work as a job ----

    // runs `tick` once per editor frame (inside the call's write and undo scopes) until it
    // returns true; the job's result is what `finish` builds. Replies through replyWhenJobDone.
    struct FrameTask {
        std::function<bool(McpJob& job)> tick;                  // true when done (job finished by it)
        std::vector<ToolImage> images;
        std::string text;
    };
    void runFrameTask(const CallPtr& call, const std::string& kind, const std::string& title, std::shared_ptr<FrameTask> task);

    // the render_title machinery, shared with edit_title's `render`
    struct RenderSpec {
        std::filesystem::path file;
        std::string relative;
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        uint64_t rev = 0;
        bool current = true;            // the document as the file holds it (kept as rev render)
        Json args;                      // the tool's arguments (backgrounds, crop, max_width...)
    };
    // starts a render as a job and replies with its result merged into `extra` (render_title,
    // edit_title {render}); `textPrefix` leads the summary
    void startRender(const CallPtr& call, RenderSpec spec, Json extra, const std::string& textPrefix = {});

    // the handlers of TitleArtToolsRender.cpp
    void renderTitleTool(const CallPtr& call);
    void checkTitleTool(const CallPtr& call);
    // render_title's `options` keys: those of later specs answer not_available naming the owner;
    // false with `error` (bad_option listing the keys this build knows)
    bool checkRenderOptions(const Json& options, std::string& error);

    // S87: export_title's delivery formats (png, psd, svg, sheet, atlas, sequence, ktx2, csv,
    // xliff) through TitleExport.h, as a job (TitleArtToolsInterchange.cpp)
    void exportTitleInterchange(const CallPtr& call, Project* project, const std::filesystem::path& file, const std::string& relative,
                                const std::string& format);

}
