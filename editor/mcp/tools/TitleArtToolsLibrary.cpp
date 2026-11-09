// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtToolsLibrary.h"

#include "TitleArtToolsShared.h"
#include "mcp/McpContext.h"
#include "titleart/TitleArtDocument.h"
#include "Project.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/designer/TransientAssetResolver.h"
#include "titleart/library/TitleLibrary.h"
#include "titleart/library/TitleLibraryApply.h"
#include "titleart/library/TitleLibraryImports.h"
#include "titleart/library/TitleLibraryProvider.h"
#include "titleart/library/TitleLibrarySearch.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
using namespace jasperengine::editor::mcp::titletools;
namespace tl = jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

namespace {

    constexpr size_t SHEET_MAX_CELLS = 9;
    constexpr int SHEET_MAX_WIDTH = 1024, SHEET_MAX_HEIGHT = 576;

    std::string str(const titleart::Json& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    Json useOp(tl::LibraryKind kind, const std::string& ref) {
        switch (kind) {
            case tl::LibraryKind::Look: return Json{{"op", "apply_look"}, {"layer", "<layer>"}, {"look", ref}};
            case tl::LibraryKind::ColorScheme: return Json{{"op", "apply_color_scheme"}, {"color_scheme", ref}};
            case tl::LibraryKind::Template: return Json{{"op", "apply_template"}, {"template", ref}};
        }
        return nullptr;
    }

    Json rowOf(const tl::LibraryItemInfo& item, int n) {
        Json tags = Json::array();
        for (size_t i = 0; i < item.tags.size() && i < 4; i++) tags.push_back(item.tags[i]);
        Json row = Json{{"n", n}, {"id", item.ref}, {"name", item.name.any()}, {"family", item.family}};
        if (!item.dimension.empty()) row["dim"] = item.dimension;
        row["tags"] = tags;
        row["cost"] = item.cost;
        row["available"] = item.available;
        if (!item.available) row["reason"] = item.unavailableReason;
        row["use"] = useOp(item.kind, item.ref);
        return row;
    }

    std::string colorsOf(const titleart::Json& material) {
        if (!material.is_object() || !material.contains("fill")) return "default";
        const titleart::Json& fill = material["fill"];
        const std::string kind = str(fill, "kind", "solid");
        if (kind == "solid") return str(fill, "color", "#ffffff");
        size_t stops = fill.contains("gradient") && fill["gradient"].contains("stops") ? fill["gradient"]["stops"].size() : 0;
        return kind + " " + std::to_string(stops) + " stops";
    }

    // §6.15's structure line ("face: chrome, bevel round 3 %; bands: 2; effects: outer_glow ...")
    std::string structureOf(tl::LibraryKind kind, const titleart::Json& j) {
        std::string out;
        auto effects = [](const titleart::Json& list) {
            std::string s;
            if (list.is_array()) for (const titleart::Json& e : list) s += (s.empty() ? "" : ", ") + str(e, "type");
            return s;
        };
        if (kind == tl::LibraryKind::Look) {
            if (j.contains("style") && j["style"].is_object()) {
                const titleart::Json& st = j["style"];
                out += "face: " + colorsOf(st.contains("face") ? st["face"] : titleart::Json());
                if (st.contains("face") && st["face"].contains("bevel")) out += ", bevel " + str(st["face"]["bevel"], "profile", "smooth");
                const size_t rims = st.contains("rims") ? st["rims"].size() : 0;
                out += "; rims: " + std::to_string(rims);
                if (st.value("knockout", false)) out += "; knockout";
            }
            if (j.contains("style_3d") && j["style_3d"].is_object()) {
                const titleart::Json& st = j["style_3d"];
                out += "3D " + str(st, "mode", "extrude") + "; face: " + (st.contains("face") ? str(st["face"], "preset", "custom") : std::string("default"));
                out += "; bands: " + std::to_string(st.contains("bands") ? st["bands"].size() : 0);
            }
            const std::string fx = effects(j.contains("effects") ? j["effects"] : titleart::Json());
            if (!fx.empty()) out += "; effects: " + fx;
            std::string gens;
            if (j.contains("generators")) for (const titleart::Json& g : j["generators"]) gens += (gens.empty() ? "" : ", ") + str(g, "type");
            if (!gens.empty()) out += "; generators: " + gens;
            if (j.contains("lighting") && j["lighting"].contains("environment")) out += "; environment: " + str(j["lighting"]["environment"], "preset", "studio");
        } else if (kind == tl::LibraryKind::ColorScheme) {
            const titleart::Json& p = j.contains("paints") ? j["paints"] : titleart::Json::object();
            out += "face: " + colorsOf(p.contains("face") ? p["face"] : titleart::Json());
            out += "; accents: " + std::to_string(p.contains("accents") ? p["accents"].size() : 0);
            out += "; glow " + str(j, "glow", "#ffffff") + ", shadow " + str(j, "shadow", "#000000");
            if (j.value("spectrum", false)) out += "; spectrum";
        } else {
            size_t layers = 0;
            titleart::forEachLayer(j, [&](const titleart::Json&, const titleart::Json*, int) {
                layers++;
                return true;
            });
            out += std::to_string(layers) + " layers; canvas " + std::to_string(j["canvas"].value("width", 1920)) + " x " + std::to_string(j["canvas"].value("height", 1080));
        }
        return out;
    }

    const char* contractLine(tl::LibraryKind kind) {
        switch (kind) {
            case tl::LibraryKind::Look:
                return "replaces style (face, rims/bands, bevel), effects, part effects, its own generator layers; keeps text, font (unless apply_font), "
                       "size, transform, masks, blend, warp; re-applies the layer's Color Scheme";
            case tl::LibraryKind::ColorScheme:
                return "recolours by role (face, dark keyline, accents, base, glows, shadows, light colours); keeps every structure, width, bevel, effect";
            case tl::LibraryKind::Template:
                return "replaces the document's content; keeps its file, GUID, name and non-default outputs; carries exposed texts with keep_text";
        }
        return "";
    }

    Json detailOf(const tl::LibraryItemInfo& item) {
        const titleart::Json j = tl::TitleLibrary::get().itemJson(item);
        const titleart::Json& identity = item.kind == tl::LibraryKind::Template && j.contains("template") ? j["template"] : j;
        Json d = Json::object();
        d["id"] = item.ref;
        d["kind"] = tl::libraryKindName(item.kind);
        d["name"] = fromOrdered(item.name.toJson());
        d["description"] = fromOrdered(item.description.toJson());
        d["family"] = item.family;
        if (!item.dimension.empty()) d["dimension"] = item.dimension;
        if (item.kind == tl::LibraryKind::Look) d["ref_size"] = j.value("ref_size", 200.0);
        d["structure"] = structureOf(item.kind, j);
        if (identity.contains("font")) d["font"] = fromOrdered(identity["font"]);
        if (item.kind == tl::LibraryKind::Template && identity.contains("placeholders")) {
            Json ph = Json::array();
            for (const titleart::Json& p : identity["placeholders"]) ph.push_back(Json{{"name", str(p, "name")}, {"sample", fromOrdered(p["sample"])}});
            d["placeholders"] = ph;
        }
        d["contract"] = contractLine(item.kind);
        d["requires"] = item.requirements;
        d["available"] = item.available;
        if (!item.available) d["reason"] = item.unavailableReason;
        for (const char* key : {"lint_allow", "check_allow"}) if (identity.contains(key) && !identity[key].empty()) d[key] = fromOrdered(identity[key]);
        if (identity.contains("provenance")) d["provenance"] = fromOrdered(identity["provenance"]);
        d["scope"] = tl::libraryScopeName(item.scope);
        d["use"] = useOp(item.kind, item.ref);
        return d;
    }

    // ---- sheets ----

    // 3 x 5 digits, scaled: the cell numbers (no font needed)
    const char* const DIGITS[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                                    "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"};

    void stampNumber(texsynth::Image& image, int x0, int y0, int n) {
        const std::string text = std::to_string(n);
        const int scale = 3, w = (int)text.size() * 4 * scale + scale, h = 7 * scale;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int px = x0 + x, py = y0 + y;
                if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
                uint8_t* p = &image.data[((size_t)py * image.width + px) * 4];
                p[0] = p[1] = p[2] = 0;
                p[3] = 220;
            }
        }
        for (size_t c = 0; c < text.size(); c++) {
            const char* g = DIGITS[text[c] - '0'];
            for (int gy = 0; gy < 5; gy++) {
                for (int gx = 0; gx < 3; gx++) {
                    if (g[gy * 3 + gx] != '1') continue;
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            const int px = x0 + scale + (int)c * 4 * scale + gx * scale + sx, py = y0 + scale + gy * scale + sy;
                            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
                            uint8_t* p = &image.data[((size_t)py * image.width + px) * 4];
                            p[0] = p[1] = p[2] = 255;
                            p[3] = 255;
                        }
                    }
                }
            }
        }
    }

    // the image fitted into a w x h cell, centred (straight RGBA8)
    texsynth::Image fitCell(const texsynth::Image& src, int w, int h) {
        texsynth::Image out = texsynth::Image::make(w, h, 4, 8);
        if (src.width <= 0 || src.height <= 0) return out;
        const double s = std::min((double)w / src.width, (double)h / src.height);
        const int dw = std::max(1, (int)std::lround(src.width * s)), dh = std::max(1, (int)std::lround(src.height * s));
        const int ox = (w - dw) / 2, oy = (h - dh) / 2;
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                const int sx = std::min(src.width - 1, (int)(x / s)), sy = std::min(src.height - 1, (int)(y / s));
                const uint8_t* p = &src.data[((size_t)sy * src.width + sx) * 4];
                std::copy(p, p + 4, &out.data[((size_t)(y + oy) * w + x + ox) * 4]);
            }
        }
        return out;
    }

    struct SheetState {
        Project* project = nullptr;
        std::vector<const tl::LibraryItemInfo*> items;
        std::string text;
        std::string colorScheme;
        std::shared_ptr<const titleart::TitleArtDoc> base;     // path + layer: the agent's document
        std::string layer;
        int cellW = 320, cellH = 160, columns = 3;
        SheetBackground background;
        std::vector<texsynth::Image> cells;
        Json numbered = Json::array();
        size_t next = 0;
        double ms = 0;
    };

    // the document a cell renders: the item applied to the agent's layer or to a text of its own
    bool cellDocument(SheetState& s, const tl::LibraryItemInfo& item, titleart::TitleArtDoc& out, std::map<std::string, std::string>& transient,
                      std::string& error) {
        titleart::TitleArtDoc doc;
        std::string layerRef = s.layer.empty() ? std::string("Title") : s.layer;
        if (s.base) {
            doc = *s.base;
        } else {
            titleart::NewTitleArtOptions o;
            o.width = s.cellW * 4;
            o.height = s.cellH * 4;
            o.text = s.text.empty() ? std::string("HERO") : s.text;
            doc = titleart::makeNewTitleArt(o);
        }
        titleart::Json ops = titleart::Json::array();
        if (item.kind == tl::LibraryKind::Look) {
            ops.push_back(titleart::Json{{"op", "apply_look"}, {"layer", layerRef}, {"look", item.ref}, {"apply_font", !s.base}});
        } else if (item.kind == tl::LibraryKind::ColorScheme) {
            if (!s.base) {
                const std::string previewLook = str(tl::TitleLibrary::get().manifest(), "scheme_preview_look", "casino/outline-gold");
                ops.push_back(titleart::Json{{"op", "apply_look"}, {"layer", layerRef}, {"look", previewLook}, {"apply_font", true}});
            }
            ops.push_back(titleart::Json{{"op", "apply_color_scheme"}, {"layer", layerRef}, {"color_scheme", item.ref}});
        } else {
            ops.push_back(titleart::Json{{"op", "apply_template"}, {"template", item.ref}, {"keep_text", !s.text.empty() || s.base != nullptr}});
            if (!s.text.empty() && !s.base) {
                // the agent's text goes into the template's title placeholder
                ops = titleart::Json::array({titleart::Json{{"op", "apply_template"}, {"template", item.ref}, {"keep_text", true}}});
            }
        }
        if (!s.colorScheme.empty() && item.kind != tl::LibraryKind::ColorScheme) {
            ops.push_back(titleart::Json{{"op", "apply_color_scheme"}, {"color_scheme", s.colorScheme}});
        }
        return tl::preview(doc, ops, out, &transient, &error);
    }

    bool sheetTick(SheetState& s, McpJob& job, FrameTask& task) {
        if (s.next < s.items.size()) {
            const tl::LibraryItemInfo& item = *s.items[s.next];
            texsynth::Image cell = texsynth::Image::make(s.cellW, s.cellH, 4, 8);
            titleart::TitleArtDoc doc;
            std::map<std::string, std::string> transient;
            std::string error;
            if (item.available && cellDocument(s, item, doc, transient, error)) {
                titledesigner::TransientAssetResolver assets;
                assets.setBase(TitleArtRenderHost::editor().resolver(s.project));
                for (const auto& [asset, file] : transient) assets.add(asset, file);
                titleart::RenderRequest r;
                r.doc = std::make_shared<const titleart::TitleArtDoc>(std::move(doc));
                r.pxScale = std::min((float)s.cellW / r.doc->canvasWidth(), (float)s.cellH / r.doc->canvasHeight());
                r.assets = &assets;
                TitleRenderedImage image;
                if (TitleArtRenderHost::editor().render(s.project, r, image, error)) {
                    s.ms += image.renderMs;
                    cell = fitCell(texsynth::toRgba8(image.image), s.cellW, s.cellH);
                } else {
                    s.numbered[s.next]["error"] = error;
                }
            } else {
                s.numbered[s.next]["error"] = item.available ? error : item.unavailableReason;
            }
            cell = composite(cell, s.background);
            stampNumber(cell, 2, 2, (int)s.next + 1);
            s.cells.push_back(std::move(cell));
            s.next++;
            job.setProgress((double)s.next / s.items.size(), "cell " + std::to_string(s.next) + " of " + std::to_string(s.items.size()));
            return false;
        }
        std::vector<SheetCell> boxes;
        texsynth::Image image = sheet(s.cells, s.columns, boxes);
        task.images.push_back(toolImage(image));
        Json result = Json{{"rows", s.numbered}, {"sheet", Json{{"width", image.width}, {"height", image.height}, {"columns", s.columns}}},
                           {"render_ms", std::lround(s.ms)}};
        task.text = std::to_string(s.items.size()) + " items rendered; cells numbered as the rows";
        job.succeed(task.text, result);
        return true;
    }

    bool startSheet(const CallPtr& call, tl::LibraryKind kind, std::vector<const tl::LibraryItemInfo*> items) {
        const Json& args = call->args();
        const Json& sheetArgs = args["sheet"];
        auto state = std::make_shared<SheetState>();
        state->project = call->ctx().project();
        if (sheetArgs.contains("ids") && sheetArgs["ids"].is_array()) {
            items.clear();
            for (const Json& id : sheetArgs["ids"]) {
                tl::LibraryError error;
                const tl::LibraryItemInfo* info = tl::TitleLibrary::get().info(kind, id.get<std::string>(), &error);
                if (!info) {
                    call->fail(error.message, Json{{"code", error.code}, {"suggestions", error.suggestions}, {"next", error.next}});
                    return true;
                }
                items.push_back(info);
            }
        }
        if (items.empty()) {
            call->fail("sheet: no items to render (give kind + query, or sheet.ids)");
            return true;
        }
        if (items.size() > SHEET_MAX_CELLS) items.resize(SHEET_MAX_CELLS);
        state->items = items;
        state->text = sheetArgs.value("text", std::string());
        state->colorScheme = sheetArgs.value("color_scheme", std::string());
        if (sheetArgs.contains("cell") && sheetArgs["cell"].is_array() && sheetArgs["cell"].size() == 2) {
            state->cellW = std::clamp(sheetArgs["cell"][0].get<int>(), 64, SHEET_MAX_WIDTH);
            state->cellH = std::clamp(sheetArgs["cell"][1].get<int>(), 32, SHEET_MAX_HEIGHT);
        }
        state->columns = std::clamp(sheetArgs.value("columns", 3), 1, 5);
        // the sheet fits 1024 x 576: cells shrink to fit the grid
        const int rows = (int)((items.size() + state->columns - 1) / state->columns);
        const int cols = std::min<int>(state->columns, (int)items.size());
        const double fit = std::min({1.0, (double)SHEET_MAX_WIDTH / (cols * (state->cellW + 4)), (double)SHEET_MAX_HEIGHT / (rows * (state->cellH + 4))});
        state->cellW = std::max(32, (int)(state->cellW * fit));
        state->cellH = std::max(16, (int)(state->cellH * fit));
        if (sheetArgs.contains("path")) {
            fs::path file;
            std::string relative, error;
            if (!resolveTitlePath(state->project, sheetArgs.value("path", std::string()), true, file, relative, error)) {
                call->fail("sheet.path: " + error);
                return true;
            }
            auto document = openTitle(state->project, file, error);
            if (!document) {
                call->fail("sheet.path: " + error);
                return true;
            }
            state->base = std::make_shared<const titleart::TitleArtDoc>(document->doc());
            state->layer = sheetArgs.value("layer", std::string());
            if (kind != tl::LibraryKind::Template && state->layer.empty()) {
                call->fail("sheet.layer: name the layer of " + relative + " to render the items on");
                return true;
            }
        }
        std::vector<SheetBackground> backgrounds;
        std::string error;
        const std::string bg = sheetArgs.value("background", std::string("checker"));
        if (!parseBackgrounds(state->project, Json::array({bg == "machine" ? std::string("#101018") : bg}), backgrounds, error) || backgrounds.empty()) {
            call->fail("sheet.background: " + error);
            return true;
        }
        state->background = backgrounds.front();
        for (size_t i = 0; i < items.size(); i++) state->numbered.push_back(rowOf(*items[i], (int)i + 1));
        auto task = std::make_shared<FrameTask>();
        task->tick = [state, task](McpJob& job) { return sheetTick(*state, job, *task); };
        runFrameTask(call, "title_library_sheet", "Title library sheet", task);
        return true;
    }

}

