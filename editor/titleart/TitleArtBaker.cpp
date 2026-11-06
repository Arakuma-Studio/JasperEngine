// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtBaker.h"

#include "EditorVersion.h"
#include "Out.h"
#include "Project.h"
#include "TitleArtDocument.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetIndex.h"
#include "asset/AssetMeta.h"
#include "asset/Guid.h"
#include "io/watch/FileWatchService.h"
#include "render/SystemRender.h"
#include "texsynth/SynthesisRecipe.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureImportSettings.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/AssetDatabase.h"
#include "util/ProjectFileWrites.h"
#include "util/Util.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;
using titleart::TitleRectI;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

    std::string str(const TJson& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return (it != object.end() && it->is_string()) ? it->get<std::string>() : fallback;
    }

    int nextPowerOfTwo(int v) {
        int p = 1;
        while (p < v && p < (1 << 14)) p <<= 1;
        return p;
    }

    // the meta of a new baked PNG: S14's texture import defaults for UI art (R4, R6 traps)
    std::string newPngMeta(int width, int height) {
        AssetMeta meta;
        meta.guid = Guid::parse(AssetDatabase::generateGuid());
        TextureImportSettings settings;
        settings.type = TextureImportType::UI;
        settings.quality = TextureCookQuality::High;
        settings.mipmaps = true;
        settings.maxSize = nextPowerOfTwo(std::max(width, height));
        YAML::Node import(YAML::NodeType::Map);
        import["textureImporter"] = settings.serialize();
        YAML::Emitter out;
        out << YAML::Flow << import;
        meta.setSection("import", out.c_str());
        return meta.serialize();
    }

    std::string guidOfMeta(const fs::path& metaPath, const std::string& stagedText) {
        std::string text = stagedText;
        if (text.empty() && !assetio::readFile(metaPath, text)) return {};
        AssetMeta meta;
        if (!AssetMeta::parse(text, meta)) return {};
        return meta.guid.str();
    }

    // the pixels of `src` covering `frame` (canvas px) at `scale`, in an image of outW x outH
    // (transparent beyond the source)
    texsynth::Image cutFrame(const TitleRenderedImage& src, const TitleRectI& frame, double scale, int outW, int outH) {
        texsynth::Image out = texsynth::Image::make(outW, outH, 4, src.image.bitDepth);
        if (src.image.empty() || src.frame.empty()) return out;
        const double srcScaleX = static_cast<double>(src.image.width) / src.frame.width;
        const double srcScaleY = static_cast<double>(src.image.height) / src.frame.height;
        const size_t pixel = out.bytesPerPixel();
        const bool sameScale = std::abs(srcScaleX - scale) < 1e-6 && std::abs(srcScaleY - scale) < 1e-6;
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                int sx, sy;
                if (sameScale) {
                    sx = x + static_cast<int>(std::lround((frame.x - src.frame.x) * scale));
                    sy = y + static_cast<int>(std::lround((frame.y - src.frame.y) * scale));
                } else {
                    // nearest sample (only when a frame render is impossible: never in practice)
                    sx = static_cast<int>(std::floor((frame.x + (x + 0.5) / scale - src.frame.x) * srcScaleX));
                    sy = static_cast<int>(std::floor((frame.y + (y + 0.5) / scale - src.frame.y) * srcScaleY));
                }
                if (sx < 0 || sy < 0 || sx >= src.image.width || sy >= src.image.height) continue;
                std::copy_n(src.image.pixel(sx, sy), pixel, out.pixel(x, y));
            }
        }
        return out;
    }

    texsynth::Image alphaAsGrey(const texsynth::Image& rgba) {
        texsynth::Image grey = texsynth::Image::make(rgba.width, rgba.height, 1, 8);
        for (int y = 0; y < rgba.height; y++) {
            for (int x = 0; x < rgba.width; x++) {
                const uint8_t* p = rgba.pixel(x, y);
                grey.pixel(x, y)[0] = rgba.bitDepth == 16 ? static_cast<uint8_t>(reinterpret_cast<const uint16_t*>(p)[3] >> 8) : p[3];
            }
        }
        return grey;
    }

    // canvas pixels a rendered image's alpha covers
    TitleRectI canvasBounds(const TitleRenderedImage& image) {
        if (image.alphaBounds.empty() || image.frame.empty() || image.image.empty()) return {};
        const double sx = static_cast<double>(image.frame.width) / image.image.width;
        const double sy = static_cast<double>(image.frame.height) / image.image.height;
        const int x0 = image.frame.x + static_cast<int>(std::floor(image.alphaBounds.x * sx));
        const int y0 = image.frame.y + static_cast<int>(std::floor(image.alphaBounds.y * sy));
        const int x1 = image.frame.x + static_cast<int>(std::ceil((image.alphaBounds.x + image.alphaBounds.width) * sx));
        const int y1 = image.frame.y + static_cast<int>(std::ceil((image.alphaBounds.y + image.alphaBounds.height) * sy));
        return {x0, y0, x1 - x0, y1 - y0};
    }

    TitleRectI unite(const TitleRectI& a, const TitleRectI& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    TJson rect(const TitleRectI& r) {
        return TJson::array({r.x, r.y, r.width, r.height});
    }

}

