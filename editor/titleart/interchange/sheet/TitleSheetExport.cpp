// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleSheetExport.h"

#include "EditorVersion.h"
#include "titleart/interchange/TitleExport.h"
#include "stb_rect_pack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleRectI;

namespace {

    constexpr size_t MAX_FRAMES = 1024;

    std::unique_ptr<TitleAnimFrameSource>& source() {
        static std::unique_ptr<TitleAnimFrameSource> s;
        return s;
    }

    // one language's frames: the posed documents, their trimmed frames and pixels
    struct Frames {
        std::string language;
        TitleAnimInfo info;
        double fps = 30.0;
        std::vector<double> times;
        std::vector<std::shared_ptr<titleart::TitleArtDoc>> posed;
        std::vector<TitleRectI> trimmed;
        TitleRectI frame;
        double scale = 1.0;
        int width = 0, height = 0;
        std::vector<texsynth::Image> pixels;
    };

    // an image's pixels with alpha >= threshold, image px
    TitleRectI alphaBox(const texsynth::Image& image, int threshold) {
        int x0 = image.width, y0 = image.height, x1 = -1, y1 = -1;
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                if (image.pixel(x, y)[3] < threshold) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        if (x1 < 0) return {};
        return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }

    void blit(const texsynth::Image& from, const TitleRectI& src, texsynth::Image& to, int dx, int dy) {
        for (int y = 0; y < src.height; y++) {
            for (int x = 0; x < src.width; x++) {
                const int tx = dx + x, ty = dy + y;
                if (tx < 0 || ty < 0 || tx >= to.width || ty >= to.height) continue;
                std::copy_n(from.pixel(src.x + x, src.y + y), 4, to.pixel(tx, ty));
            }
        }
    }

    // one pixel of the rect's edge copied outward (bilinear sampling at the edge stays inside)
    void extrude(texsynth::Image& page, int x, int y, int w, int h) {
        for (int i = 0; i < w; i++) {
            if (y > 0) std::copy_n(page.pixel(x + i, y), 4, page.pixel(x + i, y - 1));
            if (y + h < page.height) std::copy_n(page.pixel(x + i, y + h - 1), 4, page.pixel(x + i, y + h));
        }
        for (int j = -1; j <= h; j++) {
            const int row = std::clamp(y + j, 0, page.height - 1);
            if (x > 0) std::copy_n(page.pixel(x, row), 4, page.pixel(x - 1, row));
            if (x + w < page.width) std::copy_n(page.pixel(x + w - 1, row), 4, page.pixel(x + w, row));
        }
    }

    Json rectJson(int x, int y, int w, int h) {
        return Json{{"x", x}, {"y", y}, {"w", w}, {"h", h}};
    }

