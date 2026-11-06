// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// render_title and check_title (S80 R55, R56, §6.11, §6.12), and the helpers the title_art tools
// share (TitleArtToolsShared.h): sheets of what-if renders on backgrounds with real alpha,
// compare with a kept revision or a PNG, the forced RGBA8 path, and the checks of the registry.

#include "TitleArtToolsShared.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "texsynth/SynthesisRecipe.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/render/TitleArtCaps.h"
#include "command/UndoOrigin.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/ImageDiff.h"
#include "util/PngWriter.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace jasperengine::editor::mcp::titletools {

namespace fs = std::filesystem;
using titleart::TitleRectI;

// ==================================================================== shared helpers

titleart::Json toOrdered(const Json& value) {
    return titleart::Json::parse(value.dump());
}

Json fromOrdered(const titleart::Json& value) {
    return Json::parse(value.dump());
}

bool resolveTitlePath(Project* project, const std::string& given, bool mustExist, fs::path& absolute, std::string& relative, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    if (given.empty()) {
        error = "`path` is required: a .titleart file (project-relative).";
        return false;
    }
    if (!Util::isTitleArtFile(given)) {
        error = "'" + given + "' is not a .titleart file.";
        return false;
    }
    std::string resolveError;
    if (!McpRefs::resolveProjectPath(project, given, absolute, resolveError, false)) {
        error = resolveError;
        return false;
    }
    std::error_code ec;
    if (!fs::exists(absolute, ec)) {
        const fs::path asset = project->resolveAssetPath(fs::path(given));
        if (!asset.empty() && fs::exists(asset, ec)) absolute = asset.lexically_normal();
    }
    if (mustExist && !fs::is_regular_file(absolute, ec)) {
        error = "'" + given + "' does not exist (paths are project-relative; create it with edit_title {create: {}}).";
        return false;
    }
    relative = project->normalizeToProjectRelative(absolute).generic_string();
    return true;
}

std::shared_ptr<TitleArtDocument> openTitle(Project* project, const fs::path& absolute, std::string& error) {
    // the documents the tools opened stay open for the session: `rev` (TitleArtDocument::revision)
    // counts the texts one document object held, so a document closed after each call would
    // start again at 1 and expect_rev / compare "rev:<n>" would mean nothing
    static std::map<std::string, std::shared_ptr<TitleArtDocument>> kept;
    static Project* keptProject = nullptr;
    static fs::path keptRoot;
    if (keptProject != project || (project && keptRoot != project->getProjectPath())) {
        kept.clear();
        keptProject = project;
        keptRoot = project ? project->getProjectPath() : fs::path();
    }
    const std::string key = absolute.lexically_normal().generic_string();
    auto it = kept.find(key);
    // a document the registry no longer holds (closed with its project, or moved) is opened again
    if (it != kept.end() && TitleArtDocument::find(absolute) == it->second) return it->second;
    std::shared_ptr<TitleArtDocument> document = TitleArtDocument::open(project, absolute.string(), &error);
    if (document) kept[key] = document;
    else if (it != kept.end()) kept.erase(it);
    return document;
}

bool pngBase64(const texsynth::Image& image, std::string& base64) {
    std::vector<uint8_t> png;
    if (!texsynth::encodePng(image, png)) return false;
    base64 = Base64::encode(png.data(), png.size());
    return true;
}

ToolImage toolImage(const texsynth::Image& image) {
    ToolImage out;
    pngBase64(image, out.data);
    return out;
}

texsynth::Image downscaleTo(const texsynth::Image& source, int maxWidth) {
    const texsynth::Image image = source.channels == 4 && source.bitDepth == 8 ? source : texsynth::toRgba8(source);
    if (maxWidth <= 0 || image.width <= maxWidth) return image;
    const double factor = static_cast<double>(image.width) / maxWidth;
    const int w = maxWidth, h = std::max(1, static_cast<int>(std::lround(image.height / factor)));
    texsynth::Image out = texsynth::Image::make(w, h, 4, 8);
    for (int y = 0; y < h; y++) {
        const int y0 = static_cast<int>(y * factor), y1 = std::max(y0 + 1, std::min(image.height, static_cast<int>((y + 1) * factor)));
        for (int x = 0; x < w; x++) {
            const int x0 = static_cast<int>(x * factor), x1 = std::max(x0 + 1, std::min(image.width, static_cast<int>((x + 1) * factor)));
            // premultiplied box average, so transparent colour does not bleed in
            double sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t* p = image.pixel(sx, sy);
                    const double a = p[3] / 255.0;
                    sum[0] += p[0] * a; sum[1] += p[1] * a; sum[2] += p[2] * a; sum[3] += p[3];
                    n++;
                }
            }
            uint8_t* d = out.pixel(x, y);
            const double alpha = sum[3] / n;
            for (int k = 0; k < 3; k++) d[k] = alpha > 0 ? static_cast<uint8_t>(std::clamp(sum[k] / n / (alpha / 255.0), 0.0, 255.0)) : 0;
            d[3] = static_cast<uint8_t>(std::lround(alpha));
        }
    }
    return out;
}

