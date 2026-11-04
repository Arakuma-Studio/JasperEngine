// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureSynthesis.h"
#include "util/FileUtils.h"

#include "Synthesizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <set>
#include <thread>
#include "object/ui/Image.h"

namespace jasperengine::editor::texsynth {

    namespace {

        constexpr int kMaxSide = 8192;
        constexpr int64_t kMaxPixels = 4096LL * 4096LL;
        constexpr int kMaxExampleSide = kMaxImageSide;
        constexpr double kHighUncertainty = 0.1;
        constexpr double kUncertaintyMapScale = 0.25;

        void addProblem(std::vector<Problem>& problems, const std::string& field, const std::string& message) {
            problems.push_back({field, message});
        }

        std::string fmt(double v) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", v);
            return buffer;
        }

        std::string sizeText(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

        // 0..255 mask value of a pixel of an RGBA8 image
        uint8_t maskValue(const uint8_t* p, MaskChannel channel) {
            switch (channel) {
                case MaskChannel::R: return p[0];
                case MaskChannel::G: return p[1];
                case MaskChannel::B: return p[2];
                case MaskChannel::A: return p[3];
                case MaskChannel::Luma:
                default: return static_cast<uint8_t>(std::lround(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]));
            }
        }

        // upstream's transform_to_guide_map: blurred, grey (empty when cancelled)
        Image toGuideMap(const Image& rgba, const std::atomic<bool>* cancel) {
            Image blurred = blurRgba8(rgba, 2.0f, cancel);
            if (blurred.empty()) return blurred;
            return greyRgba8(blurred);
        }

        bool loadInput(ImageInput& input, const std::string& field, std::vector<Problem>& errors) {
            if (!input.image.empty()) return true;
            std::string error;
            if (!loadImage(input.path, input.image, error, &input.sha1, kMaxImageSide)) {
                addProblem(errors, field, error);
                return false;
            }
            return true;
        }

