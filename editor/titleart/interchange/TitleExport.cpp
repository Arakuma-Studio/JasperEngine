// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The frame rules (trim at DEFAULT_TRIM_ALPHA 8 on an enlarged canvas, rounding that centres the
// growth) follow Pachislop src/io/export.ts (alphaBounds, roundFrame, contentFrame) by SafEight,
// used with the author's permission.

#include "TitleExport.h"

#include "EditorVersion.h"
#include "Project.h"
#include "titleart/TitleArtBake.h"
#include "asset/AssetFileWrite.h"
#include "png/PngBandEncoder.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleFit.h"
#include "titleart/TitleTranslations.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/render/TitleRenderJob.h"
#include "util/FileUtils.h"
#include "util/ZipArchive.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <set>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleRectI;
namespace fs = std::filesystem;

namespace {

    std::vector<TitleExportFormat>& formats() {
        static std::vector<TitleExportFormat>* list = new std::vector<TitleExportFormat>();
        return *list;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string stemOf(const fs::path& document) {
        std::string name = document.filename().string();
        const std::string ext = ".titleart";
        if (name.size() > ext.size() && lower(name.substr(name.size() - ext.size())) == ext) name = name.substr(0, name.size() - ext.size());
        return name;
    }

    bool inside(const fs::path& root, const fs::path& path) {
        const fs::path r = root.lexically_normal(), p = path.lexically_normal();
        auto ri = r.begin(), pi = p.begin();
        for (; ri != r.end() && pi != p.end(); ++ri, ++pi) {
            if (ri->empty()) continue;
            if (*ri != *pi) return false;
        }
        return ri == r.end() || (ri->empty() && std::next(ri) == r.end());
    }

    // pixels of an image with alpha >= threshold (8- or 16-bit RGBA), image px; empty when none
    TitleRectI thresholdBounds(const texsynth::Image& image, int threshold) {
        int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
        const int t8 = std::clamp(threshold, 1, 255);
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                const uint8_t* p = image.pixel(x, y);
                const int a = image.bitDepth == 16 ? (reinterpret_cast<const uint16_t*>(p)[3] >> 8) : p[3];
                if (a < t8) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        if (x1 < 0) return {};
        return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }

    bool containsLayer(const Json& layer, const std::string& id) {
        if (layer.value("id", std::string()) == id) return true;
        if (auto children = layer.find("children"); children != layer.end() && children->is_array()) {
            for (const Json& c : *children) if (containsLayer(c, id)) return true;
        }
        return false;
    }

    int nextPow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // the pixels of `src` (covering `src.frame` at its own scale) over `frame` at `scale`, outW x outH
    texsynth::Image cut(const TitleRenderedImage& src, const TitleRectI& frame, double scale, int outW, int outH) {
        texsynth::Image out = texsynth::Image::make(outW, outH, 4, src.image.bitDepth);
        if (src.image.empty()) return out;
        const size_t pixel = out.bytesPerPixel();
        const int dx = (int)std::lround((frame.x - src.frame.x) * scale), dy = (int)std::lround((frame.y - src.frame.y) * scale);
        for (int y = 0; y < outH; y++) {
            const int sy = y + dy;
            if (sy < 0 || sy >= src.image.height) continue;
            for (int x = 0; x < outW; x++) {
                const int sx = x + dx;
                if (sx < 0 || sx >= src.image.width) continue;
                std::copy_n(src.image.pixel(sx, sy), pixel, out.pixel(x, y));
            }
        }
        return out;
    }

    // 16-bit straight sRGB -> 8-bit, rounded (an undithered 8-bit output)
    texsynth::Image to8(const texsynth::Image& image) {
        texsynth::Image out = texsynth::Image::make(image.width, image.height, image.channels, 8);
        const uint16_t* src = reinterpret_cast<const uint16_t*>(image.data.data());
        for (size_t i = 0; i < out.data.size(); i++) out.data[i] = (uint8_t)((src[i] * 255u + 32767u) / 65535u);
        return out;
    }

}

// ==================================================================== result

TitleJson TitleExportResult::toJson() const {
    TitleJson j = TitleJson::object();
    j["ok"] = ok;
    if (!code.empty()) j["code"] = code;
    if (!error.empty()) j["error"] = error;
    j["files"] = files;
    j["frames"] = frames;
    j["warnings"] = warnings;
    for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
    return j;
}

// ==================================================================== registry

void editor::registerTitleExportFormat(TitleExportFormat format) {
    for (TitleExportFormat& f : formats()) {
        if (f.name == format.name) {
            f = std::move(format);
            return;
        }
    }
    formats().push_back(std::move(format));
}

const TitleExportFormat* editor::findTitleExportFormat(const std::string& name) {
    for (const TitleExportFormat& f : formats()) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

const std::vector<std::string>& editor::titleImageOptionKeys() {
    static const std::vector<std::string> keys = {"scale", "trim", "padding", "round_to", "pot", "alpha_threshold", "max_size",
                                                  "registered", "bit_depth", "dither", "alpha", "alpha_bleed", "bleed_px", "metadata"};
    return keys;
}

// ==================================================================== context

void TitleExportContext::render(titleart::RenderRequest renderRequest, std::function<void(TitleRenderedImage&)> done) {
    Step s;
    s.render = std::make_unique<titleart::RenderRequest>(std::move(renderRequest));
    s.done = std::move(done);
    added.push_back(std::move(s));
    rendersTotal++;
}

void TitleExportContext::then(std::function<bool()> step) {
    Step s;
    s.cpu = std::move(step);
    added.push_back(std::move(s));
}

bool TitleExportContext::fail(const std::string& code, const std::string& message) {
    if (!failed) {
        result.ok = false;
        result.code = code;
        result.error = message;
    }
    failed = true;
    return false;
}

void TitleExportContext::warn(const std::string& warning) {
    if (std::find(result.warnings.begin(), result.warnings.end(), warning) == result.warnings.end()) result.warnings.push_back(warning);
}

std::string TitleExportContext::fileName(const std::string& part, const std::string& language, const std::string& extension) const {
    std::string name = stemOf(request.document);
    if (!part.empty()) name += "_" + part;
    if (!language.empty()) name += "." + lower(language);
    return name + "." + extension;
}

bool TitleExportContext::write(const std::string& name, std::string content) {
    if (request.options.value("zip", false)) {
        zipEntries.emplace_back(name, std::move(content));
        return true;
    }
    const fs::path target = request.out / FileUtils::pathFromUtf8(name);
    std::error_code ec;
    if (fs::exists(target, ec) && !request.options.value("overwrite", false)) {
        return fail("output_exists", "'" + name + "' exists in " + request.out.generic_string() + ": pass options.overwrite true to replace it");
    }
    fs::create_directories(target.parent_path(), ec);
    std::string error;
    if (!assetio::writeFile(target, content, assetio::Durability::Document, &error)) return fail("write_failed", "writing " + name + ": " + error);
    const fs::path project = request.project ? request.project->getProjectPath() : fs::path();
    std::error_code rel;
    result.files.push_back(!project.empty() && inside(project, target) ? fs::relative(target, project, rel).generic_string() : target.generic_string());
    return true;
}

bool TitleExportContext::writeFile(const std::string& name, const fs::path& file) {
    std::error_code ec;
    if (request.options.value("zip", false)) {
        std::string content;
        const bool read = assetio::readFile(file, content);
        fs::remove(file, ec);
        if (!read) return fail("write_failed", "reading " + file.generic_string());
        return write(name, std::move(content));
    }
    const fs::path target = request.out / FileUtils::pathFromUtf8(name);
    if (fs::exists(target, ec) && !request.options.value("overwrite", false)) {
        fs::remove(file, ec);
        return fail("output_exists", "'" + name + "' exists in " + request.out.generic_string() + ": pass options.overwrite true to replace it");
    }
    fs::rename(file, target, ec);
    if (ec) {
        fs::remove(file, ec);
        return fail("write_failed", "moving " + name + " into " + request.out.generic_string());
    }
    const fs::path project = request.project ? request.project->getProjectPath() : fs::path();
    std::error_code rel;
    result.files.push_back(!project.empty() && inside(project, target) ? fs::relative(target, project, rel).generic_string() : target.generic_string());
    return true;
}

titleart::TitleArtDoc TitleExportContext::resolved(const std::string& language) const {
    titleart::TitleArtDoc out = titleart::resolveTitleLanguage(request.doc, language);
    const titleart::AssetResolver* assets = TitleArtRenderHost::editor().resolver(request.project);
    if (titleart::titleHasFit(out.root)) titleart::applyTitleFits(out, assets);
    return out;
}

void TitleExportContext::measure(std::shared_ptr<const titleart::TitleArtDoc> doc, const std::string& language, const std::string& isolateLayer,
                                 std::function<void(TitleRenderedImage&, const TitleRectI&)> done) {
    // the canvas grown to every visible layer's reach; full-bleed generators clipped to the canvas
    const int W = doc->canvasWidth(), H = doc->canvasHeight();
    titleart::geometry::TitleGeometry geometry(TitleArtRenderHost::editor().resolver(request.project));
    const double scale = image.scale;
    TitleRectI reach;
    for (const Json& layer : doc->layers()) {
        if (!layer.value("visible", true) || layer.value("guide", false)) continue;
        if (!isolateLayer.empty() && !containsLayer(layer, isolateLayer)) continue;
        titleart::TitleRect r = geometry.reachBounds(layer, *doc, scale);
        if (titleart::layerTypeOf(layer) == titleart::LayerType::Generator) {
            const double x0 = std::max(r.x, -W / 2.0), y0 = std::max(r.y, -H / 2.0);
            const double x1 = std::min(r.x + r.width, W / 2.0), y1 = std::min(r.y + r.height, H / 2.0);
            r = {x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
        }
        if (r.empty()) continue;
        const TitleRectI px{(int)std::floor(r.x + W / 2.0), (int)std::floor(H / 2.0 - (r.y + r.height)), (int)std::ceil(r.width) + 1,
                            (int)std::ceil(r.height) + 1};
        reach = titleart::unionRect(reach, px);
    }
    // at most the canvas plus its own size on each side, and what max_size allows
    const TitleRectI cap{-W, -H, 3 * W, 3 * H};
    reach = reach.empty() ? TitleRectI{0, 0, W, H} : titleart::intersectRect(reach, cap);
    double measureScale = scale;
    const int limit = std::min(image.maxSize, titleart::RendererLimits{}.maxSide);
    if (std::max(reach.width, reach.height) * measureScale > limit) measureScale = (double)limit / std::max(reach.width, reach.height);
    auto measured = std::make_shared<titleart::TitleArtDoc>(*doc);
    Json& background = measured->root["canvas"]["background"];
    const bool opaque = background.is_object() && !background.value("transparent", true);
    if (opaque) background["transparent"] = true;
    titleart::RenderRequest r;
    r.doc = measured;
    r.pxScale = (float)measureScale;
    r.region = reach;
    r.isolateLayer = isolateLayer;
    r.language = language;
    r.outputBitDepth = 8;
    const int threshold = image.alphaThreshold;
    render(std::move(r), [this, done, reach, measureScale, threshold](TitleRenderedImage& img) {
        if (img.frame.empty()) img.frame = reach;
        img.pxScale = (float)measureScale;
        const TitleRectI b = thresholdBounds(img.image, threshold);
        TitleRectI frame;
        if (!b.empty()) {
            frame = {img.frame.x + (int)std::floor(b.x / measureScale), img.frame.y + (int)std::floor(b.y / measureScale),
                     (int)std::ceil((b.x + b.width) / measureScale) - (int)std::floor(b.x / measureScale),
                     (int)std::ceil((b.y + b.height) / measureScale) - (int)std::floor(b.y / measureScale)};
        }
        done(img, frame);
    });
}

TitleRectI TitleExportContext::finishFrame(const TitleRectI& trimmed, double& scale, int* width, int* height) {
    TitleRectI f = trimmed;
    if (!image.trim || f.empty()) f = {0, 0, request.doc.canvasWidth(), request.doc.canvasHeight()};
    const auto build = [&](double s) {
        const int pad = image.trim ? (int)std::ceil(image.padding / s) : 0;
        TitleRectI g{f.x - pad, f.y - pad, f.width + 2 * pad, f.height + 2 * pad};
        // output sizes rounded up to round_to (then a power of two); the growth centred
        int w = (int)std::ceil(g.width * s), h = (int)std::ceil(g.height * s);
        const int m = std::max(1, image.roundTo);
        w = (w + m - 1) / m * m;
        h = (h + m - 1) / m * m;
        if (image.pot) {
            w = nextPow2(w);
            h = nextPow2(h);
        }
        const int cw = (int)std::ceil(w / s), ch = (int)std::ceil(h / s);
        g.x -= (cw - g.width) / 2;
        g.y -= (ch - g.height) / 2;
        g.width = cw;
        g.height = ch;
        return std::make_pair(g, std::make_pair(w, h));
    };
    auto [frame, size] = build(scale);
    if (std::max(size.first, size.second) > image.maxSize) {
        const double reduced = scale * image.maxSize / std::max(size.first, size.second);
        warn("output_downscaled: " + std::to_string(size.first) + " x " + std::to_string(size.second) + " is over max_size " +
             std::to_string(image.maxSize) + "; exported at scale " + std::to_string(reduced));
        scale = reduced;
        std::tie(frame, size) = build(scale);
    }
    if (width) *width = size.first;
    if (height) *height = size.second;
    return frame;
}

void TitleExportContext::renderFrame(std::shared_ptr<const titleart::TitleArtDoc> doc, const std::string& language, const TitleRectI& frame,
                                     double scale, const std::string& isolateLayer, std::function<void(texsynth::Image&)> done, int width,
                                     int height) {
    titleart::RenderRequest r;
    r.doc = doc;
    r.pxScale = (float)scale;
    r.region = frame;
    r.isolateLayer = isolateLayer;
    r.language = language;
    // an undithered 8-bit image is the 16-bit output rounded
    r.outputBitDepth = (image.bitDepth == 16 || !image.dither) ? 16 : 8;
    const int outW = width > 0 ? width : (int)std::lround(frame.width * scale), outH = height > 0 ? height : (int)std::lround(frame.height * scale);
    render(std::move(r), [this, done, frame, scale, outW, outH](TitleRenderedImage& img) {
        texsynth::Image pixels = cutFrame(img, frame, scale, outW, outH);
        done(pixels);
    });
}

texsynth::Image TitleExportContext::cutFrame(TitleRenderedImage& img, const TitleRectI& frame, double scale, int width, int height) const {
    if (img.frame.empty()) img.frame = frame;
    texsynth::Image pixels = cut(img, frame, scale, std::max(1, width), std::max(1, height));
    if (image.bitDepth == 8 && pixels.bitDepth == 16) pixels = to8(pixels);
    finishPixels(pixels);
    return pixels;
}

void TitleExportContext::finishPixels(texsynth::Image& pixels) const {
    if (pixels.channels != 4) return;
    if (image.alpha == "premultiplied") {
        // colour x alpha in the encoded domain (what image editors and premultiplied textures expect)
        const size_t n = (size_t)pixels.width * pixels.height;
        if (pixels.bitDepth == 16) {
            uint16_t* p = reinterpret_cast<uint16_t*>(pixels.data.data());
            for (size_t i = 0; i < n; i++, p += 4) {
                for (int c = 0; c < 3; c++) p[c] = (uint16_t)(((uint32_t)p[c] * p[3] + 32767u) / 65535u);
            }
        } else {
            uint8_t* p = pixels.data.data();
            for (size_t i = 0; i < n; i++, p += 4) {
                for (int c = 0; c < 3; c++) p[c] = (uint8_t)((p[c] * p[3] + 127) / 255);
            }
        }
        return;
    }
    if (image.alphaBleed) {
        bleedAlpha(pixels, image.bleedPx);
        return;
    }
    // Pachislop's output: colour zeroed under alpha 0
    const size_t n = (size_t)pixels.width * pixels.height;
    const size_t bpp = pixels.bytesPerPixel();
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = pixels.data.data() + i * bpp;
        const bool clear = pixels.bitDepth == 16 ? reinterpret_cast<uint16_t*>(p)[3] == 0 : p[3] == 0;
        if (clear) std::fill(p, p + bpp, 0);
    }
}

bool TitleExportContext::encodePng(const texsynth::Image& pixels, const std::string& language, std::vector<uint8_t>& out) {
    TitlePngOptions o;
    o.dpi = request.doc.root.value("dpi", 72.0);
    if (image.metadata) {
        o.recipeKeyword = "jasper.titleart.export";
        TitleJson recipe = TitleJson::object();
        recipe["format"] = "jasperengine.titleart.export";
        recipe["version"] = 1;
        recipe["source"] = request.relative;
        recipe["source_hash"] = titleArtSourceHash(request.doc);
        recipe["bake_version"] = titleart::TITLE_ART_BAKE_VERSION_BASE;
        recipe["language"] = language;
        TitleJson options = request.options;
        options.erase("overwrite");
        recipe["options"] = options;
        recipe["editor_version"] = JASPERENGINE_EDITOR_VERSION;
        o.recipeText = recipe.dump();
    }
    std::string error;
    if (!encodeTitlePng(pixels, o, out, error)) return fail("encode_failed", error);
    return true;
}

// ==================================================================== job

TitleExportJob::TitleExportJob(std::unique_ptr<TitleExportContext> context) : ctx(std::move(context)) {
}

TitleExportJob::~TitleExportJob() = default;

float TitleExportJob::progress() const {
    if (finished) return 1.0f;
    const float inRender = rendering ? rendering->progress() : 0.0f;
    return std::min(0.99f, ((float)ctx->rendersDone + inRender) / (float)std::max<size_t>(1, ctx->rendersTotal + 1));
}

std::string TitleExportJob::progressText() const {
    if (finished) return ctx->result.ok ? "exported" : ctx->result.error;
    return std::to_string(ctx->rendersDone) + "/" + std::to_string(ctx->rendersTotal) + " renders";
}

bool TitleExportJob::finish() {
    finished = true;
    rendering.reset();
    if (ctx->failed) return true;
    if (!ctx->zipEntries.empty()) {
        const std::string zipName = stemOf(ctx->request.document) + "_" + ctx->request.format + ".zip";
        const fs::path target = ctx->request.out / zipName;
        std::error_code ec;
        if (fs::exists(target, ec) && !ctx->request.options.value("overwrite", false)) {
            ctx->fail("output_exists", "'" + zipName + "' exists: pass options.overwrite true to replace it");
            return true;
        }
        fs::create_directories(target.parent_path(), ec);
        const fs::path temp = target.string() + ".tmp";
        ZipWriter zip;
        std::string error;
        bool ok = zip.open(temp, error);
        for (const auto& [entry, content] : ctx->zipEntries) {
            if (!ok) break;
            const bool png = entry.size() > 4 && lower(entry.substr(entry.size() - 4)) == ".png";
            ok = zip.add(entry, content.data(), content.size(), png, error);
        }
        ok = ok && zip.finish(error);
        if (ok) {
            fs::rename(temp, target, ec);
            ok = !ec;
            if (ec) error = ec.message();
        }
        if (!ok) {
            fs::remove(temp, ec);
            ctx->fail("write_failed", "writing " + zipName + ": " + error);
            return true;
        }
        const fs::path project = ctx->request.project ? ctx->request.project->getProjectPath() : fs::path();
        std::error_code rel;
        ctx->result.files = {!project.empty() && inside(project, target) ? fs::relative(target, project, rel).generic_string() : target.generic_string()};
        TitleJson entries = TitleJson::array();
        for (const auto& e : ctx->zipEntries) entries.push_back(e.first);
        ctx->result.extra["zip_entries"] = entries;
    }
    ctx->result.ok = true;
    return true;
}

bool TitleExportJob::step(double budgetMs) {
    if (finished) return true;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    while (!finished) {
        if (ctx->failed) return finish();
        if (rendering) {
            titleart::StepBudget budget;
            budget.budgetUs = (uint32_t)std::max(1000.0, (budgetMs - elapsed()) * 1000.0);
            rendering->step(budget);
            if (rendering->failed()) {
                ctx->fail("render_failed", rendering->error());
                return finish();
            }
            if (!rendering->done()) return false;
            TitleRenderedImage image;
            std::string error;
            if (!TitleArtRenderHost::readBack(*rendering, current.render->outputBitDepth, image, error)) {
                ctx->fail(error.rfind("bit_depth_unsupported", 0) == 0 ? "bit_depth_unsupported" : "readback_failed", error);
                return finish();
            }
            for (const titleart::TitleProblem& p : image.problems) {
                if (p.severity != titleart::TitleSeverity::Info) ctx->warn(p.code + ": " + p.message);
            }
            rendering.reset();
            ctx->rendersDone++;
            current.done(image);
            for (auto it = ctx->added.rbegin(); it != ctx->added.rend(); ++it) ctx->queue.push_front(std::move(*it));
            ctx->added.clear();
            if (elapsed() >= budgetMs) return false;
            continue;
        }
        if (ctx->queue.empty()) return finish();
        current = std::move(ctx->queue.front());
        ctx->queue.pop_front();
        if (current.render) {
            std::string error;
            titleart::RenderRequest request = *current.render;
            rendering = TitleArtRenderHost::editor().begin(ctx->request.project, std::move(request), error);
            if (!rendering) {
                ctx->fail("render_failed", error);
                return finish();
            }
            continue;
        }
        const bool ok = current.cpu();
        for (auto it = ctx->added.rbegin(); it != ctx->added.rend(); ++it) ctx->queue.push_front(std::move(*it));
        ctx->added.clear();
        if (!ok && !ctx->failed) ctx->fail("export_failed", "the export stopped");
        if (elapsed() >= budgetMs) return finished;
    }
    return true;
}

const TitleExportResult& TitleExportJob::run() {
    while (!step(1000.0)) {
    }
    return ctx->result;
}

// ==================================================================== start

fs::path editor::titleExportDefaultOut(Project* project, const fs::path& document) {
    const fs::path root = project ? project->getProjectPath() : document.parent_path();
    return root / "Recordings" / "Title Art" / stemOf(document);
}

bool editor::titleExportLanguages(const titleart::TitleArtDoc& doc, const TitleJson& value, std::vector<std::string>& out, std::string& error) {
    out.clear();
    const std::vector<std::string> codes = titleart::titleLanguageCodes(doc.root);
    std::vector<std::string> asked;
    if (value.is_null()) return true;
    if (value.is_string() && value.get<std::string>() == "all") {
        out = codes;
        return true;
    }
    if (value.is_string()) {
        asked.push_back(value.get<std::string>());
    } else if (value.is_array()) {
        for (const TitleJson& v : value) {
            if (!v.is_string()) {
                error = "languages is \"all\", a code or a list of codes";
                return false;
            }
            asked.push_back(v.get<std::string>());
        }
    } else {
        error = "languages is \"all\", a code or a list of codes";
        return false;
    }
    for (const std::string& a : asked) {
        std::string found;
        for (const std::string& c : codes) if (lower(c) == lower(a)) found = c;
        if (found.empty()) {
            std::string list;
            for (const std::string& c : codes) list += (list.empty() ? "" : ", ") + c;
            error = "the title has no language '" + a + "'" + (list.empty() ? " (it is monolingual)" : " (languages: " + list + ")");
            return false;
        }
        if (std::find(out.begin(), out.end(), found) == out.end()) out.push_back(found);
    }
    return true;
}

namespace {

    bool readNumber(const TitleJson& o, const char* key, double lo, double hi, double& out, std::string& error) {
        if (!o.contains(key)) return true;
        if (!o[key].is_number() || o[key].get<double>() < lo || o[key].get<double>() > hi) {
            error = std::string(key) + " is a number in " + std::to_string(lo) + ".." + std::to_string(hi);
            return false;
        }
        out = o[key].get<double>();
        return true;
    }

    bool readBool(const TitleJson& o, const char* key, bool& out, std::string& error) {
        if (!o.contains(key)) return true;
        if (!o[key].is_boolean()) {
            error = std::string(key) + " is true or false";
            return false;
        }
        out = o[key].get<bool>();
        return true;
    }

    bool imageSpec(const TitleJson& o, TitleImageSpec& spec, std::string& error) {
        double v = spec.scale;
        if (!readNumber(o, "scale", 0.1, 8, v, error)) return false;
        spec.scale = v;
        if (!readBool(o, "trim", spec.trim, error) || !readBool(o, "pot", spec.pot, error) || !readBool(o, "dither", spec.dither, error) ||
            !readBool(o, "alpha_bleed", spec.alphaBleed, error) || !readBool(o, "metadata", spec.metadata, error)) {
            return false;
        }
        v = spec.padding;
        if (!readNumber(o, "padding", 0, 1024, v, error)) return false;
        spec.padding = v;
        if (o.contains("round_to")) {
            const TitleJson& r = o["round_to"];
            if (!r.is_number_integer() || (r != 1 && r != 2 && r != 4 && r != 8)) {
                error = "round_to is 1, 2, 4 or 8";
                return false;
            }
            spec.roundTo = r.get<int>();
        }
        v = spec.alphaThreshold;
        if (!readNumber(o, "alpha_threshold", 1, 255, v, error)) return false;
        spec.alphaThreshold = (int)v;
        v = spec.maxSize;
        if (!readNumber(o, "max_size", 16, 16384, v, error)) return false;
        spec.maxSize = (int)v;
        v = spec.bleedPx;
        if (!readNumber(o, "bleed_px", 0, 64, v, error)) return false;
        spec.bleedPx = (int)v;
        if (o.contains("bit_depth")) {
            if (o["bit_depth"] != 8 && o["bit_depth"] != 16) {
                error = "bit_depth is 8 or 16";
                return false;
            }
            spec.bitDepth = o["bit_depth"].get<int>();
        }
        if (o.contains("alpha")) {
            if (o["alpha"] != "straight" && o["alpha"] != "premultiplied") {
                error = "alpha is straight or premultiplied";
                return false;
            }
            spec.alpha = o["alpha"].get<std::string>();
        }
        return true;
    }

}

std::unique_ptr<TitleExportJob> editor::startTitleExport(TitleExportRequest request, std::string& code, std::string& error) {
    if (formats().empty()) registerTitleExportFormats();
    const TitleExportFormat* format = findTitleExportFormat(request.format);
    if (!format) {
        code = "bad_value";
        std::string list;
        for (const TitleExportFormat& f : formats()) list += (list.empty() ? "" : ", ") + f.name;
        error = "format '" + request.format + "' is not one of bake, glb, " + list;
        return nullptr;
    }
    // the closed table of the format's options (S47 D4): checked before anything renders
    std::vector<std::string> keys = format->optionKeys;
    if (format->image) keys.insert(keys.end(), titleImageOptionKeys().begin(), titleImageOptionKeys().end());
    keys.push_back("overwrite");
    keys.push_back("zip");
    if (!request.options.is_object()) request.options = TitleJson::object();
    for (auto it = request.options.begin(); it != request.options.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
            code = "bad_option";
            std::string list;
            for (const std::string& k : keys) list += (list.empty() ? "" : ", ") + k;
            error = "format " + request.format + " takes options {" + list + "}; '" + it.key() + "' is not one of them";
            return nullptr;
        }
    }
    for (const char* flag : {"overwrite", "zip"}) {
        if (request.options.contains(flag) && !request.options[flag].is_boolean()) {
            code = "bad_option";
            error = std::string(flag) + " is true or false";
            return nullptr;
        }
    }
    auto ctx = std::make_unique<TitleExportContext>();
    if (format->image && !imageSpec(request.options, ctx->image, error)) {
        code = "bad_option";
        return nullptr;
    }
    if (request.out.empty()) request.out = titleExportDefaultOut(request.project, request.document);
    if (request.insideProjectOnly && request.project && !inside(request.project->getProjectPath(), request.out)) {
        code = "out_outside_project";
        error = "out must be inside the project (" + request.out.generic_string() + " is not); the CLI writes anywhere";
        return nullptr;
    }
    ctx->request = std::move(request);
    if (!format->plan(*ctx)) {
        code = ctx->result.code.empty() ? "export_failed" : ctx->result.code;
        error = ctx->result.error;
        return nullptr;
    }
    for (auto& s : ctx->added) ctx->queue.push_back(std::move(s));
    ctx->added.clear();
    return std::make_unique<TitleExportJob>(std::move(ctx));
}