bool parseBackgrounds(Project* project, const Json& list, std::vector<SheetBackground>& out, std::string& error) {
    out.clear();
    if (list.is_null()) {
        SheetBackground checker;
        checker.name = "checker";
        out.push_back(checker);
        return true;
    }
    if (!list.is_array() || list.empty()) {
        error = "`backgrounds` is a list of: transparent, checker, black, white, #rrggbb or a project image path.";
        return false;
    }
    for (const Json& item : list) {
        if (!item.is_string()) {
            error = "`backgrounds` holds strings: transparent, checker, black, white, #rrggbb or an image path.";
            return false;
        }
        SheetBackground bg;
        bg.name = item.get<std::string>();
        titleart::TitleColor color;
        if (bg.name == "transparent") {
            bg.kind = SheetBackground::Kind::Transparent;
        } else if (bg.name == "checker") {
            bg.kind = SheetBackground::Kind::Checker;
        } else if (bg.name == "black" || bg.name == "white") {
            bg.kind = SheetBackground::Kind::Color;
            const uint8_t v = bg.name == "white" ? 255 : 0;
            bg.color[0] = bg.color[1] = bg.color[2] = v;
        } else if (!bg.name.empty() && bg.name[0] == '#' && titleart::parseTitleColor(titleart::Json(bg.name), color)) {
            bg.kind = SheetBackground::Kind::Color;
            bg.color[0] = static_cast<uint8_t>(std::lround(color.r * 255));
            bg.color[1] = static_cast<uint8_t>(std::lround(color.g * 255));
            bg.color[2] = static_cast<uint8_t>(std::lround(color.b * 255));
        } else {
            fs::path file;
            std::string resolveError;
            if (!McpRefs::resolveProjectPath(project, bg.name, file, resolveError, true)) {
                error = "background '" + bg.name + "': not transparent, checker, black, white, #rrggbb or an existing image (" + resolveError + ")";
                return false;
            }
            texsynth::Image loaded;
            if (!texsynth::loadImage(file, loaded, resolveError)) {
                error = "background '" + bg.name + "': " + resolveError;
                return false;
            }
            bg.kind = SheetBackground::Kind::Image;
            bg.image = texsynth::toRgba8(loaded);
        }
        out.push_back(std::move(bg));
    }
    return true;
}

texsynth::Image composite(const texsynth::Image& source, const SheetBackground& background) {
    const texsynth::Image image = source.channels == 4 && source.bitDepth == 8 ? source : texsynth::toRgba8(source);
    if (background.kind == SheetBackground::Kind::Transparent) return image;
    texsynth::Image out = texsynth::Image::make(image.width, image.height, 4, 8);
    for (int y = 0; y < image.height; y++) {
        for (int x = 0; x < image.width; x++) {
            uint8_t bg[3];
            if (background.kind == SheetBackground::Kind::Checker) {
                const uint8_t v = ((x / 16 + y / 16) % 2) ? 204 : 255;
                bg[0] = bg[1] = bg[2] = v;
            } else if (background.kind == SheetBackground::Kind::Image && !background.image.empty()) {
                // the image stretched over the cell
                const int sx = std::min(background.image.width - 1, x * background.image.width / std::max(1, image.width));
                const int sy = std::min(background.image.height - 1, y * background.image.height / std::max(1, image.height));
                const uint8_t* p = background.image.pixel(sx, sy);
                std::copy_n(p, 3, bg);
            } else {
                std::copy_n(background.color, 3, bg);
            }
            const uint8_t* s = image.pixel(x, y);
            uint8_t* d = out.pixel(x, y);
            for (int k = 0; k < 3; k++) d[k] = static_cast<uint8_t>((s[k] * s[3] + bg[k] * (255 - s[3]) + 127) / 255);
            d[3] = 255;
        }
    }
    return out;
}

texsynth::Image sheet(const std::vector<texsynth::Image>& cells, int columns, std::vector<SheetCell>& boxes) {
    const int gap = 4;
    columns = std::max(1, std::min(columns, static_cast<int>(cells.size())));
    int cellW = 1, cellH = 1;
    for (const texsynth::Image& c : cells) {
        cellW = std::max(cellW, c.width);
        cellH = std::max(cellH, c.height);
    }
    const int rows = (static_cast<int>(cells.size()) + columns - 1) / columns;
    texsynth::Image out = texsynth::Image::make(columns * cellW + (columns - 1) * gap, rows * cellH + (rows - 1) * gap, 4, 8);
    for (size_t i = 0; i < cells.size(); i++) {
        const int cx = static_cast<int>(i % columns) * (cellW + gap), cy = static_cast<int>(i / columns) * (cellH + gap);
        const texsynth::Image cell = cells[i].channels == 4 && cells[i].bitDepth == 8 ? cells[i] : texsynth::toRgba8(cells[i]);
        for (int y = 0; y < cell.height; y++) std::copy_n(cell.pixel(0, y), static_cast<size_t>(cell.width) * 4, out.pixel(cx, cy + y));
        if (i < boxes.size()) {
            boxes[i].x = cx;
            boxes[i].y = cy;
            boxes[i].width = cell.width;
            boxes[i].height = cell.height;
        }
    }
    return out;
}

