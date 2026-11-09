// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R35-R44, D8, D9, §6.3, §6.6)
// - Purpose: export_title's delivery formats (png, psd, svg, sheet, atlas, sequence, ktx2, csv,
//   xliff) beside S80's bake: a registry of formats, each with the closed table of its `options`
//   keys (checked before anything renders: bad_option listing them), and one job that runs a
//   format's plan: renders through the one renderer (TitleArtRenderHost), CPU steps, file writes.
// - A plan is a queue: steps (CPU) and renders (a RenderRequest and what to do with its pixels);
//   what a step adds runs next, before the steps after it. The job advances within a frame budget
//   (an McpJobs job, progress done/total renders) or to the end (the CLI).
// - Names (R40): `<stem>[_<part>][.<code>].<ext>` in `out` (default `Recordings/Title Art/<stem>/`,
//   D9), listed in write order; existing files fail with output_exists unless `overwrite`;
//   `zip` bundles them into `<stem>_<format>.zip` (stored PNGs, deflated text) and lists only it.
//   MCP writes only inside the project; the CLI anywhere.
// - Frames (R36, S80's TitleFrame): an image export renders on a canvas enlarged to every layer's
//   reach (glows never clip; full-bleed generators clipped to the canvas), measures with the
//   opaque background left out, trims to alpha >= alpha_threshold, pads, rounds to round_to,
//   optionally to a power of two, caps at max_size (output_downscaled), per language or shared
//   (`registered`).
// - Pixels (R37, D8): straight alpha with colour bleed (S80's bleedAlpha within bleed_px) or
//   zeros, or premultiplied in the encoded domain; 8-bit dithered by the output pass or 16-bit
//   (refused on the RGBA8 path, bit_depth_unsupported).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "titleart/render/TitleFrame.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TitleExportRequest {
        Project* project = nullptr;
        std::filesystem::path document;         // absolute
        std::string relative;                   // project-relative (results, recipes)
        titleart::TitleArtDoc doc;              // the snapshot to export
        std::string format;
        TitleJson options = TitleJson::object();
        std::vector<std::string> languages;     // the codes to export; empty: the source only
        std::filesystem::path out;              // absolute output directory
        bool insideProjectOnly = true;          // MCP: `out` must be inside the project
        // several documents in one call (strings formats only): the documents after `document`
        std::vector<std::pair<std::filesystem::path, titleart::TitleArtDoc>> more;
    };

    struct TitleExportResult {
        bool ok = false;
        std::string code;
        std::string error;
        std::vector<std::string> files;         // project-relative inside the project, else absolute; write order
        TitleJson frames = TitleJson::object();         // language -> [x, y, w, h] canvas px
        std::vector<std::string> warnings;
        TitleJson extra = TitleJson::object();          // a format's own members (sizes, counts)

        TitleJson toJson() const;
    };

    // one image an export writes (PNG and the formats built on it)
    struct TitleImageSpec {
        double scale = 1.0;
        int bitDepth = 8;
        bool dither = true;
        bool trim = true;
        double padding = 4.0;           // output px
        int roundTo = 4;
        bool pot = false;
        int alphaThreshold = 8;
        int maxSize = 16384;
        std::string alpha = "straight"; // straight | premultiplied
        bool alphaBleed = true;
        int bleedPx = 8;
        bool metadata = true;
    };

    class TitleExportJob;

    // what a format's plan works with
    class TitleExportContext {
    public:
        TitleExportRequest request;
        TitleExportResult result;
        TitleImageSpec image;           // the image options of the call (image formats)

        // a render of `request` (doc, scale, region, language, isolate...); `done` gets its pixels
        void render(titleart::RenderRequest renderRequest, std::function<void(TitleRenderedImage&)> done);
        // CPU work after what is queued before it; false fails the export (after fail())
        void then(std::function<bool()> step);
        bool fail(const std::string& code, const std::string& message);
        void warn(const std::string& warning);

        // `<stem>[_<part>][.<code>].<ext>` (the code lowercase, as bake sets)
        std::string fileName(const std::string& part, const std::string& language, const std::string& extension) const;
        // writes (or stages for the ZIP) a file of the export; false after fail() (output_exists, write_failed)
        bool write(const std::string& name, std::string content);
        // the same for a finished file on disk (moved into place; a ZIP reads it into memory)
        bool writeFile(const std::string& name, const std::filesystem::path& file);
        // the document resolved for a language ("" = the source), its fit applied
        titleart::TitleArtDoc resolved(const std::string& language) const;

        // ---- image helpers (R36, R37) ----
        // measures `doc` for `language` (one render of the reach-enlarged canvas, the background
        // left out) and calls `done` with the image and its trimmed frame (canvas px, image space)
        void measure(std::shared_ptr<const titleart::TitleArtDoc> doc, const std::string& language, const std::string& isolateLayer,
                     std::function<void(TitleRenderedImage& image, const titleart::TitleRectI& frame)> done);
        // the frame grown by padding and rounded (round_to, pot), at the call's scale; the scale
        // reduced to fit max_size (a warning) written to `scale`
        titleart::TitleRectI finishFrame(const titleart::TitleRectI& trimmed, double& scale, int* width = nullptr, int* height = nullptr);
        // renders `doc` over `frame` (canvas px) at `scale` and hands the final pixels (bled or
        // premultiplied as the options say) to `done`
        // `width` x `height` the output size finishFrame gave (0: the frame at the scale)
        void renderFrame(std::shared_ptr<const titleart::TitleArtDoc> doc, const std::string& language, const titleart::TitleRectI& frame,
                         double scale, const std::string& isolateLayer, std::function<void(texsynth::Image& pixels)> done, int width = 0,
                         int height = 0);
        // the call's alpha handling on straight sRGB pixels (bleed, zeros, or premultiplied)
        void finishPixels(texsynth::Image& pixels) const;
        // PNG bytes with the call's chunks and the export recipe (language, options)
        bool encodePng(const texsynth::Image& pixels, const std::string& language, std::vector<uint8_t>& out);

        // internal: the job's queue
        struct Step {
            std::function<bool()> cpu;
            std::unique_ptr<titleart::RenderRequest> render;
            std::function<void(TitleRenderedImage&)> done;
        };
        std::deque<Step> queue;
        std::vector<Step> added;
        size_t rendersTotal = 0, rendersDone = 0;
        std::vector<std::pair<std::string, std::string>> zipEntries;    // name, content
        bool failed = false;
    };

    struct TitleExportFormat {
        std::string name;
        std::vector<std::string> optionKeys;    // the closed table (overwrite and zip are every format's)
        bool image = true;                      // takes the image options of R36-R37
        // queues the format's work into the context; false after ctx.fail()
        std::function<bool(TitleExportContext& ctx)> plan;
    };

    void registerTitleExportFormat(TitleExportFormat format);
    const TitleExportFormat* findTitleExportFormat(const std::string& name);
    // the image options every image format takes (scale, trim, padding ...)
    const std::vector<std::string>& titleImageOptionKeys();

    class TitleExportJob {
    public:
        explicit TitleExportJob(std::unique_ptr<TitleExportContext> context);
        ~TitleExportJob();
        // advances within about `budgetMs`; true when finished
        bool step(double budgetMs);
        const TitleExportResult& run();
        float progress() const;
        std::string progressText() const;
        const TitleExportResult& result() const { return ctx->result; }

    private:
        std::unique_ptr<TitleExportContext> ctx;
        std::unique_ptr<titleart::TitleRenderJob> rendering;
        TitleExportContext::Step current;
        bool finished = false;
        bool finish();
    };

    // checks the request (format known, options against its table, languages, `out`) and makes
    // the job; null with `code` and `error` (nothing rendered, nothing written)
    std::unique_ptr<TitleExportJob> startTitleExport(TitleExportRequest request, std::string& code, std::string& error);
    // the default output directory of a document (`Recordings/Title Art/<stem>/`)
    std::filesystem::path titleExportDefaultOut(Project* project, const std::filesystem::path& document);
    // the `languages` argument ("all", a code or a list) as the document's codes; false with `error`
    bool titleExportLanguages(const titleart::TitleArtDoc& doc, const TitleJson& value, std::vector<std::string>& out, std::string& error);

    // the formats of S87 (png, sheet, atlas, sequence, ktx2, csv, xliff; psd and svg register
    // from their modules)
    void registerTitleExportFormats();

}