struct TitleArtBakeJob::OutputWork {
    TJson output;
    std::string id;
    titleart::OutputContent content = titleart::OutputContent::All;
    std::string contentName = "all";
    std::vector<std::string> layers;
    double scale = 1.0;
    bool trim = true;
    double padding = 4.0;
    int roundTo = 4;
    int bitDepth = 8;
    bool alphaBleed = true;
    int maxSize = 4096;
    bool write = false;
    // the content render over the canvas, and the frame render when the scale differs
    float renderScale = 1.0f;
    TitleRenderedImage full;
    TitleRectI contentBounds;
    double finalScale = 1.0;
    bool needsFrameRender = false;
    TitleRenderedImage framed;
    int width = 0, height = 0;
};

// ==================================================================== results

TJson TitleBakeFile::toJson() const {
    TJson j = TJson::object();
    j["output"] = output;
    j["path"] = path;
    if (!guid.empty()) j["guid"] = guid;
    j["size"] = TJson::array({width, height});
    j["frame"] = rect(frame);
    j["scale"] = scale;
    j["bytes"] = bytes;
    j["vram_estimate"] = vramEstimate;
    j["status"] = status;
    return j;
}

TJson TitleBakeResult::toJson() const {
    TJson j = TJson::object();
    j["ok"] = ok;
    if (!error.empty()) j["error"] = error;
    if (!code.empty()) j["code"] = code;
    TJson list = TJson::array();
    for (const TitleBakeFile& f : files) list.push_back(f.toJson());
    j["files"] = std::move(list);
    if (!frame.empty()) j["frame"] = rect(frame);
    j["warnings"] = warnings;
    return j;
}

// ==================================================================== the job

TitleArtBakeJob::TitleArtBakeJob(Project* project, fs::path document, titleart::TitleArtDoc snapshot, TitleBakeOptions options)
    : project(project), documentPath(std::move(document)), doc(std::make_shared<const titleart::TitleArtDoc>(std::move(snapshot))),
      options(std::move(options)) {
}

TitleArtBakeJob::~TitleArtBakeJob() = default;

void TitleArtBakeJob::fail(const std::string& code, const std::string& message) {
    outcome.ok = false;
    outcome.code = code;
    outcome.error = message;
    finished = true;
    rendering.reset();
}

void TitleArtBakeJob::cancel() {
    cancelled = true;
}

float TitleArtBakeJob::progress() const {
    if (finished) return 1.0f;
    size_t total = 0, done = 0;
    for (const auto& w : work) {
        total += 1 + (w->needsFrameRender ? 1 : 0);
        done += (w->full.image.empty() ? 0 : 1) + (w->framed.image.empty() ? 0 : 1);
    }
    total += 1;     // the write
    float inRender = rendering ? rendering->progress() : 0.0f;
    return std::min(0.99f, (static_cast<float>(done) + inRender) / static_cast<float>(std::max<size_t>(total, 1)));
}

std::string TitleArtBakeJob::progressText() const {
    if (finished) return outcome.ok ? "baked" : outcome.error;
    if (phase == 3) return "writing";
    if (current < work.size()) return "rendering " + work[current]->id;
    return "baking";
}