fs::path revisionRenderPath(Project* project, const std::string& guid, uint64_t rev) {
    return project->getProjectPath() / ".jasperengine" / "cache" / "titleart" / "renders" / guid / ("rev" + std::to_string(rev) + ".png");
}

void keepRevisionRender(Project* project, const std::string& guid, uint64_t rev, const texsynth::Image& image) {
    if (!project || guid.empty()) return;
    static std::set<std::string> cleared;
    const fs::path folder = revisionRenderPath(project, guid, rev).parent_path();
    std::error_code ec;
    const std::string sessionKey = project->getProjectPath().generic_string() + "\n" + guid;
    if (cleared.insert(sessionKey).second) fs::remove_all(folder, ec);
    FileUtils::createDirectories(folder, ec);
    std::vector<uint8_t> png;
    if (!texsynth::encodePng(image, png)) return;
    FileUtils::writeFileReplacing(revisionRenderPath(project, guid, rev), std::string(png.begin(), png.end()));
    // the 16 newest
    std::vector<uint64_t> kept = keptRevisions(project, guid);
    for (size_t i = 16; i < kept.size(); i++) fs::remove(revisionRenderPath(project, guid, kept[i]), ec);
}

std::vector<uint64_t> keptRevisions(Project* project, const std::string& guid) {
    std::vector<uint64_t> revs;
    const fs::path folder = revisionRenderPath(project, guid, 0).parent_path();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return revs;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("rev", 0) != 0 || entry.path().extension() != ".png") continue;
        revs.push_back(std::strtoull(name.c_str() + 3, nullptr, 10));
    }
    std::sort(revs.rbegin(), revs.rend());
    return revs;
}

Json renderMetrics(const texsynth::Image& image, const TitleRectI& frame, int canvasWidth, int canvasHeight) {
    Json metrics = Json::object();
    metrics["size"] = Json::array({image.width, image.height});
    const TitleRectI box = TitleArtRenderHost::alphaBoundsOf(image);
    size_t covered = 0;
    bool uniform = true;
    for (size_t i = 0; i < image.data.size(); i += 4) {
        if (image.data[i + 3] > 0) covered++;
        if (uniform && !std::equal(image.data.begin(), image.data.begin() + 4, image.data.begin() + static_cast<long>(i))) uniform = false;
    }
    const size_t pixels = static_cast<size_t>(image.width) * image.height;
    metrics["coverage_pct"] = pixels ? std::round(1000.0 * covered / pixels) / 10.0 : 0.0;
    metrics["flat"] = covered == 0 || uniform;
    if (box.empty() || frame.empty() || image.width <= 0) {
        metrics["alpha_bounds"] = nullptr;
        return metrics;
    }
    const double sx = static_cast<double>(frame.width) / image.width, sy = static_cast<double>(frame.height) / image.height;
    const int x = frame.x + static_cast<int>(std::floor(box.x * sx)), y = frame.y + static_cast<int>(std::floor(box.y * sy));
    const int w = static_cast<int>(std::ceil(box.width * sx)), h = static_cast<int>(std::ceil(box.height * sy));
    metrics["alpha_bounds"] = Json{{"x", x}, {"y", y}, {"w", w}, {"h", h},
                                   {"margins", Json{{"left", x}, {"top", y}, {"right", canvasWidth - x - w}, {"bottom", canvasHeight - y - h}}}};
    return metrics;
}

namespace {

    struct FrameTickState {
        std::weak_ptr<McpJob> job;
        std::shared_ptr<FrameTask> task;
        CallPtr call;
        App* app = nullptr;
    };

    // one tick per editor frame until the task is done (no self-reference: each frame enqueues anew)
    void scheduleFrameTick(const std::shared_ptr<FrameTickState>& s) {
        s->app->requestRedraw();
        getEditorHost().enqueueMainThreadTask([s]() {
            JobPtr live = s->job.lock();
            if (!live || live->finished()) return;
            McpWriteScope writeScope;
            std::optional<UndoOriginScope> origin;
            if (s->call->attributed()) origin.emplace(s->call->getUndoOrigin());
            bool done = false;
            try {
                if (live->cancelRequested()) {
                    live->markCancelled("Cancelled; nothing was written.");
                    return;
                }
                done = s->task->tick(*live);
            } catch (const std::exception& e) {
                if (!live->finished()) live->fail(std::string("Failed: ") + e.what());
                return;
            }
            if (!done && !live->finished()) scheduleFrameTick(s);
        });
    }

}