    std::string frameName(const std::string& stem, size_t i) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%04zu", i);
        return stem + "_" + buffer;
    }

    Json meta(const std::string& image, int w, int h, const Frames& f) {
        return Json{{"app", "JasperEngine"}, {"version", JASPERENGINE_EDITOR_VERSION}, {"image", image}, {"format", "RGBA8888"},
                    {"size", Json{{"w", w}, {"h", h}}}, {"scale", "1"},
                    {"frameTags", Json::array({Json{{"name", f.info.name}, {"from", 0}, {"to", (int)f.times.size() - 1}, {"direction", "forward"}}})}};
    }

    // ---- the three writers ----

    bool writeSheet(TitleExportContext& c, Frames& f) {
        const size_t n = f.pixels.size();
        const int columns = c.request.options.contains("columns") ? std::clamp(c.request.options["columns"].get<int>(), 1, (int)n)
                                                                  : (int)std::ceil(std::sqrt((double)n));
        const int rows = (int)((n + columns - 1) / columns);
        const int cw = f.pixels.front().width, ch = f.pixels.front().height;
        texsynth::Image grid = texsynth::Image::make(cw * columns, ch * rows, 4, 8);
        Json frames = Json::array();
        const std::string stem = c.fileName("", "", "");
        const std::string base = stem.substr(0, stem.size() - 1);
        for (size_t i = 0; i < n; i++) {
            const int x = (int)(i % columns) * cw, y = (int)(i / columns) * ch;
            blit(f.pixels[i], {0, 0, cw, ch}, grid, x, y);
            frames.push_back(Json{{"filename", frameName(base, i)}, {"frame", rectJson(x, y, cw, ch)}, {"rotated", false}, {"trimmed", false},
                                  {"spriteSourceSize", rectJson(0, 0, cw, ch)}, {"sourceSize", Json{{"w", cw}, {"h", ch}}},
                                  {"duration", (int)std::lround(1000.0 / f.fps)}});
        }
        const std::string code = f.language;
        // S45's grid convention: <stem>@<W>x<H>[.<code>].png
        std::string imageName = base + "@" + std::to_string(cw) + "x" + std::to_string(ch);
        if (!code.empty()) {
            std::string lowerCode;
            for (char letter : code) lowerCode += (char)std::tolower((unsigned char)letter);
            imageName += "." + lowerCode;
        }
        imageName += ".png";
        std::vector<uint8_t> png;
        if (!c.encodePng(grid, code, png) || !c.write(imageName, std::string(png.begin(), png.end()))) return false;
        Json j = Json{{"format", "jasperengine.titleart.sheet"}, {"version", 1}, {"frames", frames}, {"meta", meta(imageName, grid.width, grid.height, f)}};
        return c.write(c.fileName("", code, "sheet.json"), j.dump(2) + "\n");
    }

    bool writeSequence(TitleExportContext& c, Frames& f) {
        for (size_t i = 0; i < f.pixels.size(); i++) {
            char part[16];
            std::snprintf(part, sizeof(part), "%04zu", i);
            std::vector<uint8_t> png;
            if (!c.encodePng(f.pixels[i], f.language, png) || !c.write(c.fileName(part, f.language, "png"), std::string(png.begin(), png.end()))) return false;
        }
        return true;
    }

    bool writeAtlas(TitleExportContext& c, Frames& f) {
        const int maxAtlas = std::clamp(c.request.options.value("max_atlas", 4096), 64, 16384);
        // 2 px between frames, 1 px of it the frames' extruded edge
        const int padding = 2;
        struct Item {
            size_t frame;
            TitleRectI box;     // the trimmed pixels in the frame image
            int page = -1, x = 0, y = 0;
        };
        std::vector<Item> items;
        for (size_t i = 0; i < f.pixels.size(); i++) {
            TitleRectI box = alphaBox(f.pixels[i], c.image.alphaThreshold);
            if (box.empty()) box = {0, 0, 1, 1};
            if (box.width + 2 * padding > maxAtlas || box.height + 2 * padding > maxAtlas) {
                return c.fail("atlas_too_small", "frame " + std::to_string(i) + " (" + std::to_string(box.width) + " x " + std::to_string(box.height) +
                                                      ") does not fit max_atlas " + std::to_string(maxAtlas));
            }
            items.push_back({i, box});
        }
        // height descending, then frame index (deterministic, the flipbook's order rules)
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].box.height > items[b].box.height; });
        std::vector<size_t> left = order;
        int pages = 0;
        while (!left.empty()) {
            if (pages == 16) return c.fail("atlas_too_many_pages", "the frames need more than 16 pages of " + std::to_string(maxAtlas));
            std::vector<stbrp_node> nodes((size_t)maxAtlas);
            stbrp_context context;
            stbrp_init_target(&context, maxAtlas, maxAtlas, nodes.data(), (int)nodes.size());
            std::vector<stbrp_rect> rects(left.size());
            for (size_t k = 0; k < left.size(); k++) {
                rects[k].id = (int)left[k];
                rects[k].w = items[left[k]].box.width + 2 * padding;
                rects[k].h = items[left[k]].box.height + 2 * padding;
            }
            stbrp_pack_rects(&context, rects.data(), (int)rects.size());
            std::vector<size_t> next;
            for (const stbrp_rect& r : rects) {
                if (!r.was_packed) {
                    next.push_back((size_t)r.id);
                    continue;
                }
                items[(size_t)r.id].page = pages;
                items[(size_t)r.id].x = r.x + padding;
                items[(size_t)r.id].y = r.y + padding;
            }
            if (next.size() == left.size()) return c.fail("atlas_pack_failed", "no frame fits a page");
            left = next;
            pages++;
        }
        const std::string stem = c.fileName("", "", "");
        const std::string base = stem.substr(0, stem.size() - 1);
        Json frames = Json::object();
        Json pageNames = Json::array();
        for (int p = 0; p < pages; p++) {
            int w = 1, h = 1;
            for (const Item& it : items) {
                if (it.page != p) continue;
                w = std::max(w, it.x + it.box.width + padding);
                h = std::max(h, it.y + it.box.height + padding);
            }
            texsynth::Image page = texsynth::Image::make(w, h, 4, 8);
            for (const Item& it : items) {
                if (it.page != p) continue;
                blit(f.pixels[it.frame], it.box, page, it.x, it.y);
                extrude(page, it.x, it.y, it.box.width, it.box.height);
            }
            const std::string name = c.fileName("page" + std::to_string(p), f.language, "png");
            std::vector<uint8_t> png;
            if (!c.encodePng(page, f.language, png) || !c.write(name, std::string(png.begin(), png.end()))) return false;
            pageNames.push_back(Json{{"image", name}, {"size", Json{{"w", w}, {"h", h}}}});
        }
        const int sw = f.pixels.front().width, sh = f.pixels.front().height;
        for (const Item& it : items) {
            frames[frameName(base, it.frame)] = Json{{"frame", rectJson(it.x, it.y, it.box.width, it.box.height)}, {"rotated", false},
                                                    {"trimmed", it.box.width != sw || it.box.height != sh},
                                                    {"spriteSourceSize", rectJson(it.box.x, it.box.y, it.box.width, it.box.height)},
                                                    {"sourceSize", Json{{"w", sw}, {"h", sh}}}, {"duration", (int)std::lround(1000.0 / f.fps)},
                                                    {"page", it.page}};
        }
        Json m = meta(pageNames.empty() ? std::string() : pageNames[0]["image"].get<std::string>(), pageNames.empty() ? 0 : pageNames[0]["size"]["w"].get<int>(),
                      pageNames.empty() ? 0 : pageNames[0]["size"]["h"].get<int>(), f);
        m["pages"] = pageNames;
        Json j = Json{{"format", "jasperengine.titleart.atlas"}, {"version", 1}, {"frames", frames}, {"meta", m}};
        return c.write(c.fileName("", f.language, "atlas.json"), j.dump(2) + "\n");
    }

    bool plan(TitleExportContext& ctx, const std::string& format) {
        const TitleAnimFrameSource* s = titleAnimFrameSource();
        std::vector<std::string> languages = ctx.request.languages;
        if (languages.empty()) languages.push_back("");
        if (ctx.request.options.contains("columns") && !ctx.request.options["columns"].is_number_integer()) {
            return ctx.fail("bad_option", "columns is a whole number");
        }
        if (ctx.request.options.contains("max_atlas") && !ctx.request.options["max_atlas"].is_number_integer()) {
            return ctx.fail("bad_option", "max_atlas is a whole number");
        }
        for (const std::string& code : languages) {
            auto f = std::make_shared<Frames>();
            f->language = code;
            const titleart::TitleArtDoc doc = ctx.resolved(code);
            if (!s) {
                if (doc.root.contains("animation")) return ctx.fail("not_available", "sampling a title's animations arrives with S85 (Title Art animation)");
                return ctx.fail("no_animation", "the title has no animation to export as a " + format);
            }
            const std::vector<TitleAnimInfo> animations = s->animations(doc);
            if (animations.empty()) return ctx.fail("no_animation", "the title has no animation to export as a " + format);
            const std::string wanted = ctx.request.options.value("animation", std::string());
            auto found = std::find_if(animations.begin(), animations.end(), [&](const TitleAnimInfo& a) { return wanted.empty() || a.name == wanted; });
            if (found == animations.end()) {
                std::string list;
                for (const TitleAnimInfo& a : animations) list += (list.empty() ? "" : ", ") + a.name;
                return ctx.fail("bad_option", "no animation '" + wanted + "' (animations: " + list + ")");
            }
            f->info = *found;
            f->fps = ctx.request.options.contains("fps") && ctx.request.options["fps"].is_number() ? ctx.request.options["fps"].get<double>() : found->fps;
            if (!(f->fps > 0 && f->fps <= 240)) return ctx.fail("bad_option", "fps is a number in 0..240");
            std::string error;
            if (!titleAnimFrameTimes(*found, f->fps, ctx.request.options.contains("frames") ? ctx.request.options["frames"] : Json(), f->times, error)) {
                return ctx.fail("bad_option", error);
            }
            for (double t : f->times) {
                auto posed = std::make_shared<titleart::TitleArtDoc>();
                if (!s->pose(doc, found->name, t, *posed, error)) return ctx.fail("pose_failed", error);
                f->posed.push_back(posed);
            }
            f->trimmed.resize(f->posed.size());
            for (size_t i = 0; i < f->posed.size(); i++) {
                ctx.measure(f->posed[i], "", "", [f, i](TitleRenderedImage&, const TitleRectI& frame) { f->trimmed[i] = frame; });
            }
            TitleExportContext* c = &ctx;
            ctx.then([c, f, format] {
                TitleRectI shared;
                for (const TitleRectI& r : f->trimmed) shared = titleart::unionRect(shared, r);
                f->scale = c->image.scale;
                f->frame = c->finishFrame(shared, f->scale, &f->width, &f->height);
                c->result.frames[f->language.empty() ? titleart::titleSourceLanguage(c->request.doc.root) : f->language] =
                    Json::array({f->frame.x, f->frame.y, f->frame.width, f->frame.height});
                f->pixels.resize(f->posed.size());
                for (size_t i = 0; i < f->posed.size(); i++) {
                    c->renderFrame(f->posed[i], "", f->frame, f->scale, "", [f, i](texsynth::Image& pixels) { f->pixels[i] = std::move(pixels); },
                                   f->width, f->height);
                }
                c->then([c, f, format] {
                    c->result.extra["frame_count"] = f->pixels.size();
                    if (format == "sheet") return writeSheet(*c, *f);
                    if (format == "atlas") return writeAtlas(*c, *f);
                    return writeSequence(*c, *f);
                });
                return true;
            });
        }
        return true;
    }

}

