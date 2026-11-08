// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R16, R17, R20, R38, R40, R55, R60, §6.8)
// - Purpose: the Baked output of a title's animations: per animation that needs frames (a track
//   that is not title live), its flipbook (whole-title frames of the non-title-live tracks at
//   k / fps, one frame size: the union of their alpha bounds with 1 px, identical frames stored
//   once: fnv1a64 then a byte compare), packed on pages (`<file>.<animation id>.flip<N>.png`, at
//   most max_page a side, 4-px aligned cells with 4-px gutters so block compression never mixes
//   two frames) with S80's recipe chunk and the animation's hash, a `.meta` of S14's import
//   settings (ui, no mips, normal quality, alpha is transparency, max size 4096); and
//   `<file>.anim.json` (every animation's settings, title tracks, markers, flipbook index and
//   R20's Baked figures). All or nothing, not an undo step (derived files).
// - Pages, being all one cell size, are a grid (stb_rect_pack's general packing is not needed:
//   S85 Deviation 9).
// - TitleFlipbookBakeJob is resumable (step(budgetMs): a frame a step) for McpJobs and the Title
//   Designer; cancel() leaves no partial files.
// - Thread-safety: main thread.

#pragma once

#include "texsynth/SynthImage.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class TitleAnimRenderer;

    // R38's cook bytes of a page (block formats of S14's `ui` at quality normal, no mips):
    // desktop BC7, mobile ASTC 6x6, android_etc2 ETC2 RGBA8, web transcoded to 4x4 blocks
    struct TitleFlipbookBytes {
        uint64_t desktop = 0, mobile = 0, androidEtc2 = 0, web = 0;
        uint64_t of(const std::string& family) const;
        void add(int width, int height);
    };
    // the flipbook budgets (MiB) of Project Settings > Title Art > Animation (§6.9)
    struct TitleFlipbookBudget {
        double desktop = 32.0, mobile = 8.0, androidEtc2 = 8.0, web = 8.0;
        double of(const std::string& family) const;
    };
    const char* const TITLE_FLIPBOOK_FAMILIES[] = {"desktop", "mobile", "android_etc2", "web"};

    // frame times of a flipbook (R16): once 0..ceil(D x fps) inclusive, a loop [0, D); <= 240
    std::vector<double> titleFlipbookFrameTimes(const titleanim::CompiledTitleAnimation& animation);
    double titleFlipbookFps(const titleanim::CompiledTitleAnimation& animation);

    struct TitleFlipbookReport {
        std::string animation, id;
        bool needed = false;            // a track that is not title live (Baked frames)
        int frames = 0, unique = 0, pages = 0;
        int frameWidth = 0, frameHeight = 0;    // a frame's pixels
        titleart::TitleRectI frame;     // canvas pixels it covers
        TitleFlipbookBytes bytes;
        std::vector<std::string> files; // the pages (project-relative)
        titleart::Json toJson() const;
    };

    struct TitleFlipbookResult {
        bool ok = false;
        std::string code, error;
        std::vector<TitleFlipbookReport> animations;
        std::vector<std::filesystem::path> written;
        std::vector<std::string> warnings;
        titleanim::TitleAnimData data;  // what <file>.anim.json holds
        titleart::Json toJson() const;
    };

    struct TitleFlipbookOptions {
        std::vector<std::string> animations;    // names; empty = all
        bool dryRun = false;
        float outputScale = 1.0f;       // the main output's scale (the still's pixels per canvas pixel)
    };

    class TitleFlipbookBakeJob {
    public:
        TitleFlipbookBakeJob(Project* project, std::filesystem::path document, titleart::TitleArtDoc doc, TitleFlipbookOptions options);
        ~TitleFlipbookBakeJob();
        bool step(double budgetMs);
        const TitleFlipbookResult& run();
        bool done() const{ return finished; }
        float progress() const;
        std::string progressText() const;
        void cancel(){ cancelled = true; }
        const TitleFlipbookResult& result() const{ return outcome; }

    private:
        struct Work;
        Project* project = nullptr;
        std::filesystem::path documentPath;
        std::shared_ptr<titleart::TitleArtDoc> doc;
        TitleFlipbookOptions options;
        std::unique_ptr<TitleAnimRenderer> renderer;
        std::unique_ptr<titleanim::TitleAnimationSet> authoredSet;
        std::vector<std::unique_ptr<Work>> work;
        size_t current = 0, frameIndex = 0;
        bool started = false, finished = false, cancelled = false;
        TitleFlipbookResult outcome;
        bool begin();
        bool renderOne();
        bool write();
        void fail(const std::string& code, const std::string& error);
    };

    // <file>.anim.json without frames: every animation's settings, title tracks and markers, the
    // flipbook index kept from the file that is there for animations whose pages are fresh (the
    // S80 bake writes it beside the sidecar)
    titleanim::TitleAnimData titleAnimDataOf(const titleart::TitleArtDoc& doc, const std::filesystem::path& document,
                                             const std::string& sourceHash, uint32_t bakeVersion, std::vector<std::string>* warnings);

}