bool TitleArtBakeJob::setUp() {
    status = titleArtStatusOf(project, documentPath, *doc);
    const TJson& outputs = doc->root.contains("outputs") ? doc->root["outputs"] : TJson::array();
    std::vector<std::string> ids;
    for (const TJson& o : outputs) ids.push_back(str(o, "id"));
    for (const std::string& wanted : options.outputs) {
        if (std::find(ids.begin(), ids.end(), wanted) == ids.end()) {
            std::string valid;
            for (const std::string& id : ids) valid += (valid.empty() ? "" : ", ") + id;
            fail("bad_output", "no output '" + wanted + "' (outputs: " + valid + ")");
            return false;
        }
    }
    bool anyToWrite = false;
    for (size_t i = 0; i < outputs.size(); i++) {
        auto w = std::make_unique<OutputWork>();
        const TJson& o = outputs[i];
        w->output = o;
        w->id = ids[i];
        w->contentName = str(o, "content", "all");
        titleart::outputContentFromName(w->contentName, w->content);
        if (o.contains("layers") && o["layers"].is_array()) {
            for (const TJson& l : o["layers"]) if (l.is_string()) w->layers.push_back(l.get<std::string>());
        }
        w->scale = o.value("scale", 1.0);
        w->trim = o.value("trim", true);
        w->padding = o.value("padding", 4.0);
        w->roundTo = o.value("round_to", 4);
        w->bitDepth = w->content == titleart::OutputContent::Mask ? 8 : o.value("bit_depth", 8);
        w->alphaBleed = o.value("alpha_bleed", true);
        w->maxSize = o.value("max_size", 4096);
        const bool selected = options.outputs.empty() || std::find(options.outputs.begin(), options.outputs.end(), w->id) != options.outputs.end();
        bool fresh = false;
        for (const TitleOutputState& s : status.outputs) {
            if (s.id == w->id) fresh = s.status == TitleOutputStatus::Fresh;
        }
        w->write = selected && (options.force || !fresh);
        anyToWrite = anyToWrite || w->write;
        work.push_back(std::move(w));
    }
    if (!anyToWrite) {
        // everything asked for is fresh: nothing to render
        outcome.ok = true;
        for (const auto& w : work) {
            const bool selected = options.outputs.empty() || std::find(options.outputs.begin(), options.outputs.end(), w->id) != options.outputs.end();
            if (!selected) continue;
            TitleBakeFile file;
            file.output = w->id;
            const fs::path png = titleArtOutputPath(documentPath, w->output);
            file.path = project ? project->normalizeToProjectRelative(png).generic_string() : png.generic_string();
            texsynth::probeImage(png, file.width, file.height);
            std::error_code ec;
            file.bytes = static_cast<size_t>(fs::file_size(png, ec));
            file.scale = w->scale;
            file.status = "fresh";
            if (project) {
                AssetDatabase::AssetInfo info;
                if (project->getAssetDatabase().getInfo(fs::path(file.path), info)) file.guid = info.guid.str();
            }
            outcome.files.push_back(std::move(file));
        }
        finished = true;
        return false;
    }
    phase = 1;
    current = 0;
    return true;
}

bool TitleArtBakeJob::startRender(OutputWork& output, bool framePhase) {
    titleart::RenderRequest request;
    request.doc = doc;
    request.content = output.content;
    request.contentLayers = output.layers;
    request.outputBitDepth = output.bitDepth;
    const int canvasSide = std::max(doc->canvasWidth(), doc->canvasHeight());
    const float renderLimit = static_cast<float>(titleart::RendererLimits{}.maxSide) / static_cast<float>(std::max(canvasSide, 1));
    if (framePhase) {
        request.pxScale = static_cast<float>(output.finalScale);
        request.region = outcome.frame;
    } else {
        output.renderScale = std::min(static_cast<float>(output.scale), renderLimit);
        request.pxScale = output.renderScale;
    }
    std::string error;
    rendering = TitleArtRenderHost::editor().begin(project, std::move(request), error);
    if (!rendering) {
        fail("render_failed", error);
        return false;
    }
    return true;
}

