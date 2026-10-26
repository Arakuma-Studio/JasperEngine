// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Example-based texture synthesis (editor-only, never shipped in games): a C++ port of Embark
// Studios' texture-synthesis (MIT OR Apache-2.0, NOTICE.md), the multiresolution stochastic
// synthesis of Opara & Stachowiak ("More Like This, Please! Texture Synthesis and Remixing from
// a Single Example"), in the Resynthesizer lineage.
//
// Every output pixel is resolved in a random order: its k nearest already-resolved output pixels
// propose candidates (where they were copied from, shifted by their offset: coherence) and m
// random source locations add more; each candidate's neighbourhood is compared with the output
// neighbourhood by a Cauchy cost ln(1 + d^2 / sigma^2) weighted by exp(-dist^2 / mean dist^2)
// (plus a guide cost in guided mode) and the cheapest wins. Coarse-to-fine backtracking stages
// re-resolve a growing share of the pixels against blurred copies of the examples ("pyramid").
//
// The result is a coordinate map (output pixel -> example pixel), so the same arrangement is
// applied texel for texel to companion maps (normal, roughness, height, ...) in their own bit
// depth and channel count: PBR sets stay consistent. Never rotates or flips (it would break
// normal maps).
//
// Deterministic: identical inputs, parameters and seed give identical bytes whatever the thread
// count. Each stage resolves its pixels in batches; a batch reads only the state committed by the
// batches before it (k-NN grid, coordinate map, colours) and commits after all its pixels are
// resolved, in batch order. Batch sizes depend only on pixel counts and every pixel's random
// numbers on (seed, stage, position), so threads only change the speed.
//
// Usage (the CLI is texsynth/SynthesisCli.cpp, `jasperengine-editor synthesize-texture`):
//
//   texsynth::Request request;
//   request.mode = texsynth::Mode::Tileable;
//   request.examples.push_back(texsynth::Example::fromPath("rock_albedo.png"));
//   request.companions.push_back({"normal", {texsynth::ImageInput::fromPath("rock_normal.png")}});
//   request.width = request.height = 1024;
//   texsynth::Control control;   // progress callback, cancel flag
//   texsynth::Result result = texsynth::synthesize(request, control);   // blocking
//   if (result.status == texsynth::Status::Ok) { result.primary ... result.companions[0].image ... }