        std::string megapixels(int64_t pixels) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(pixels) / 1e6);
            return buffer;
        }

        Result synthesizeImpl(Request& request, const Control& control);

    }

    Params presetParams(Quality quality) {
        Params p;
        switch (quality) {
            case Quality::Draft:
                p.nearestNeighbors = 20;
                p.randomSampleLocations = 20;
                p.cauchyDispersion = 1.0f;
                p.backtrackPercent = 0.35f;
                p.backtrackStages = 3;
                break;
            case Quality::High:
                p.nearestNeighbors = 80;
                p.randomSampleLocations = 80;
                p.cauchyDispersion = 1.0f;
                p.backtrackPercent = 0.5f;
                p.backtrackStages = 6;
                break;
            case Quality::Balanced:
            default:
                break;   // upstream's defaults: 50, 50, 1.0, 0.5, 5
        }
        return p;
    }

    Params resolveParams(Quality quality, const ParamOverrides& o) {
        Params p = presetParams(quality);
        if (o.nearestNeighbors) p.nearestNeighbors = *o.nearestNeighbors;
        if (o.randomSampleLocations) p.randomSampleLocations = *o.randomSampleLocations;
        if (o.cauchyDispersion) p.cauchyDispersion = *o.cauchyDispersion;
        if (o.backtrackPercent) p.backtrackPercent = *o.backtrackPercent;
        if (o.backtrackStages) p.backtrackStages = *o.backtrackStages;
        if (o.guideAlpha) p.guideAlpha = *o.guideAlpha;
        if (o.guideDecay) p.guideDecay = *o.guideDecay;
        if (o.borderFraction) p.borderFraction = *o.borderFraction;
        return p;
    }

    const char* toString(Mode mode) {
        switch (mode) {
            case Mode::Extend: return "extend";
            case Mode::Tileable: return "tileable";
            case Mode::MakeTileable: return "make_tileable";
            case Mode::Inpaint: return "inpaint";
            case Mode::Guided: return "guided";
        }
        return "?";
    }

    const char* toString(Quality quality) {
        switch (quality) {
            case Quality::Draft: return "draft";
            case Quality::Balanced: return "balanced";
            case Quality::High: return "high";
        }
        return "?";
    }

    const char* toString(MaskChannel channel) {
        switch (channel) {
            case MaskChannel::Luma: return "luma";
            case MaskChannel::R: return "r";
            case MaskChannel::G: return "g";
            case MaskChannel::B: return "b";
            case MaskChannel::A: return "a";
        }
        return "?";
    }

    const char* toString(Status status) {
        switch (status) {
            case Status::Ok: return "ok";
            case Status::InvalidRequest: return "invalid_request";
            case Status::LoadFailed: return "load_failed";
            case Status::Cancelled: return "cancelled";
            case Status::Failed: return "failed";
        }
        return "?";
    }

    bool parseMode(const std::string& text, Mode& mode) {
        for (Mode m : {Mode::Extend, Mode::Tileable, Mode::MakeTileable, Mode::Inpaint, Mode::Guided}) {
            if (text == toString(m)) {
                mode = m;
                return true;
            }
        }
        if (text == "make-tileable") {
            mode = Mode::MakeTileable;
            return true;
        }
        return false;
    }

    bool parseQuality(const std::string& text, Quality& quality) {
        for (Quality q : {Quality::Draft, Quality::Balanced, Quality::High}) {
            if (text == toString(q)) {
                quality = q;
                return true;
            }
        }
        return false;
    }

    bool parseMaskChannel(const std::string& text, MaskChannel& channel) {
        for (MaskChannel c : {MaskChannel::Luma, MaskChannel::R, MaskChannel::G, MaskChannel::B, MaskChannel::A}) {
            if (text == toString(c)) {
                channel = c;
                return true;
            }
        }
        if (text == "alpha") {
            channel = MaskChannel::A;
            return true;
        }
        return false;
    }

    std::vector<Problem> validateParams(const Params& p) {
        std::vector<Problem> errors;
        auto range = [&](const char* name, double value, double lo, double hi, bool loOpen) {
            const bool ok = std::isfinite(value) && (loOpen ? value > lo : value >= lo) && value <= hi;
            if (!ok) {
                addProblem(errors, std::string("params.") + name,
                           std::string(name) + " is " + fmt(value) + ", must be in " + (loOpen ? "(" : "[") + fmt(lo) + ", " + fmt(hi) + "]");
            }
        };
        range("nearestNeighbors", p.nearestNeighbors, 1, 256, false);
        range("randomSampleLocations", p.randomSampleLocations, 1, 1024, false);
        // 0 would divide by zero in the Cauchy cost
        range("cauchyDispersion", p.cauchyDispersion, 0, 1, true);
        range("backtrackPercent", p.backtrackPercent, 0, 0.95, false);
        range("backtrackStages", p.backtrackStages, 0, 10, false);
        range("guideAlpha", p.guideAlpha, 0, 1, false);
        range("borderFraction", p.borderFraction, 0, 0.5, true);
        return errors;
    }

    bool applyCoordinateMap(const CoordinateMap& map, const std::vector<const Image*>& perExample, Image& out, std::string& error) {
        if (perExample.size() != map.exampleSizes.size()) {
            error = std::to_string(perExample.size()) + " images for " + std::to_string(map.exampleSizes.size()) + " examples";
            return false;
        }
        const Image* first = nullptr;
        for (size_t e = 0; e < perExample.size(); e++) {
            const Image* image = perExample[e];
            if (!image || image->empty()) {
                error = "no image for example " + std::to_string(e);
                return false;
            }
            if (image->width != map.exampleSizes[e].first || image->height != map.exampleSizes[e].second) {
                error = "image " + std::to_string(e) + " is " + sizeText(image->width, image->height) + ", its example " +
                        sizeText(map.exampleSizes[e].first, map.exampleSizes[e].second);
                return false;
            }
            if (first && !image->sameFormat(*first)) {
                error = "image " + std::to_string(e) + " is " + describeFormat(*image) + ", image 0 " + describeFormat(*first);
                return false;
            }
            if (!first) first = image;
        }
        if (!first || map.coords.size() != static_cast<size_t>(map.width) * map.height) {
            error = "empty coordinate map";
            return false;
        }
        out = Image::make(map.width, map.height, first->channels, first->bitDepth);
        const size_t bpp = out.bytesPerPixel();
        for (size_t i = 0; i < map.coords.size(); i++) {
            const SourceCoord& c = map.coords[i];
            std::memcpy(out.data.data() + i * bpp, perExample[c.example]->pixel(c.x, c.y), bpp);
        }
        return true;
    }

    void seamScores(const Image& image, double& seamX, double& seamY) {
        seamX = seamY = 1.0;
        if (image.empty() || image.channels != 4 || image.bitDepth != 8) return;
        const int w = image.width, h = image.height;
        auto step = [&](int x0, int y0, int x1, int y1) {
            const uint8_t* a = image.pixel(x0, y0);
            const uint8_t* b = image.pixel(x1, y1);
            return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) + std::abs(a[3] - b[3]);
        };
        // the step across the wrap edge against the steps between the columns (rows) within
        // `reach` of it on both sides: a seam is a discontinuity its surroundings don't have
        constexpr double eps = 1e-3;
        auto score = [&](int size, int across, bool columns) {
            if (size < 3) return 1.0;
            const int reach = std::clamp(size / 4, 1, 8);
            double edge = 0.0, near = 0.0;
            int pairs = 0;
            for (int t = 0; t < across; t++) {
                edge += columns ? step(size - 1, t, 0, t) : step(t, size - 1, t, 0);
                for (int d = 0; d < reach; d++) {
                    // pairs (d, d + 1) after the edge and (size - 2 - d, size - 1 - d) before it
                    near += columns ? step(d, t, d + 1, t) + step(size - 2 - d, t, size - 1 - d, t)
                                    : step(t, d, t, d + 1) + step(t, size - 2 - d, t, size - 1 - d);
                }
            }
            pairs = 2 * reach;
            edge /= across;
            near /= static_cast<double>(across) * pairs;
            return (edge + eps) / (near + eps);
        };
        seamX = score(w, h, true);
        seamY = score(h, w, false);
    }

    std::string companionSuffix(const std::string& exampleStem, const std::string& companionStem, char& separator) {
        auto isSeparator = [](char c) { return c == '_' || c == '-' || c == '.' || c == ' '; };
        size_t common = 0;
        while (common < exampleStem.size() && common < companionStem.size() && exampleStem[common] == companionStem[common]) common++;
        if (common == companionStem.size()) return std::string();   // the same stem, or a prefix of the example's
        size_t cut = 0;
        separator = '_';
        if (common == exampleStem.size() && common < companionStem.size() && isSeparator(companionStem[common])) {
            // the example's whole stem is the prefix: rock + rock_normal
            separator = companionStem[common];
            cut = common + 1;
        } else {
            for (size_t i = common; i > 0; i--) {
                if (isSeparator(companionStem[i - 1])) {
                    separator = companionStem[i - 1];
                    cut = i;
                    break;
                }
            }
        }
        while (cut < companionStem.size() && isSeparator(companionStem[cut])) cut++;
        return companionStem.substr(cut);
    }

    std::filesystem::path companionOutputPath(const std::filesystem::path& primaryOut, const std::filesystem::path& example,
                                              const std::filesystem::path& companion) {
        char separator = '_';
        const std::string suffix = companionSuffix(FileUtils::pathToUtf8(example.stem()), FileUtils::pathToUtf8(companion.stem()), separator);
        if (suffix.empty()) return std::filesystem::path();
        return primaryOut.parent_path() / editor::FileUtils::pathFromUtf8(FileUtils::pathToUtf8(primaryOut.stem()) + separator + suffix + ".png");
    }

    Result synthesize(Request& request, const Control& control) {
        // nothing escapes: a worker thread would std::terminate on it (and the CLI abort)
        auto failed = [&](const std::string& message) {
            Result result;
            result.status = Status::Failed;
            result.params = resolveParams(request.quality, request.overrides);
            result.seed = request.seed;
            result.errors.push_back({"synthesis", message});
            return result;
        };
        try {
            return synthesizeImpl(request, control);
        } catch (const std::bad_alloc&) {
            return failed("out of memory: try smaller examples or output, fewer companions, or a lower quality (fewer backtrack stages)");
        } catch (const std::exception& e) {
            return failed(std::string("the synthesis failed: ") + e.what());
        } catch (...) {
            return failed("the synthesis failed");
        }
    }

    namespace {

    Result synthesizeImpl(Request& request, const Control& control) {
        const auto started = std::chrono::steady_clock::now();
        const auto cancelled = [&control] { return control.cancel && control.cancel->load(std::memory_order_relaxed); };
        auto cancelledResult = [&](Result& result) {
            result.status = Status::Cancelled;
            result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return result;
        };
        Result result;
        result.params = resolveParams(request.quality, request.overrides);
        result.seed = request.seed;
        unsigned hw = std::thread::hardware_concurrency();
        result.threads = request.threads > 0 ? std::min(request.threads, 256) : static_cast<int>(std::max(1u, hw));
        std::vector<Problem>& errors = result.errors;
        const Params& params = result.params;
        const Mode mode = request.mode;

        // ---- the request's shape
        errors = validateParams(params);
        if (request.threads < 0) addProblem(errors, "threads", "threads must be 0 (all cores) or more");
        if (request.examples.empty()) addProblem(errors, "examples", "at least one example is required");
        if (request.examples.size() > 1024) addProblem(errors, "examples", "at most 1024 examples");
        for (size_t e = 0; e < request.examples.size(); e++) {
            if (!request.examples[e].image.given()) addProblem(errors, "examples[" + std::to_string(e) + "].image", "no image");
        }
        // MakeTileable and Inpaint: a given size is checked against the image once it is loaded
        const bool keepsInput = mode == Mode::MakeTileable || mode == Mode::Inpaint;
        if (mode == Mode::Guided) {
            if (!request.targetGuide.given()) addProblem(errors, "targetGuide", "guided mode needs a target guide");
        } else if (!keepsInput && (request.width <= 0 || request.height <= 0)) {
            addProblem(errors, "width", std::string(toString(mode)) + " mode needs an output size");
        }
        if (mode != Mode::Guided && request.targetGuide.given()) {
            addProblem(errors, "targetGuide", std::string("a target guide is only used in guided mode, not ") + toString(mode));
        }
        if (mode != Mode::Inpaint && request.inpaintMask.image.given()) {
            addProblem(errors, "inpaintMask", std::string("an inpaint mask is only used in inpaint mode, not ") + toString(mode));
        }
        const size_t guided = std::count_if(request.examples.begin(), request.examples.end(), [](const Example& ex) { return ex.guide.given(); });
        if (guided && mode != Mode::Guided) addProblem(errors, "examples.guide", "example guides are only used in guided mode");
        if (guided && guided != request.examples.size()) {
            addProblem(errors, "examples.guide",
                       std::to_string(guided) + " of " + std::to_string(request.examples.size()) +
                           " examples have a guide: give every example a guide, or none (style transfer)");
        }
        std::set<std::string> names;
        for (size_t c = 0; c < request.companions.size(); c++) {
            const Companion& companion = request.companions[c];
            const std::string field = "companions[" + std::to_string(c) + "]";
            if (companion.name.empty()) addProblem(errors, field + ".name", "a companion needs a name");
            else if (companion.name == "primary" || companion.name == "uncertainty") addProblem(errors, field + ".name", "'" + companion.name + "' is reserved");
            else if (!names.insert(companion.name).second) addProblem(errors, field + ".name", "two companions are named '" + companion.name + "'");
            if (companion.images.size() != request.examples.size()) {
                addProblem(errors, field + ".images",
                           "companion '" + companion.name + "' has " + std::to_string(companion.images.size()) + " images for " +
                               std::to_string(request.examples.size()) + " examples (one per example, in order)");
            }
            for (size_t e = 0; e < companion.images.size(); e++) {
                if (!companion.images[e].given()) addProblem(errors, field + ".images[" + std::to_string(e) + "]", "no image");
            }
        }
        if (!errors.empty()) {
            result.status = Status::InvalidRequest;
            return result;
        }

        // ---- every input, by field (examples, their masks and guides, companions, masks, target guide)
        std::vector<std::pair<ImageInput*, std::string>> inputs;
        for (size_t e = 0; e < request.examples.size(); e++) {
            Example& ex = request.examples[e];
            const std::string field = "examples[" + std::to_string(e) + "]";
            inputs.push_back({&ex.image, field + ".image"});
            if (ex.sampleMask.given()) inputs.push_back({&ex.sampleMask, field + ".sampleMask"});
            if (ex.guide.given()) inputs.push_back({&ex.guide, field + ".guide"});
        }
        for (size_t c = 0; c < request.companions.size(); c++) {
            for (size_t e = 0; e < request.companions[c].images.size(); e++) {
                inputs.push_back({&request.companions[c].images[e], "companions[" + std::to_string(c) + "].images[" + std::to_string(e) + "]"});
            }
        }
        if (request.inpaintMask.image.given()) inputs.push_back({&request.inpaintMask.image, "inpaintMask"});
        if (request.targetGuide.given()) inputs.push_back({&request.targetGuide, "targetGuide"});

        // ---- their sizes from the headers: too large is refused before anything is decoded
        int64_t inputPixels = 0;
        for (const auto& [input, field] : inputs) {
            int w = 0, h = 0;
            if (!input->image.empty()) {
                w = input->image.width;
                h = input->image.height;
            } else if (!probeImage(input->path, w, h)) {
                continue;   // unreadable: loading reports it
            }
            if (w > kMaxImageSide || h > kMaxImageSide) {
                addProblem(errors, field, "the image is " + sizeText(w, h) + ", at most " + std::to_string(kMaxImageSide) + " per side (scale it down)");
            }
            inputPixels += static_cast<int64_t>(w) * h;
        }
        if (inputPixels > kMaxInputPixels) {
            addProblem(errors, "examples", "the input images total " + megapixels(inputPixels) + " megapixels, at most " + megapixels(kMaxInputPixels) +
                                               " (e.g. one 8192x8192 example with three companion maps): use smaller images, fewer examples or "
                                               "companions");
        }
        if (!errors.empty()) {
            result.status = Status::InvalidRequest;
            return result;
        }

        // ---- the images
        for (const auto& [input, field] : inputs) {
            if (cancelled()) return cancelledResult(result);
            loadInput(*input, field, errors);
        }
        if (!errors.empty()) {
            result.status = Status::LoadFailed;
            return result;
        }
        if (cancelled()) return cancelledResult(result);

        // ---- sizes
        for (size_t e = 0; e < request.examples.size(); e++) {
            const Example& ex = request.examples[e];
            const std::string field = "examples[" + std::to_string(e) + "]";
            const Image& img = ex.image.image;
            if (img.width > kMaxExampleSide || img.height > kMaxExampleSide) {
                addProblem(errors, field + ".image", "example is " + sizeText(img.width, img.height) + ", at most " + std::to_string(kMaxExampleSide) + " per side");
            }
            if (ex.sampleMask.given() && (ex.sampleMask.image.width != img.width || ex.sampleMask.image.height != img.height)) {
                addProblem(errors, field + ".sampleMask",
                           "sample mask is " + sizeText(ex.sampleMask.image.width, ex.sampleMask.image.height) + ", its example " + sizeText(img.width, img.height));
            }
        }
        for (size_t c = 0; c < request.companions.size(); c++) {
            const Companion& companion = request.companions[c];
            for (size_t e = 0; e < companion.images.size(); e++) {
                const Image& img = companion.images[e].image;
                const Image& ex = request.examples[e].image.image;
                if (img.width != ex.width || img.height != ex.height) {
                    addProblem(errors, "companions[" + std::to_string(c) + "].images[" + std::to_string(e) + "]",
                               "companion '" + companion.name + "' is " + sizeText(img.width, img.height) + " but its example is " + sizeText(ex.width, ex.height) +
                                   ": companions must match their example's size exactly (resampling would blend normals and heights; resize it beforehand)");
                }
            }
        }
        const Image& first = request.examples[0].image.image;
        int width = request.width, height = request.height;
        if (keepsInput) {
            if ((width || height) && (width != first.width || height != first.height)) {
                addProblem(errors, "width", std::string(toString(mode)) + " keeps the image's size (" + sizeText(first.width, first.height) +
                                                "): leave the output size out");
            }
            width = first.width;
            height = first.height;
        } else if (mode == Mode::Guided && (width <= 0 || height <= 0)) {
            width = request.targetGuide.image.width;
            height = request.targetGuide.image.height;
        }
        if (width < 1 || height < 1 || width > kMaxSide || height > kMaxSide || static_cast<int64_t>(width) * height > kMaxPixels) {
            addProblem(errors, "width", "output size " + sizeText(width, height) + ": each side 1.." + std::to_string(kMaxSide) + ", at most 4096x4096 pixels");
        }
        if (mode == Mode::Inpaint) {
            const Image& mask = request.inpaintMask.image.image;
            if (request.inpaintMask.image.given() && (mask.width != first.width || mask.height != first.height)) {
                addProblem(errors, "inpaintMask", "mask is " + sizeText(mask.width, mask.height) + ", the image " + sizeText(first.width, first.height));
            }
            if (!request.inpaintMask.image.given() && first.channels != 2 && first.channels != 4) {
                addProblem(errors, "inpaintMask", "examples[0] has no alpha channel to inpaint: give a mask image");
            }
        }
        if (!errors.empty()) {
            result.status = Status::InvalidRequest;
            return result;
        }
        result.width = width;
        result.height = height;
        const size_t pixels = static_cast<size_t>(width) * height;

        // ---- the job
        detail::CoreJob job;
        job.width = width;
        job.height = height;
        job.tiling = mode == Mode::Tileable || mode == Mode::MakeTileable;
        job.params = params;
        job.seed = request.seed;
        job.threads = result.threads;
        job.pyramidLevels = std::max(1, params.backtrackStages);

        // kept pixels (Inpaint, MakeTileable)
        std::vector<uint8_t> regenerate;   // examples[0]-sized, 1 = synthesized
        if (mode == Mode::Inpaint) {
            regenerate.assign(pixels, 0);
            const bool own = !request.inpaintMask.image.given();
            const Image maskRgba = toRgba8(own ? first : request.inpaintMask.image.image);
            const MaskChannel channel = own ? MaskChannel::A : request.inpaintMask.channel;
            for (size_t i = 0; i < pixels; i++) {
                const bool high = maskValue(maskRgba.data.data() + i * 4, channel) >= 128;
                // a mask image: white is regenerated; the image's own alpha: transparent is
                bool regen = own ? !high : high;
                if (request.inpaintMask.invert) regen = !regen;
                regenerate[i] = regen ? 1 : 0;
            }
        } else if (mode == Mode::MakeTileable) {
            regenerate.assign(pixels, 0);
            const int bandX = std::clamp(static_cast<int>(std::lround(params.borderFraction * width)), 1, std::max(1, width / 2));
            const int bandY = std::clamp(static_cast<int>(std::lround(params.borderFraction * height)), 1, std::max(1, height / 2));
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    regenerate[static_cast<size_t>(y) * width + x] = (x < bandX || x >= width - bandX || y < bandY || y >= height - bandY) ? 1 : 0;
                }
            }
        }
        if (!regenerate.empty()) {
            job.locked.resize(pixels);
            size_t regenCount = 0;
            for (size_t i = 0; i < pixels; i++) {
                job.locked[i] = regenerate[i] ? 0 : 1;
                regenCount += regenerate[i];
            }
            if (regenCount == 0) result.warnings.push_back("the mask selects no pixel: the output is the input");
        }

        // target guide, then each example's levels, sample mask and guide (a cancel is checked
        // between every step: each is at most one image's conversion, blur or resize)
        const bool guidedMode = mode == Mode::Guided;
        if (guidedMode) {
            if (cancelled()) return cancelledResult(result);
            Image targetGuide = resizeRgba8(toRgba8(request.targetGuide.image), width, height);
            if (request.targetGuide.image.width != width || request.targetGuide.image.height != height) {
                result.warnings.push_back("the target guide (" + sizeText(request.targetGuide.image.width, request.targetGuide.image.height) +
                                          ") was resized to the output size " + sizeText(width, height));
            }
            if (guided == 0) targetGuide = toGuideMap(targetGuide, control.cancel);   // style transfer: both sides are blurred grey maps
            if (cancelled() || !detail::Pyramid::build(std::move(targetGuide), job.pyramidLevels, job.targetGuideLevels, control.cancel)) {
                return cancelledResult(result);
            }
        }
        // the target guide at the output size (its pyramid's last level): the examples' guides match its histogram
        const Image& targetGuide = job.targetGuideLevels.full;
        bool anySample = false;
        for (size_t e = 0; e < request.examples.size(); e++) {
            if (cancelled()) return cancelledResult(result);
            const Example& ex = request.examples[e];
            detail::CoreExample core;
            Image rgba = toRgba8(ex.image.image);
            core.width = rgba.width;
            core.height = rgba.height;
            core.sampleable = ex.sampleable;
            if (ex.sampleMask.given()) {
                const Image mask = toRgba8(ex.sampleMask.image);
                core.allowed.resize(rgba.pixelCount());
                for (size_t i = 0; i < core.allowed.size(); i++) core.allowed[i] = maskValue(mask.data.data() + i * 4, MaskChannel::Luma) >= 128 ? 1 : 0;
            }
            if (e == 0 && mode == Mode::Inpaint) {
                // never copy what is being replaced
                if (core.allowed.empty()) core.allowed.assign(rgba.pixelCount(), 1);
                for (size_t i = 0; i < core.allowed.size(); i++) core.allowed[i] = core.allowed[i] && !regenerate[i];
            }
            if (guidedMode) {
                if (cancelled()) return cancelledResult(result);
                Image guide;
                if (ex.guide.given()) {
                    guide = toRgba8(ex.guide.image);
                    if (guide.width != rgba.width || guide.height != rgba.height) {
                        result.warnings.push_back("examples[" + std::to_string(e) + "].guide was resized to its example's size");
                        guide = resizeRgba8(guide, rgba.width, rgba.height);
                    }
                } else {
                    // upstream: the example as a blurred grey map, its histogram matched to the target guide's
                    guide = toGuideMap(rgba, control.cancel);
                    if (cancelled()) return cancelledResult(result);
                    matchHistogramRgba8(guide, targetGuide);
                }
                if (cancelled() || !detail::Pyramid::build(std::move(guide), job.pyramidLevels, core.guideLevels, control.cancel)) {
                    return cancelledResult(result);
                }
            }
            // the example's levels last: its RGBA copy moves into the pyramid
            if (cancelled() || !detail::Pyramid::build(std::move(rgba), job.pyramidLevels, core.levels, control.cancel)) return cancelledResult(result);
            if (core.sampleable) {
                const bool some = core.allowed.empty() || std::find(core.allowed.begin(), core.allowed.end(), 1) != core.allowed.end();
                if (!some && ex.sampleMask.given()) {
                    result.warnings.push_back("examples[" + std::to_string(e) + "]: the sample mask excludes every pixel");
                }
                anySample = anySample || some;
            }
            job.examples.push_back(std::move(core));
        }
        if (!anySample) {
            addProblem(errors, "examples", "no example pixel may be copied (sample masks, the inpaint mask or sampleable=false exclude them all)");
            result.status = Status::InvalidRequest;
            return result;
        }

        // ---- synthesis
        detail::CoreResult core;
        if (!detail::runSynthesis(job, control, core)) return cancelledResult(result);
        result.stageSeconds = core.stageSeconds;
        result.coordinates.width = width;
        result.coordinates.height = height;
        result.coordinates.coords = std::move(core.coords);
        for (const Example& ex : request.examples) result.coordinates.exampleSizes.push_back({ex.image.image.width, ex.image.image.height});
        result.uncertainty = std::move(core.uncertainty);

        // ---- outputs: texels copied along the coordinate map, in the widest format of their sources
        auto transfer = [&](const std::vector<const Image*>& sources, const std::string& what, Image& out) {
            int channels = 4, depth = 8;
            unionFormat(sources, channels, depth);
            std::vector<Image> widened(sources.size());
            std::vector<const Image*> pointers;
            bool changed = false;
            for (size_t i = 0; i < sources.size(); i++) {
                widenTo(*sources[i], channels, depth, widened[i]);
                changed = changed || !sources[i]->sameFormat(widened[i]);
                pointers.push_back(&widened[i]);
            }
            if (changed) {
                result.warnings.push_back(what + ": the examples' images differ in format, written as " + describeFormat(widened[0]) +
                                          " (widened losslessly)");
            }
            std::string error;
            if (!applyCoordinateMap(result.coordinates, pointers, out, error)) addProblem(errors, what, error);
        };
        {
            std::vector<const Image*> sources;
            for (const Example& ex : request.examples) sources.push_back(&ex.image.image);
            transfer(sources, "primary", result.primary);
        }
        for (const Companion& companion : request.companions) {
            std::vector<const Image*> sources;
            for (const ImageInput& input : companion.images) sources.push_back(&input.image);
            CompanionOutput out;
            out.name = companion.name;
            transfer(sources, "companion '" + companion.name + "'", out.image);
            result.companions.push_back(std::move(out));
        }
        if (!errors.empty()) {
            result.status = Status::InvalidRequest;
            return result;
        }

        // ---- metrics
        Metrics& metrics = result.metrics;
        metrics.highUncertaintyThreshold = kHighUncertainty;
        double sum = 0.0;
        int64_t high = 0;
        for (size_t i = 0; i < pixels; i++) {
            if (!job.locked.empty() && job.locked[i]) {
                metrics.lockedPixels++;
                continue;
            }
            metrics.synthesizedPixels++;
            sum += result.uncertainty[i];
            if (result.uncertainty[i] > kHighUncertainty) high++;
        }
        if (metrics.synthesizedPixels > 0) {
            metrics.meanUncertainty = sum / static_cast<double>(metrics.synthesizedPixels);
            metrics.highUncertaintyFraction = static_cast<double>(high) / static_cast<double>(metrics.synthesizedPixels);
        }
        seamScores(core.rgba, metrics.seamScoreX, metrics.seamScoreY);
        metrics.seamScore = std::max(metrics.seamScoreX, metrics.seamScoreY);
        result.uncertaintyMap = Image::make(width, height, 1, 8);
        for (size_t i = 0; i < pixels; i++) {
            const double v = std::min(1.0, result.uncertainty[i] / kUncertaintyMapScale);
            result.uncertaintyMap.data[i] = static_cast<uint8_t>(std::lround(v * 255.0));
        }

        result.status = Status::Ok;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return result;
    }

    }

}