void runFrameTask(const CallPtr& call, const std::string& kind, const std::string& title, std::shared_ptr<FrameTask> task) {
    McpContext& ctx = call->ctx();
    JobPtr job = ctx.jobs().create(kind, title);
    job->setProgress(0.0, "starting");
    auto state = std::make_shared<FrameTickState>();
    state->job = job;
    state->task = task;
    state->call = call;
    state->app = &ctx.app();
    scheduleFrameTick(state);
    const Json& args = call->args();
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 120.0), [task](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(task->text.empty() ? finished.message() : task->text, result);
        reply.images = task->images;
        return reply;
    });
}

// ==================================================================== render_title

namespace {

    // render_title's `options`: the keys later specs add, by owner; none is served by S80
    struct OptionKey {
        const char* key;
        const char* owner;
    };
    const OptionKey RENDER_OPTIONS[] = {
        {"overrides", "S81"}, {"rerender", "S81"}, {"live_time", "S81"}, {"cost", "S83"}, {"animation", "S85"},
        {"quality", "S86"}, {"debug_view", "S86"}, {"languages", "S87"},
    };

    struct Variant {
        std::string label;
        std::shared_ptr<const titleart::TitleArtDoc> doc;
    };

    struct PendingRender {
        int variant = 0;
        std::string role;           // "main", "rgba8", "canonical"
        titleart::RenderRequest request;
        TitleRenderedImage image;
        bool done = false;
    };

    struct RenderState {
        RenderSpec spec;
        Json extra;
        std::string textPrefix;
        Project* project = nullptr;
        std::vector<Variant> variants;
        std::vector<PendingRender> renders;
        size_t current = 0;
        std::unique_ptr<titleart::TitleRenderJob> job;
        std::vector<SheetBackground> backgrounds;
        int maxWidth = 768;
        int columns = 0;
        bool images = true;
        std::string compare;
        std::string fallback;
        std::string savePath;
        TitleRectI region;
        int outputIndex = -1;
        bool keep = false;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    };

    bool resolveLayerRef(const titleart::TitleArtDoc& doc, const std::string& ref, std::string& id, std::string& error) {
        titleart::TitleArtDoc copy = doc;
        titleart::TitleIdGenerator ids;
        TitleOpsResult result;
        TitleOpContext context(copy, ids, result);
        titleart::Json* layer = context.resolveLayer(ref, "layer");
        if (!layer) {
            error = result.problems.empty() ? "no layer '" + ref + "'" : result.problems.front().message;
            if (!result.problems.empty() && !result.problems.front().validHere.empty()) {
                std::string list;
                for (const std::string& v : result.problems.front().validHere) list += (list.empty() ? "" : ", ") + v;
                error += " (layers: " + list + ")";
            }
            return false;
        }
        id = layer->value("id", std::string());
        return true;
    }

    bool applyWhatIf(const titleart::TitleArtDoc& base, const Json& ops, std::shared_ptr<const titleart::TitleArtDoc>& out, std::string& error) {
        if (ops.is_null()) {
            out = std::make_shared<titleart::TitleArtDoc>(base);
            return true;
        }
        TitleOpsResult result = TitleArtOps::apply(base, toOrdered(ops));
        if (!result.ok) {
            error = result.error();
            return false;
        }
        out = std::make_shared<titleart::TitleArtDoc>(std::move(result.doc));
        return true;
    }

    bool finishRender(RenderState& s, McpJob& job, FrameTask& task) {
        const titleart::TitleArtDoc& doc = *s.spec.doc;
        const int cw = doc.canvasWidth(), ch = doc.canvasHeight();
        Json result = s.extra.is_object() ? s.extra : Json::object();
        const PendingRender* main = nullptr;
        const PendingRender* rgba8 = nullptr;
        const PendingRender* canonical = nullptr;
        for (const PendingRender& r : s.renders) {
            if (r.variant == 0 && r.role == "main" && !main) main = &r;
            if (r.role == "rgba8") rgba8 = &r;
            if (r.role == "canonical") canonical = &r;
        }
        if (!main) {
            job.fail("Nothing was rendered.");
            return true;
        }
        if (!canonical && s.keep) canonical = main;
        result["path"] = s.spec.relative;
        result["rev"] = s.spec.rev;
        const Json metrics = renderMetrics(main->image.image, main->image.frame, cw, ch);
        for (auto it = metrics.begin(); it != metrics.end(); ++it) result[it.key()] = it.value();
        double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.start).count();
        result["render_ms"] = std::round(totalMs * 10.0) / 10.0;
        result["path_used"] = main->image.pathUsed;
        Json problems = Json::array();
        for (const titleart::TitleProblem& p : main->image.problems) problems.push_back(fromOrdered(p.toJson()));
        if (!problems.empty()) result["problems"] = problems;