Json titletools::libraryCatalogue() {
    tl::TitleLibrary& lib = tl::TitleLibrary::get();
    Json out = Json::object();
    for (tl::LibraryKind kind : tl::LIBRARY_KINDS) {
        size_t total = 0, available = 0;
        Json families = Json::object();
        for (const tl::LibraryItemInfo& item : lib.items(kind)) {
            total++;
            if (item.available) available++;
            if (kind == tl::LibraryKind::Look) families[item.family] = families.value(item.family, 0) + 1;
        }
        Json k = Json{{"total", total}, {"available", available}};
        if (kind == tl::LibraryKind::Look) k["families"] = families;
        out[tl::libraryKindName(kind)] = k;
    }
    out["pack_fonts"] = tl::packFonts().size();
    out["next"] = "list_title_library {kind:\"looks\", query:\"gold\"}";
    return out;
}

std::vector<Json> titletools::packFontRows(const std::string& query) {
    std::vector<Json> rows;
    const std::string q = query;
    for (const tl::PackFont& f : tl::packFonts()) {
        std::string hay = f.id + " " + f.family + " " + f.category;
        std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::string lq = q;
        std::transform(lq.begin(), lq.end(), lq.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (!lq.empty() && hay.find(lq) == std::string::npos) continue;
        rows.push_back(Json{{"id", f.id}, {"family", f.family}, {"style", f.style}, {"scripts", f.scripts}, {"category", f.category}, {"pack", true},
                            {"path", std::string(tl::TITLE_FONTS_ASSET_DIR) + "/" + f.id + "/" + (f.files.empty() ? std::string() : f.files.front())}});
    }
    return rows;
}

bool titletools::listLibraryKind(const CallPtr& call) {
    const Json& args = call->args();
    tl::LibraryKind kind;
    if (!tl::libraryKindFromName(args.value("kind", std::string()), kind)) return false;
    if (Project* project = call->ctx().project()) tl::TitleLibrary::get().setProject(project->getProjectPath(), project->getAssetsPath());
    tl::TitleLibrary& lib = tl::TitleLibrary::get();
    const std::string id = args.value("id", std::string());
    if (!id.empty() && !args.contains("sheet")) {
        tl::LibraryError error;
        const tl::LibraryItemInfo* item = lib.info(kind, id, &error);
        if (!item) {
            call->fail(error.message, Json{{"code", error.code}, {"suggestions", error.suggestions}, {"next", error.next}});
            return true;
        }
        Json detail = detailOf(*item);
        if (error.deprecated) detail["deprecated"] = error.message;
        call->ok(item->name.any() + " (" + item->ref + ")", detail);
        return true;
    }
    tl::LibraryQuery query;
    query.kind = kind;
    query.text = args.value("query", std::string());
    query.family = args.value("family", std::string());
    query.dimension = args.value("dimension", std::string());
    if (args.contains("tags") && args["tags"].is_array()) for (const Json& t : args["tags"]) if (t.is_string()) query.tags.push_back(t.get<std::string>());
    const std::string scope = args.value("scope", std::string("all"));
    if (scope == "builtin") query.scope = tl::LibraryScope::BuiltIn;
    else if (scope == "project") query.scope = tl::LibraryScope::Project;
    else if (scope == "user") query.scope = tl::LibraryScope::User;
    else if (scope != "all") {
        call->fail("scope is builtin, project, user or all");
        return true;
    }
    if (!query.family.empty() && kind != tl::LibraryKind::ColorScheme && !lib.family(query.family) && query.family != "custom") {
        std::string list;
        for (const tl::LibraryFamily& f : lib.families()) list += (list.empty() ? "" : ", ") + f.id;
        call->fail("No family '" + query.family + "'. Families: " + list + ", custom.");
        return true;
    }
    if (args.value("favorites", false)) {
        query.favoritesOnly = true;
        for (const std::string& f : tl::LibraryPrefs::load().favorites) {
            const std::string prefix = std::string(tl::libraryKindName(kind)) + ":";
            if (f.rfind(prefix, 0) == 0) query.favorites.push_back(f.substr(prefix.size()));
        }
    }
    const std::vector<tl::LibraryHit> hits = tl::searchLibrary(query);
    if (args.contains("sheet")) {
        std::vector<const tl::LibraryItemInfo*> items;
        for (const tl::LibraryHit& h : hits) items.push_back(h.item);
        return startSheet(call, kind, items);
    }
    const int limit = std::clamp(args.value("limit", 20), 1, 48);
    const int offset = std::max(0, args.value("offset", 0));
    Json rows = Json::array();
    for (size_t i = (size_t)offset; i < hits.size() && rows.size() < (size_t)limit; i++) rows.push_back(rowOf(*hits[i].item, (int)i + 1));
    Json result = Json{{"kind", tl::libraryKindName(kind)}, {"rows", rows}, {"total", hits.size()}};
    if ((size_t)offset + rows.size() < hits.size()) result["next_offset"] = offset + (int)rows.size();
    std::string text = std::to_string(hits.size()) + " " + tl::libraryKindName(kind);
    if (!rows.empty()) text += ": " + rows[0]["id"].get<std::string>() + (rows.size() > 1 ? ", ..." : "");
    call->ok(text, result);
    return true;
}

bool titletools::editLibraryOnly(const CallPtr& call) {
    const Json& args = call->args();
    if (args.contains("path") || !args.contains("ops")) return false;
    const titleart::Json ops = toOrdered(args["ops"]);
    if (!tl::isLibraryOnlyBatch(ops)) return false;
    Project* project = call->ctx().project();
    TitleOpsResult result = tl::runLibraryBatch(project, ops);
    if (!result.ok) {
        call->fail(result.error(), fromOrdered(result.toJson()));
        return true;
    }
    call->ok(result.undoName, fromOrdered(result.toJson()));
    return true;
}

bool titletools::createFromLibrary(Project* project, const Json& create, titleart::TitleArtDoc& out, std::vector<TitleOpFile>& files,
                                   std::string& undoName, std::string& error) {
    if (project) tl::TitleLibrary::get().setProject(project->getProjectPath(), project->getAssetsPath());
    titleart::NewTitleArtOptions options;
    if (create.contains("text") && create["text"].is_string()) options.text = create["text"].get<std::string>();
    const titleart::TitleArtDoc start = titleart::makeNewTitleArt(options);
    titleart::Json ops = titleart::Json::array();
    if (create.contains("template")) {
        titleart::Json op = titleart::Json{{"op", "apply_template"}, {"template", create.value("template", std::string())},
                                           {"keep_text", create.contains("text")}};
        if (create.contains("language")) op["language"] = create.value("language", std::string());
        ops.push_back(op);
    } else {
        ops.push_back(titleart::Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", create.value("look", std::string())}, {"apply_font", true}});
    }
    TitleOpsResult result = TitleArtOps::apply(start, ops);
    if (!result.ok) {
        error = result.error();
        return false;
    }
    out = std::move(result.doc);
    files = std::move(result.files);
    const std::string ref = create.contains("template") ? create.value("template", std::string()) : create.value("look", std::string());
    const tl::LibraryItemInfo* info = tl::TitleLibrary::get().info(create.contains("template") ? tl::LibraryKind::Template : tl::LibraryKind::Look, ref, nullptr);
    undoName = std::string(create.contains("template") ? "Create Title Art from Template " : "Create Title Art from Look ") + (info ? info->name.any() : ref);
    return true;
}