bool TitleArtBakeJob::advanceRender(double budgetMs, bool framePhase) {
    // the next output this phase renders
    while (current < work.size()) {
        OutputWork& w = *work[current];
        const bool wanted = framePhase ? (w.write && w.needsFrameRender) : true;
        const bool have = framePhase ? !w.framed.image.empty() : !w.full.image.empty();
        if (wanted && !have) break;
        current++;
    }
    if (current >= work.size()) return true;
    OutputWork& w = *work[current];
    if (!rendering && !startRender(w, framePhase)) return false;
    titleart::StepBudget budget;
    budget.budgetUs = static_cast<uint32_t>(std::max(1.0, budgetMs * 1000.0));
    rendering->step(budget);
    if (rendering->failed()) {
        fail("render_failed", "rendering output '" + w.id + "' failed: " + rendering->error());
        return false;
    }
    if (!rendering->done()) return false;
    TitleRenderedImage& target = framePhase ? w.framed : w.full;
    std::string error;
    if (!TitleArtRenderHost::readBack(*rendering, w.bitDepth, target, error)) {
        fail(error.rfind("bit_depth_unsupported", 0) == 0 ? "bit_depth_unsupported" : "readback_failed",
             "output '" + w.id + "': " + error);
        return false;
    }
    if (const titleart::RendererStats* stats = TitleArtRenderHost::editor().stats()) target.pathUsed = stats->path ? stats->path : "";
    target.pxScale = framePhase ? static_cast<float>(w.finalScale) : w.renderScale;
    // a renderer that leaves the frame unset rendered what was asked: the region, else the canvas
    if (target.frame.empty()) target.frame = framePhase ? outcome.frame : TitleRectI{0, 0, doc->canvasWidth(), doc->canvasHeight()};
    if (!framePhase) w.contentBounds = canvasBounds(w.full);
    for (const titleart::TitleProblem& p : target.problems) {
        if (p.severity != titleart::TitleSeverity::Info) outcome.warnings.push_back(p.code + ": " + p.message);
    }
    rendering.reset();
    current++;
    return current >= work.size();
}

bool TitleArtBakeJob::finishFrame() {
    std::vector<TitleFrameInput> inputs;
    for (const auto& w : work) {
        TitleFrameInput in;
        in.contentBounds = w->contentBounds;
        in.trim = w->trim;
        in.paddingCanvasPx = w->padding / std::max(w->scale, 1e-6);
        in.roundTo = w->roundTo;
        inputs.push_back(in);
    }
    outcome.frame = titleArtBakeFrame(doc->canvasWidth(), doc->canvasHeight(), inputs);
    if (outcome.frame.empty()) {
        outcome.warnings.push_back("output_empty: the title shows nothing; the outputs are the whole canvas, transparent");
        outcome.frame = {0, 0, doc->canvasWidth(), doc->canvasHeight()};
    }
    bool anyFrameRender = false;
    for (auto& w : work) {
        if (!w->write) continue;
        w->finalScale = w->scale;
        titleArtOutputSize(outcome.frame, w->finalScale, w->roundTo, w->width, w->height);
        if (std::max(w->width, w->height) > w->maxSize) {
            const double reduced = w->finalScale * w->maxSize / std::max(w->width, w->height);
            outcome.warnings.push_back("output_downscaled: output '" + w->id + "' would be " + std::to_string(w->width) + " x " +
                                       std::to_string(w->height) + ", over max_size " + std::to_string(w->maxSize) +
                                       "; baked at scale " + std::to_string(reduced));
            w->finalScale = reduced;
            titleArtOutputSize(outcome.frame, w->finalScale, w->roundTo, w->width, w->height);
            // rounding up to round_to may pass max_size by less than round_to: the cap wins
            w->width = std::min(w->width, std::max(w->maxSize, 1));
            w->height = std::min(w->height, std::max(w->maxSize, 1));
        }
        w->needsFrameRender = std::abs(w->finalScale - w->renderScale) > 1e-6;
        anyFrameRender = anyFrameRender || w->needsFrameRender;
    }
    current = 0;
    phase = anyFrameRender ? 2 : 3;
    return true;
}