        std::string text = s.textPrefix + "Rendered " + s.spec.relative + " (rev " + std::to_string(s.spec.rev) + ") at " +
                           std::to_string(main->image.image.width) + " x " + std::to_string(main->image.image.height);
        if (s.keep && canonical) keepRevisionRender(s.project, doc.guid(), s.spec.rev, canonical->image.image);

        if (!s.compare.empty()) {
            texsynth::Image against;
            std::string error;
            if (s.compare.rfind("rev:", 0) == 0) {
                const uint64_t rev = std::strtoull(s.compare.c_str() + 4, nullptr, 10);
                const fs::path kept = revisionRenderPath(s.project, doc.guid(), rev);
                if (!texsynth::loadImage(kept, against, error)) {
                    std::string list;
                    for (uint64_t r : keptRevisions(s.project, doc.guid())) list += (list.empty() ? "" : ", ") + std::to_string(r);
                    job.fail("compare: no render of rev " + std::to_string(rev) + " is kept (kept: " + (list.empty() ? "none" : list) +
                             "; render_title keeps the last 16 revisions it rendered).");
                    return true;
                }
            } else {
                fs::path file;
                if (!McpRefs::resolveProjectPath(s.project, s.compare, file, error, true) || !texsynth::loadImage(file, against, error)) {
                    job.fail("compare: " + error);
                    return true;
                }
            }
            const PendingRender* subject = s.compare.rfind("rev:", 0) == 0 && canonical ? canonical : main;
            ImageDiffOptions options;
            const ImageDiffResult diff = diffImages(subject->image.image, against, options);
            if (!diff.sameSize) {
                job.fail("compare: " + diff.error + " (compare renders the whole canvas at the kept scale; drop crop, layer and output)");
                return true;
            }
            result["diff"] = Json{{"against", s.compare}, {"changed_pct", std::round(diff.changedPct * 1000.0) / 1000.0}, {"max", diff.max},
                                  {"mean", std::round(diff.mean * 1000.0) / 1000.0},
                                  {"bbox", diff.bbox.empty() ? Json(nullptr) : Json::array({diff.bbox.x, diff.bbox.y, diff.bbox.width, diff.bbox.height})}};
            text += "; " + std::to_string(diff.changedPct).substr(0, 6) + " % changed against " + s.compare;
        }
        if (rgba8) {
            ImageDiffOptions options;
            const ImageDiffResult diff = diffImages(main->image.image, rgba8->image.image, options);
            result["pixel_paths_diff"] = Json{{"max", diff.max}, {"mean", std::round(diff.mean * 1000.0) / 1000.0},
                                              {"changed_pct", std::round(diff.changedPct * 1000.0) / 1000.0}};
        }
        if (!s.savePath.empty()) {
            fs::path file;
            std::string error;
            if (!McpRefs::resolveProjectWritePath(s.project, s.savePath, file, error)) {
                job.fail("save_path: " + error);
                return true;
            }
            std::vector<uint8_t> png;
            if (!texsynth::encodePng(main->image.image, png) || !FileUtils::writeFileReplacing(file, std::string(png.begin(), png.end()))) {
                job.fail("save_path: could not write " + s.savePath);
                return true;
            }
            result["saved_to"] = s.project->normalizeToProjectRelative(file).generic_string();
        }

        // the sheet: each variant (and the RGBA8 path) on each background
        Json cells = Json::array();
        if (s.images) {
            std::vector<texsynth::Image> images;
            std::vector<SheetCell> boxes;
            for (const PendingRender& r : s.renders) {
                if (r.role == "canonical") continue;
                for (const SheetBackground& bg : s.backgrounds) {
                    images.push_back(composite(r.image.image, bg));
                    SheetCell cell;
                    cell.n = static_cast<int>(boxes.size());
                    cell.label = s.variants[static_cast<size_t>(r.variant)].label + (r.role == "rgba8" ? " (rgba8)" : "");
                    cell.background = bg.name;
                    boxes.push_back(cell);
                }
            }
            const int columns = s.columns > 0 ? s.columns : static_cast<int>(s.backgrounds.size()) * (rgba8 ? 2 : 1);
            texsynth::Image full = sheet(images, columns, boxes);
            const texsynth::Image shown = downscaleTo(full, s.maxWidth);
            const double factor = full.width > 0 ? static_cast<double>(shown.width) / full.width : 1.0;
            for (const SheetCell& c : boxes) {
                cells.push_back(Json{{"n", c.n}, {"label", c.label}, {"background", c.background},
                                     {"box", Json::array({(int)std::lround(c.x * factor), (int)std::lround(c.y * factor),
                                                          (int)std::lround(c.width * factor), (int)std::lround(c.height * factor)})}});
            }
            task.images.push_back(toolImage(shown));
        }
        result["cells"] = cells;
        task.text = text + ".";
        job.succeed(task.text, result);
        return true;
    }

    bool tickRender(RenderState& s, McpJob& job, FrameTask& task) {
        const auto frameStart = std::chrono::steady_clock::now();
        while (s.current < s.renders.size()) {
            PendingRender& r = s.renders[s.current];
            if (!s.job) {
                std::string error;
                s.job = TitleArtRenderHost::editor().begin(s.project, r.request, error);
                if (!s.job) {
                    job.fail(error);
                    return true;
                }
            }
            titleart::StepBudget budget;
            budget.budgetUs = 12000;
            s.job->step(budget);
            if (s.job->failed()) {
                job.fail("The render failed: " + s.job->error());
                return true;
            }
            if (!s.job->done()) {
                job.setProgress((s.current + s.job->progress()) / s.renders.size(), "rendering " + r.role);
                return false;
            }
            std::string error;
            if (!TitleArtRenderHost::readBack(*s.job, 8, r.image, error)) {
                job.fail(error);
                return true;
            }
            if (const titleart::RendererStats* stats = TitleArtRenderHost::editor().stats()) r.image.pathUsed = stats->path ? stats->path : "";
            if (r.image.frame.empty()) {
                r.image.frame = r.request.region.empty() ? TitleRectI{0, 0, r.request.doc->canvasWidth(), r.request.doc->canvasHeight()} : r.request.region;
            }
            r.done = true;
            s.job.reset();
            s.current++;
            if (std::chrono::steady_clock::now() - frameStart > std::chrono::milliseconds(12)) return false;
        }
        return finishRender(s, job, task);
    }

}