#include "SynthImage.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::texsynth {

    enum class Mode {
        Extend,         // a new (non-tiling) texture of any size from 1+ examples
        Tileable,       // a new seamlessly tiling texture of any size from 1+ examples
        MakeTileable,   // examples[0] made seamless: only a band along its edges is re-synthesized
                        // (with wrapping), the rest kept; output size = its size
        Inpaint,        // examples[0]'s masked region regenerated, the rest kept (locked);
                        // output size = its size
        Guided          // examples (+ optional example guides) arranged to follow targetGuide; with
                        // no example guide it is a style transfer (guides made by blur, grey and
                        // histogram match, as upstream)
    };

    enum class Quality { Draft, Balanced, High };

    // The resolved synthesis parameters (a quality preset plus the overrides)
    struct Params {
        // k: resolved output pixels each pixel compares (larger: more global structure). 1..256
        int nearestNeighbors = 50;
        // m: random source locations tried besides the k coherence candidates. 1..1024
        int randomSampleLocations = 50;
        // sigma of the Cauchy cost: near 0 hard seams between copied patches, 1 smooth. (0, 1]
        float cauchyDispersion = 1.0f;
        // share of the pixels each backtracking stage resolves relative to the next: stage s of
        // S resolves backtrackPercent^s of the pixels (the last one all of them). [0, 0.95]
        float backtrackPercent = 0.5f;
        // backtracking stages S (S + 1 stages; also the depth of the blur pyramid, clamped to
        // what the example sizes allow). 0..10
        int backtrackStages = 5;
        // Guide weight w in cost = (1 - w) * example cost + w * guide cost (Guided mode).
        // guideDecay true (default, upstream's behaviour): w = (guideAlpha * (1 - f))^3 where f
        // is the share of pixels resolved before the stage, and w = 0 in the last stage, so the
        // guide sets the layout and the example the detail. guideDecay false: w = guideAlpha in
        // every stage (what upstream documented for 1.0 but never did). [0, 1]
        float guideAlpha = 0.8f;
        bool guideDecay = true;
        // MakeTileable: width of the re-synthesized band on each side of every edge, as a share
        // of the image's width (left/right) and height (top/bottom). (0, 0.5]
        float borderFraction = 0.125f;
    };

    // Raw parameter overrides on top of the quality preset (unset = the preset's value)
    struct ParamOverrides {
        std::optional<int> nearestNeighbors;
        std::optional<int> randomSampleLocations;
        std::optional<float> cauchyDispersion;
        std::optional<float> backtrackPercent;
        std::optional<int> backtrackStages;
        std::optional<float> guideAlpha;
        std::optional<bool> guideDecay;
        std::optional<float> borderFraction;
    };

    // draft (fast preview), balanced (upstream's defaults), high
    Params presetParams(Quality quality);
    Params resolveParams(Quality quality, const ParamOverrides& overrides);
    const char* toString(Mode mode);       // "extend", "tileable", "make_tileable", "inpaint", "guided"
    const char* toString(Quality quality); // "draft", "balanced", "high"
    bool parseMode(const std::string& text, Mode& mode);       // also accepts "make-tileable"
    bool parseQuality(const std::string& text, Quality& quality);

    // An image given by path (loaded by synthesize()) or already decoded (`image` non-empty wins;
    // `path` is then only recorded in the recipe). `sha1` is filled with the file's hash on load.
    struct ImageInput {
        std::filesystem::path path;
        Image image;
        std::string sha1;

        bool given() const { return !path.empty() || !image.empty(); }
        static ImageInput fromPath(const std::filesystem::path& path) { ImageInput in; in.path = path; return in; }
        static ImageInput fromImage(Image image) { ImageInput in; in.image = std::move(image); return in; }
    };

    // Which samples of a mask image are read: the luma of its colour, or one channel
    enum class MaskChannel { Luma, R, G, B, A };
    const char* toString(MaskChannel channel);
    bool parseMaskChannel(const std::string& text, MaskChannel& channel);

    struct Example {
        ImageInput image;        // the primary map (albedo): drives the synthesis
        // Optional sampling mask, the example's size (another size is an error): where its luma
        // is white (>= 128) may be copied, black never. Inpaint also excludes what it regenerates.
        ImageInput sampleMask;
        // Guided mode: this example's guide (resized to the example if its size differs)
        ImageInput guide;
        // false: never copied from (e.g. an inpainted image whose rest should come from other
        // examples); its pixels still count as context, and companions still need an image for it
        bool sampleable = true;

        static Example fromPath(const std::filesystem::path& path) { Example ex; ex.image = ImageInput::fromPath(path); return ex; }
        static Example fromImage(Image image) { Example ex; ex.image = ImageInput::fromImage(std::move(image)); return ex; }
    };

    // A companion map set (normal, roughness, height, ...): one image per example, in the same
    // order; each must have exactly its example's size (an error otherwise: resampling would
    // blend normals and heights, resize them beforehand). Copied texel for texel along the
    // coordinate map, in its own format (8/16-bit, 1-4 channels).
    struct Companion {
        std::string name;               // "normal" (output naming, recipe role)
        std::vector<ImageInput> images; // one per example
    };

    struct InpaintMask {
        // The mask image (examples[0]'s size). Empty: examples[0]'s own alpha, where transparent
        // (< 128) is regenerated.
        ImageInput image;
        MaskChannel channel = MaskChannel::Luma;
        // Mask image: white (>= 128) is regenerated, black kept; invert swaps it (so does it for
        // the alpha form)
        bool invert = false;
    };

    struct Request {
        Mode mode = Mode::Extend;
        std::vector<Example> examples;
        std::vector<Companion> companions;
        InpaintMask inpaintMask;     // Inpaint
        ImageInput targetGuide;      // Guided (resized to the output size)
        // Output size (Extend, Tileable, Guided; 0 = the target guide's size in Guided mode).
        // MakeTileable and Inpaint use examples[0]'s size. 1..8192 each, at most 4096 x 4096 pixels.
        int width = 0;
        int height = 0;
        Quality quality = Quality::Balanced;
        ParamOverrides overrides;
        uint64_t seed = 0;
        int threads = 0;             // 0: hardware concurrency; never changes the output
    };

    struct Progress {
        int stage = 0;               // 0-based
        int stageCount = 0;
        double stageFraction = 0.0;  // 0..1 of the current stage
        double fraction = 0.0;       // 0..1 overall (weighted by the pixels each stage resolves)
        double elapsedSeconds = 0.0;
        double etaSeconds = -1.0;    // estimated seconds left, -1 before there is an estimate
        // A snapshot of the partial output (RGBA8, unresolved pixels transparent), downscaled to
        // Control::previewMaxSize, at most every Control::previewIntervalSeconds; null otherwise.
        // Immutable: it may be kept and read from any thread.
        std::shared_ptr<const Image> preview;
    };

    struct Control {
        // Called on the synthesizing thread between batches, at most every progressIntervalSeconds
        // (and at the start and end of each stage). Keep it cheap; copy what it needs.
        std::function<void(const Progress&)> onProgress;
        double progressIntervalSeconds = 0.1;
        double previewIntervalSeconds = 0.0;   // 0: no previews
        int previewMaxSize = 512;
        // Set to true from any thread: synthesize() returns Status::Cancelled within a batch
        // (milliseconds), or between the steps of loading and preparing the images (each at most
        // one image decode or resize: about a second for an 8192 x 8192 example)
        const std::atomic<bool>* cancel = nullptr;
    };

    // Failed: the synthesis itself could not run (out of memory: Result::errors says so)
    enum class Status { Ok, InvalidRequest, LoadFailed, Cancelled, Failed };
    const char* toString(Status status);

    // Where an output pixel was copied from: example `example`'s pixel (x, y)
    struct SourceCoord {
        uint16_t x = 0;
        uint16_t y = 0;
        uint16_t example = 0;
    };

    struct CoordinateMap {
        int width = 0;
        int height = 0;
        std::vector<SourceCoord> coords;          // width * height, rows top to bottom
        std::vector<std::pair<int, int>> exampleSizes;
    };

    struct Metrics {
        // Uncertainty of a pixel: the neighbourhood mismatch of its final choice expressed as the
        // equivalent per-channel colour difference (0..1 of the RGBA range; x255 for 8-bit
        // levels): sigma * sqrt(exp(c) - 1) for the distance-weighted mean Cauchy cost c per
        // neighbour and channel. Kept (locked) pixels count 0 and are left out of these numbers.
        double meanUncertainty = 0.0;
        double highUncertaintyThreshold = 0.1;  // ~25 levels of 255
        double highUncertaintyFraction = 0.0;   // share of synthesized pixels above the threshold
        // Seam score: the mean RGBA step across the wrap edge (last column -> first column, last
        // row -> first row) divided by the mean step between the adjacent columns (rows) within 8
        // pixels of it on both sides; seamScore = max of the two. ~1 tiles seamlessly (the edge
        // is like its surroundings), > 1.5 a visible seam. Computed for every mode (for
        // Extend/Inpaint/Guided it says how badly the result would tile).
        double seamScoreX = 0.0;
        double seamScoreY = 0.0;
        double seamScore = 0.0;
        int64_t synthesizedPixels = 0;
        int64_t lockedPixels = 0;
    };

    struct Problem {
        std::string field;     // "examples[0].image", "params.cauchyDispersion", "width", ...
        std::string message;
    };

    struct CompanionOutput {
        std::string name;
        Image image;
    };

    struct Result {
        Status status = Status::InvalidRequest;
        std::vector<Problem> errors;
        std::vector<std::string> warnings;

        Params params;                 // resolved (preset + overrides)
        uint64_t seed = 0;
        int threads = 0;               // used
        int width = 0;
        int height = 0;

        // The primary output: examples' texels in the widest of their formats (a single RGB
        // example gives RGB; 16-bit stays 16-bit)
        Image primary;
        std::vector<CompanionOutput> companions;   // in Request::companions order
        CoordinateMap coordinates;
        std::vector<float> uncertainty;            // width * height, see Metrics
        Image uncertaintyMap;                      // 8-bit grey: min(1, uncertainty / 0.25) * 255
        Metrics metrics;
        double seconds = 0.0;
        std::vector<double> stageSeconds;
    };

    // Validates the request (every problem, not only the first), loads the images, synthesizes.
    // Never throws: errors come back in Result::errors with Status::InvalidRequest (or LoadFailed
    // for an unreadable image, Failed when memory ran out). The input images may total at most
    // kMaxInputPixels, checked from their headers before anything is decoded. The images given by path are loaded into the
    // request (ImageInput::image and ::sha1), so makeRecipe() can record their hashes and a
    // second run (another seed) does not decode them again. Blocking: run it on a worker thread
    // in the editor; one request at a time per Request object.
    Result synthesize(Request& request, const Control& control = Control());

    // Limits of the inputs: each side of an image, and every input image together (examples,
    // companions, masks, guides; one 8192 x 8192 example with three companion maps fits)
    inline constexpr int kMaxImageSide = 8192;
    inline constexpr int64_t kMaxInputPixels = 4LL * 8192 * 8192;

    // Only the validation (no image loaded): what synthesize() would refuse for the parameters
    std::vector<Problem> validateParams(const Params& params);

    // The coordinate map applied to one image per example (each its example's size, one format):
    // the texels copied, nothing interpolated. False with `error` otherwise.
    bool applyCoordinateMap(const CoordinateMap& map, const std::vector<const Image*>& perExample, Image& out, std::string& error);

    // Quality numbers of an RGBA8 image alone (seam scores), as in Metrics
    void seamScores(const Image& image, double& seamX, double& seamY);

    // ---- companion naming (the CLI's and the editor's rule)

    // The name a companion map gets from its file name next to its example's: what its stem adds
    // after the longest common prefix that ends at a separator (_ - . space), leading separators
    // dropped. rock_albedo + rock_normal -> "normal", rock_albedo + rock_ao -> "ao",
    // rock + rock_normal_gl -> "normal_gl", albedo + normal -> "normal". `separator` receives
    // the separator before the suffix ('_' when there is none). Empty when the stems are equal.
    std::string companionSuffix(const std::string& exampleStem, const std::string& companionStem, char& separator);

    // Where a companion's output goes by default: next to the primary output, its stem + the
    // separator + the suffix, .png: out rock_tile.png, example rock_albedo.png, companion
    // rock_normal.png -> rock_tile_normal.png. Empty when the suffix is (equal stems).
    std::filesystem::path companionOutputPath(const std::filesystem::path& primaryOut, const std::filesystem::path& example,
                                              const std::filesystem::path& companion);

}