bool TitleArtBakeJob::write() {
    const std::string docName = documentPath.filename().string();
    const std::string docAssetPath = project ? project->normalizeToAssetsRelative(documentPath).generic_string() : docName;
    std::vector<texsynth::StagedFile> staged;
    std::vector<std::string> stagedMetaText;
    TitleRectI alphaUnion;
    TitleBakeInfo info;
    std::string sidecarText;
    {
        std::string oldText;
        TitleBakeInfo old;
        std::string error;
        if (assetio::readFile(titleArtSidecarPath(documentPath), oldText) && TitleBakeInfo::parse(oldText, old, error)) info.extra = old.extra;
    }
    for (auto& w : work) {
        const fs::path png = titleArtOutputPath(documentPath, w->output);
        TitleBakeInfoOutput entry;
        entry.id = w->id;
        entry.file = titleArtOutputFileName(docName, w->output);
        entry.content = w->contentName;
        if (!w->write) {
            texsynth::probeImage(png, entry.width, entry.height);
            info.outputs.push_back(std::move(entry));
            continue;
        }
        const TitleRenderedImage& source = w->needsFrameRender ? w->framed : w->full;
        texsynth::Image pixels = cutFrame(source, outcome.frame, w->finalScale, w->width, w->height);
        const TitleRectI bounds = TitleArtRenderHost::alphaBoundsOf(pixels);
        if (!bounds.empty()) {
            const TitleRectI canvasRect{outcome.frame.x + static_cast<int>(std::floor(bounds.x / w->finalScale)),
                                        outcome.frame.y + static_cast<int>(std::floor(bounds.y / w->finalScale)),
                                        static_cast<int>(std::ceil(bounds.width / w->finalScale)),
                                        static_cast<int>(std::ceil(bounds.height / w->finalScale))};
            alphaUnion = unite(alphaUnion, canvasRect);
        }
        if (w->content == titleart::OutputContent::Mask) {
            pixels = alphaAsGrey(pixels);
        } else if (w->alphaBleed) {
            bleedAlpha(pixels, options.bleedPx);
        }
        std::vector<uint8_t> encoded;
        if (!texsynth::encodePng(pixels, encoded)) {
            fail("encode_failed", "encoding output '" + w->id + "' failed");
            return false;
        }
        TitleBakeRecipe recipe;
        recipe.sourceGuid = doc->guid();
        recipe.sourcePath = docAssetPath;
        recipe.sourceHash = status.sourceHash;
        recipe.assets = status.assets;
        recipe.bakeVersion = status.bakeVersion;
        recipe.output = w->id;
        recipe.frame = outcome.frame;
        recipe.scale = w->finalScale;
        recipe.bitDepth = pixels.bitDepth;
        recipe.pathUsed = source.pathUsed;
        recipe.backend = SystemRender::getBackendName();
        recipe.gpu = SystemRender::getDeviceName();
        recipe.editorVersion = JASPERENGINE_EDITOR_VERSION;
        std::vector<uint8_t> withRecipe;
        std::string error;
        if (!insertTitleBakeRecipe(encoded, recipe, withRecipe, error)) {
            fail("encode_failed", "the recipe of output '" + w->id + "': " + error);
            return false;
        }
        TitleBakeFile file;
        file.output = w->id;
        file.path = project ? project->normalizeToProjectRelative(png).generic_string() : png.generic_string();
        file.width = pixels.width;
        file.height = pixels.height;
        file.frame = outcome.frame;
        file.bytes = withRecipe.size();
        file.scale = w->finalScale;
        file.vramEstimate = static_cast<size_t>(pixels.width) * pixels.height * 4 * (pixels.bitDepth == 16 ? 2 : 1) * 4 / 3;
        file.status = options.dryRun ? "would_write" : "written";
        const fs::path meta(png.string() + ".meta");
        std::error_code ec;
        std::string metaText;
        if (!fs::exists(meta, ec)) {
            metaText = newPngMeta(pixels.width, pixels.height);
            staged.push_back({meta, std::vector<uint8_t>(metaText.begin(), metaText.end())});
        }
        file.guid = guidOfMeta(meta, metaText);
        staged.push_back({png, std::move(withRecipe)});
        entry.width = pixels.width;
        entry.height = pixels.height;
        info.outputs.push_back(std::move(entry));
        outcome.files.push_back(std::move(file));
    }
    info.frame = outcome.frame;
    info.originX = doc->canvasWidth() / 2.0 - outcome.frame.x;
    info.originY = doc->canvasHeight() / 2.0 - outcome.frame.y;
    info.alphaBounds = alphaUnion;
    info.sourceHash = status.sourceHash;
    info.bakeVersion = status.bakeVersion;
    sidecarText = info.toText();
    staged.push_back({titleArtSidecarPath(documentPath), std::vector<uint8_t>(sidecarText.begin(), sidecarText.end())});

    if (options.dryRun) {
        outcome.ok = true;
        finished = true;
        return true;
    }
    {
        std::vector<std::unique_ptr<ScopedExpectedWrite>> expected;
        for (const texsynth::StagedFile& f : staged) expected.push_back(std::make_unique<ScopedExpectedWrite>(f.path));
        std::string failedPath, error;
        if (!texsynth::writeFilesAllOrNothing(staged, failedPath, error)) {
            fail("write_failed", "writing " + failedPath + " failed: " + error + " (nothing was written)");
            return false;
        }
        for (size_t i = 0; i < staged.size(); i++) expected[i]->reported(staged[i].path);
    }
    for (const texsynth::StagedFile& f : staged) outcome.written.push_back(f.path);
    if (project) {
        // editor writes are not watcher events: reload the textures and recook them (S14)
        ProjectFileWrites::filesWritten(project, outcome.written);
        if (TextureCookQueue::get().getProject() == project) {
            for (const fs::path& path : outcome.written) {
                if (Util::isImageFile(path.string())) TextureCookQueue::get().sourceChanged(project->normalizeToAssetsRelative(path).generic_string());
            }
        }
        for (TitleBakeFile& file : outcome.files) {
            if (!file.guid.empty()) continue;
            AssetDatabase::AssetInfo assetInfo;
            if (project->getAssetDatabase().getInfo(fs::path(file.path), assetInfo)) file.guid = assetInfo.guid.str();
        }
    }
    outcome.ok = true;
    finished = true;
    return true;
}