bool checkRenderOptions(const Json& options, std::string& error) {
    if (options.is_null()) return true;
    if (!options.is_object()) {
        error = "bad_option: `options` is an object of feature keys.";
        return false;
    }
    for (auto it = options.begin(); it != options.end(); ++it) {
        bool known = false;
        for (const OptionKey& k : RENDER_OPTIONS) {
            if (it.key() != k.key) continue;
            known = true;
            error = std::string("not_available: render_title option '") + k.key + "' arrives with " + k.owner + "; this build serves none.";
            return false;
        }
        if (!known) {
            std::string list;
            for (const OptionKey& k : RENDER_OPTIONS) list += (list.empty() ? "" : ", ") + std::string(k.key) + " (" + k.owner + ")";
            error = "bad_option: render_title has no option '" + it.key() + "'. This build has no option keys; later features add: " + list + ".";
            return false;
        }
    }
    return true;
}

void startRender(const CallPtr& call, RenderSpec spec, Json extra, const std::string& textPrefix) {
    Project* project = call->ctx().project();
    auto s = std::make_shared<RenderState>();
    s->project = project;
    s->extra = std::move(extra);
    s->textPrefix = textPrefix;
    const Json& args = spec.args;
    std::string error;
    if (!parseBackgrounds(project, args.contains("backgrounds") ? args["backgrounds"] : Json(), s->backgrounds, error)) {
        call->fail(error);
        return;
    }
    s->maxWidth = std::clamp(args.value("max_width", 768), 64, 4096);
    s->columns = std::clamp(args.value("columns", 0), 0, 16);
    s->images = args.value("image", std::string("always")) != "none";
    s->compare = args.value("compare", std::string());
    s->fallback = args.value("fallback", std::string());
    s->savePath = args.value("save_path", std::string());
    const titleart::TitleArtDoc& doc = *spec.doc;
    const int cw = doc.canvasWidth(), ch = doc.canvasHeight();

    // the variants: the document (with `ops`), then each `try`
    Variant base;
    base.label = "current";
    if (!applyWhatIf(doc, args.contains("ops") ? args["ops"] : Json(), base.doc, error)) {
        call->fail("ops: " + error);
        return;
    }
    const bool whatIf = args.contains("ops") || args.contains("try");
    s->variants.push_back(base);
    if (args.contains("try")) {
        const Json& tries = args["try"];
        if (!tries.is_array() || tries.size() > 9) {
            call->fail("`try` is a list of at most 9 {label, ops}.");
            return;
        }
        for (size_t i = 0; i < tries.size(); i++) {
            Variant v;
            v.label = tries[i].value("label", "try " + std::to_string(i + 1));
            if (!applyWhatIf(*base.doc, tries[i].contains("ops") ? tries[i]["ops"] : Json::array(), v.doc, error)) {
                call->fail("try[" + std::to_string(i) + "] (" + v.label + "): " + error + " Nothing was rendered.");
                return;
            }
            s->variants.push_back(v);
        }
    }

    // what each render covers
    std::string isolate;
    if (args.contains("layer")) {
        if (!resolveLayerRef(*base.doc, args.value("layer", std::string()), isolate, error)) {
            call->fail("layer: " + error);
            return;
        }
    }
    titleart::OutputContent content = titleart::OutputContent::All;
    std::vector<std::string> contentLayers;
    if (args.contains("output")) {
        const std::string id = args.value("output", std::string());
        const titleart::Json& outputs = doc.root["outputs"];
        std::string list;
        for (size_t i = 0; i < outputs.size(); i++) {
            list += (list.empty() ? "" : ", ") + outputs[i].value("id", std::string());
            if (outputs[i].value("id", std::string()) != id) continue;
            s->outputIndex = static_cast<int>(i);
            titleart::outputContentFromName(outputs[i].value("content", std::string("all")), content);
            if (outputs[i].contains("layers")) {
                for (const auto& l : outputs[i]["layers"]) if (l.is_string()) contentLayers.push_back(l.get<std::string>());
            }
        }
        if (s->outputIndex < 0) {
            call->fail("output: no output '" + id + "' (outputs: " + list + ").");
            return;
        }
        // the output's frame: the shared one of its last bake when it is fresh, else the canvas
        TitleBakeInfo info;
        std::string text;
        if (ProjectChecks::readFile(titleArtSidecarPath(spec.file), text) && TitleBakeInfo::parse(text, info, error) &&
            info.sourceHash == titleArtSourceHash(doc)) {
            s->region = info.frame;
        }
    }
    if (args.contains("crop")) {
        const Json& crop = args["crop"];
        if (!crop.is_array() || crop.size() != 4) {
            call->fail("`crop` is [x, y, w, h] in canvas pixels (image space, top-left origin).");
            return;
        }
        s->region = {crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>()};
        if (s->region.empty()) {
            call->fail("`crop` needs a width and a height.");
            return;
        }
    }
    const int regionW = s->region.empty() ? cw : s->region.width;
    const int columns = std::max(1, s->columns > 0 ? s->columns : static_cast<int>(s->backgrounds.size()));
    const float scale = std::min(1.0f, static_cast<float>(s->maxWidth) / static_cast<float>(std::max(1, regionW * columns)));
    uint32_t flags = 0;
    if (s->fallback == "rgba8") flags = titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS | titleart::TITLE_ART_TEST_NO_R16F | titleart::TITLE_ART_TEST_NO_RGBA32F;
    for (size_t v = 0; v < s->variants.size(); v++) {
        PendingRender r;
        r.variant = static_cast<int>(v);
        r.role = "main";
        r.request.doc = s->variants[v].doc;
        r.request.pxScale = scale;
        r.request.region = s->region;
        r.request.isolateLayer = isolate;
        r.request.content = content;
        r.request.contentLayers = contentLayers;
        r.request.testFlags = flags;
        s->renders.push_back(r);
        if (v == 0 && s->fallback == "compare") {
            PendingRender forced = s->renders.back();
            forced.role = "rgba8";
            forced.request.testFlags = titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS | titleart::TITLE_ART_TEST_NO_R16F | titleart::TITLE_ART_TEST_NO_RGBA32F;
            s->renders.push_back(forced);
        }
    }
    // the canonical render of the current state: kept as this rev's, and what rev compares use
    const bool plain = !whatIf && isolate.empty() && s->outputIndex < 0 && s->region.empty() && flags == 0;
    s->keep = spec.current && !whatIf;
    const float canonicalScale = std::min(1.0f, 768.0f / static_cast<float>(std::max(1, cw)));
    const bool needCanonical = (s->keep || s->compare.rfind("rev:", 0) == 0) && !(plain && std::abs(scale - canonicalScale) < 1e-6f);
    if (needCanonical) {
        PendingRender c;
        c.role = "canonical";
        c.request.doc = s->variants[0].doc;
        c.request.pxScale = canonicalScale;
        s->renders.push_back(c);
    }
    (void)ch;
    s->spec = std::move(spec);
    auto task = std::make_shared<FrameTask>();
    std::weak_ptr<FrameTask> weakTask = task;
    task->tick = [s, weakTask](McpJob& job) {
        auto t = weakTask.lock();
        return t ? tickRender(*s, job, *t) : true;
    };
    runFrameTask(call, "render_title", "Render " + s->spec.relative, task);
}