void editor::registerTitleAnimFrameSource(TitleAnimFrameSource s) {
    source() = std::make_unique<TitleAnimFrameSource>(std::move(s));
}

const TitleAnimFrameSource* editor::titleAnimFrameSource() {
    return source().get();
}

bool editor::titleAnimFrameTimes(const TitleAnimInfo& info, double fps, const Json& frames, std::vector<double>& times, std::string& error) {
    times.clear();
    // a once animation includes its end, a loop covers [0, D)
    size_t total = info.loop ? (size_t)std::max(1.0, std::ceil(info.duration * fps - 1e-9)) : (size_t)std::floor(info.duration * fps + 1e-9) + 1;
    size_t from = 0, to = total - 1;
    if (frames.is_number_integer()) {
        const long long count = frames.get<long long>();
        if (count < 1) {
            error = "frames is a count of at least 1, or [from, to]";
            return false;
        }
        total = (size_t)count;
        to = total - 1;
    } else if (frames.is_array()) {
        if (frames.size() != 2 || !frames[0].is_number_integer() || !frames[1].is_number_integer() || frames[0].get<long long>() < 0 ||
            frames[1].get<long long>() < frames[0].get<long long>()) {
            error = "frames is a count, or [from, to] frame indexes with from <= to";
            return false;
        }
        from = (size_t)frames[0].get<long long>();
        to = (size_t)frames[1].get<long long>();
    } else if (!frames.is_null()) {
        error = "frames is a count, or [from, to] frame indexes";
        return false;
    }
    if (to - from + 1 > MAX_FRAMES) {
        error = "at most " + std::to_string(MAX_FRAMES) + " frames (" + std::to_string(to - from + 1) + " asked)";
        return false;
    }
    for (size_t i = from; i <= to; i++) times.push_back((double)i / fps);
    return true;
}

void editor::registerTitleSheetExport() {
    for (const char* name : {"sheet", "atlas", "sequence"}) {
        TitleExportFormat format;
        format.name = name;
        format.optionKeys = {"animation", "fps", "frames"};
        if (std::string(name) == "sheet") format.optionKeys.push_back("columns");
        if (std::string(name) == "atlas") format.optionKeys.push_back("max_atlas");
        const std::string n = name;
        format.plan = [n](TitleExportContext& ctx) { return plan(ctx, n); };
        registerTitleExportFormat(std::move(format));
    }
}