bool TitleArtBakeJob::step(double budgetMs) {
    if (finished) return true;
    if (cancelled) {
        fail("cancelled", "the bake was cancelled; nothing was written");
        return true;
    }
    const auto start = Clock::now();
    auto left = [&]() { return budgetMs - std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    do {
        switch (phase) {
            case 0:
                if (!setUp()) return true;
                break;
            case 1:
                if (advanceRender(std::max(0.5, left()), false)) finishFrame();
                break;
            case 2:
                if (advanceRender(std::max(0.5, left()), true)) phase = 3;
                break;
            default:
                write();
                return true;
        }
        if (finished) return true;
    } while (left() > 0.0);
    return finished;
}

const TitleBakeResult& TitleArtBakeJob::run() {
    while (!step(1000.0)) {}
    return outcome;
}

// ==================================================================== helpers

TitleBakeResult editor::bakeTitleArt(Project* project, const fs::path& document, const titleart::TitleArtDoc& doc, const TitleBakeOptions& options) {
    TitleArtBakeJob job(project, document, doc, options);
    return job.run();
}

bool editor::loadTitleArtForBake(Project* project, const fs::path& document, titleart::TitleArtDoc& out, std::string& code, std::string& error) {
    (void)project;
    if (std::shared_ptr<TitleArtDocument> open = TitleArtDocument::find(document)) {
        if (!open->editable()) {
            code = "version_newer";
            error = open->versionError();
            return false;
        }
        if (open->loadError().empty()) {
            out = open->doc();
            return true;
        }
    }
    std::string text;
    if (!assetio::readFile(document, text)) {
        code = "not_found";
        error = "cannot read " + document.generic_string();
        return false;
    }
    titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
    if (!parsed.ok) {
        code = parsed.error.code.empty() ? "parse_failed" : parsed.error.code;
        error = parsed.error.toString();
        return false;
    }
    out = std::move(parsed.doc);
    return true;
}

TitleBakeStatus editor::titleArtStatusOf(Project* project, const fs::path& document, const titleart::TitleArtDoc& doc) {
    if (project) {
        if (const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project)) {
            return titleArtBakeStatus(document, doc, *resolver);
        }
    }
    EditorTitleAssetResolver none(nullptr, document.parent_path(), document.parent_path());
    return titleArtBakeStatus(document, doc, none);
}