// ==================================================================== tools

void renderTitleTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, args.value("path", std::string()), true, file, relative, error)) {
        call->fail(error);
        return;
    }
    if (!checkRenderOptions(args.contains("options") ? args["options"] : Json(), error)) {
        call->fail(error, Json{{"code", error.substr(0, error.find(':'))}});
        return;
    }
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document) {
        call->fail(error);
        return;
    }
    if (!document->editable()) {
        call->fail("version_newer: " + document->versionError() + "; it renders nothing in this editor.");
        return;
    }
    RenderSpec spec;
    spec.file = file;
    spec.relative = relative;
    spec.doc = std::make_shared<titleart::TitleArtDoc>(document->doc());
    spec.rev = document->revision();
    spec.args = args;
    startRender(call, std::move(spec), Json::object());
}

void checkTitleTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, args.value("path", std::string()), true, file, relative, error)) {
        call->fail(error);
        return;
    }
    registerBuiltinTitleChecks();
    std::vector<std::string> names;
    if (args.contains("checks")) {
        for (const Json& n : args["checks"]) if (n.is_string()) names.push_back(n.get<std::string>());
    }
    std::vector<const CheckDef*> checks;
    std::vector<std::string> valid;
    if (!TitleArtChecks::select(names, checks, error, valid)) {
        std::string list;
        for (const std::string& v : valid) list += (list.empty() ? "" : ", ") + v;
        call->fail(error + ". Checks: " + list + ".", Json{{"valid", valid}});
        return;
    }
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document) {
        call->fail(error);
        return;
    }
    auto state = std::make_shared<std::tuple<std::shared_ptr<TitleArtDocument>, std::vector<const CheckDef*>, Json, fs::path, std::string>>(
        document, checks, args, file, relative);
    auto task = std::make_shared<FrameTask>();
    std::weak_ptr<FrameTask> weakTask = task;
    task->tick = [state, project, weakTask](McpJob& job) {
        auto t = weakTask.lock();
        if (!t) return true;
        auto& [doc, selected, a, path, rel] = *state;
        TitleCheckContext context(doc->doc(), rel);
        context.file = path;
        context.assets = TitleArtRenderHost::editor().resolver(project);
        context.loadWarnings = doc->warnings();
        context.versionError = doc->versionError();
        if (a.contains("backgrounds") && a["backgrounds"].is_array() && !a["backgrounds"].empty()) {
            context.options.backgrounds.clear();
            for (const Json& b : a["backgrounds"]) if (b.is_string()) context.options.backgrounds.push_back(b.get<std::string>());
        }
        context.options.failOnWarn = a.value("fail_on", std::string("fail")) == "warn";
        const std::string baseline = a.value("baseline", std::string());
        if (baseline.rfind("rev:", 0) == 0) {
            context.options.baseline = revisionRenderPath(project, doc->guid(), std::strtoull(baseline.c_str() + 4, nullptr, 10)).string();
        } else if (!baseline.empty()) {
            fs::path baselineFile;
            std::string error;
            if (!McpRefs::resolveProjectPath(project, baseline, baselineFile, error, true)) {
                job.fail("baseline: " + error);
                return true;
            }
            context.options.baseline = baselineFile.string();
        }
        texsynth::Image lastRender;
        if (doc->editable()) {
            context.render = [project, &lastRender](const titleart::RenderRequest& request, TitleCheckImage& out, std::string& error) {
                TitleRenderedImage image;
                if (!TitleArtRenderHost::editor().render(project, request, image, error)) return false;
                out.width = image.image.width;
                out.height = image.image.height;
                out.rgba = image.image.data;
                out.frame = image.frame;
                if (request.isolateLayer.empty() && request.overrides.empty() && request.testFlags == 0 && lastRender.empty()) lastRender = image.image;
                return true;
            };
        }
        const TitleCheckReport report = TitleArtChecks::run(selected, context);
        Json result = fromOrdered(report.toJson());
        result["path"] = rel;
        result["rev"] = doc->revision();
        result["guidance"] = TitleArtChecks::guidance();
        const std::string saveBaseline = a.value("save_baseline", std::string());
        if (!saveBaseline.empty()) {
            fs::path out;
            std::string error;
            std::vector<uint8_t> png;
            if (lastRender.empty()) {
                job.fail("save_baseline: no render to save (the pixel checks did not run).");
                return true;
            }
            if (!McpRefs::resolveProjectWritePath(project, saveBaseline, out, error) || !texsynth::encodePng(lastRender, png) ||
                !FileUtils::writeFileReplacing(out, std::string(png.begin(), png.end()))) {
                job.fail("save_baseline: " + (error.empty() ? "could not write " + saveBaseline : error));
                return true;
            }
            result["saved_baseline"] = project->normalizeToProjectRelative(out).generic_string();
        }
        const std::string screenshot = a.value("screenshot", std::string("on_failure"));
        const bool failing = report.verdict == TitleCheckStatus::Fail || (context.options.failOnWarn && report.verdict == TitleCheckStatus::Warn);
        if (!lastRender.empty() && (screenshot == "always" || (screenshot == "on_failure" && failing))) {
            SheetBackground checker;
            t->images.push_back(toolImage(downscaleTo(composite(lastRender, checker), 512)));
        }
        size_t counts[4] = {0, 0, 0, 0};
        for (const TitleCheckResult& r : report.checks) counts[static_cast<int>(r.status)]++;
        t->text = std::string(titleCheckStatusName(report.verdict == TitleCheckStatus::Skip ? TitleCheckStatus::Pass : report.verdict)) + " " + rel +
                  ": " + std::to_string(counts[0]) + " pass, " + std::to_string(counts[1]) + " warn, " + std::to_string(counts[2]) + " fail, " +
                  std::to_string(counts[3]) + " skipped. " + TitleArtChecks::guidance();
        job.succeed(t->text, result);
        return true;
    };
    runFrameTask(call, "check_title", "Check " + relative, task);
}

}