// ==================================================================== Auto Bake

TitleArtAutoBake& TitleArtAutoBake::get() {
    static TitleArtAutoBake instance;
    return instance;
}

void TitleArtAutoBake::documentChanged(Project* project, const fs::path& document) {
    if (!project || !project->getTitleArtSettings().autoBake) return;
    pending[document.lexically_normal().string()] = Clock::now() + std::chrono::milliseconds(DEBOUNCE_MS);
}

void TitleArtAutoBake::assetChanged(Project* project, const fs::path& asset) {
    if (!project || !project->getTitleArtSettings().autoBake) return;
    const std::string relative = project->normalizeToProjectRelative(asset).generic_string();
    const AssetDatabase::Resolution resolution = project->getAssetDatabase().lookupPath(relative);
    const std::vector<DepEdge> edges =
        project->getAssetIndex().referencesTo(resolution.found() ? resolution.guid : Guid(), relative, false, 4096);
    for (const DepEdge& edge : edges) {
        if (edge.kind != DepEdgeKind::TitleArtAsset) continue;
        const std::string referrer = project->getAssetIndex().referrerPath(edge.referrer);
        if (referrer.empty() || !Util::isTitleArtFile(referrer)) continue;
        documentChanged(project, project->getProjectPath() / fs::path(referrer));
    }
}

void TitleArtAutoBake::requestBake(Project* project, const fs::path& document, std::vector<std::string> outputs, bool force) {
    if (!project) return;
    for (const Asked& a : asked) {
        if (a.document == document.lexically_normal() && a.outputs == outputs && a.force == force) return;
    }
    asked.push_back({document.lexically_normal(), std::move(outputs), force});
}

bool TitleArtAutoBake::baking(const fs::path& document) const {
    const fs::path normal = document.lexically_normal();
    if (running && running->document().lexically_normal() == normal) return true;
    for (const Asked& a : asked) if (a.document == normal) return true;
    return false;
}

void TitleArtAutoBake::tick(Project* project) {
    if (!project) return;
    if (!running && !asked.empty()) {
        Asked next = std::move(asked.front());
        asked.erase(asked.begin());
        titleart::TitleArtDoc doc;
        std::string code, error;
        if (!loadTitleArtForBake(project, next.document, doc, code, error)) {
            failure = next.document.generic_string() + ": " + error;
            Out::error("Bake of %s failed: %s", next.document.filename().string().c_str(), error.c_str());
            return;
        }
        TitleBakeOptions options;
        options.outputs = std::move(next.outputs);
        options.force = next.force;
        running = std::make_unique<TitleArtBakeJob>(project, next.document, std::move(doc), options);
    }
    if (running) {
        if (running->step(FRAME_BUDGET_MS)) {
            const TitleBakeResult& result = running->result();
            if (!result.ok) {
                failure = running->document().generic_string() + ": " + result.error;
                Out::warning("Auto Bake of %s failed: %s", running->document().filename().string().c_str(), result.error.c_str());
            }
            completed++;
            running.reset();
        }
        return;
    }
    const auto now = Clock::now();
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->second > now) continue;
        const fs::path document(it->first);
        pending.erase(it);
        if (!project->getTitleArtSettings().autoBake) return;
        titleart::TitleArtDoc doc;
        std::string code, error;
        std::error_code ec;
        if (!fs::is_regular_file(document, ec) || !loadTitleArtForBake(project, document, doc, code, error)) return;
        const TitleBakeStatus status = titleArtStatusOf(project, document, doc);
        // only documents that were baked before; a newer version never reaches here
        if (!status.anyBaked() || status.allFresh()) return;
        TitleBakeOptions options;
        options.outputs = status.notFresh();
        running = std::make_unique<TitleArtBakeJob>(project, document, std::move(doc), options);
        return;
    }
}

void TitleArtAutoBake::reset() {
    pending.clear();
    asked.clear();
    running.reset();
}
