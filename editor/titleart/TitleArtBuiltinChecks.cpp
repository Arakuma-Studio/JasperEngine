// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtBuiltinChecks.h"
#include "fx/FxGeneratorChecks.h"
#include "titleart/runtime/TitleArtRuntimeChecks.h"

#include "TitleArtBake.h"
#include "TitleArtRenderHost.h"
#include "runtime/TitleCharacterChecks.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "util/FontFace.h"
#include "util/ImageDiff.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;
using titleart::TitleRectI;
namespace fs = std::filesystem;

namespace {

    using Out = std::vector<TitleCheckResult>;

    std::string str(const TJson& object, const char* key) {
        auto it = object.find(key);
        return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    TitleCheckResult result(TitleCheckStatus status, const std::string& message, const std::string& method) {
        TitleCheckResult r;
        r.status = status;
        r.message = message;
        r.method = method;
        return r;
    }

    TJson rect(const TitleRectI& r) {
        return TJson::array({r.x, r.y, r.width, r.height});
    }

    std::string layerRef(const std::string& id) {
        return "id:" + id;
    }

    std::string layerName(const titleart::TitleArtDoc& doc, const std::string& id) {
        const TJson* layer = titleart::findLayer(doc.root, id);
        return layer ? str(*layer, "name") : id;
    }

    // "layers[1].children[0].style.face.reflection.map" -> the layer's id and "style.face.reflection.map"
    bool splitLayerPath(const TJson& root, const std::string& path, std::string& layerId, std::string& field) {
        std::vector<titleart::FieldSegment> segments;
        if (!titleart::parseFieldPath(path, segments, nullptr)) return false;
        const TJson* layer = nullptr;
        size_t i = 0;
        if (segments.size() >= 2 && segments[0].key == "layers" && segments[1].isIndex) {
            const TJson& layers = root.contains("layers") ? root["layers"] : TJson::array();
            if (segments[1].index >= layers.size()) return false;
            layer = &layers[segments[1].index];
            i = 2;
            while (i + 1 < segments.size() && segments[i].key == "children" && segments[i + 1].isIndex) {
                const TJson& children = titleart::layerChildren(*layer);
                if (segments[i + 1].index >= children.size()) return false;
                layer = &children[segments[i + 1].index];
                i += 2;
            }
        }
        layerId = layer ? str(*layer, "id") : std::string();
        field = titleart::formatFieldPath(std::vector<titleart::FieldSegment>(segments.begin() + static_cast<long>(i), segments.end()));
        return true;
    }

    // ---- pixels ----

    texsynth::Image asImage(const TitleCheckImage& image) {
        texsynth::Image out;
        out.width = image.width;
        out.height = image.height;
        out.channels = 4;
        out.bitDepth = 8;
        out.data = image.rgba;
        return out;
    }

    uint8_t alphaAt(const TitleCheckImage& image, int x, int y) {
        return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3];
    }

    TitleRectI alphaBox(const TitleCheckImage& image, int threshold) {
        int minX = image.width, minY = image.height, maxX = -1, maxY = -1;
        for (int y = 0; y < image.height; y++) {
            for (int x = 0; x < image.width; x++) {
                if (alphaAt(image, x, y) < threshold) continue;
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
        }
        if (maxX < 0) return {};
        return {minX, minY, maxX - minX + 1, maxY - minY + 1};
    }

    // image pixels -> canvas pixels (image space)
    TitleRectI toCanvas(const TitleCheckImage& image, const TitleRectI& r) {
        if (image.width <= 0 || image.frame.empty()) return r;
        const double sx = static_cast<double>(image.frame.width) / image.width, sy = static_cast<double>(image.frame.height) / image.height;
        const int x0 = image.frame.x + static_cast<int>(std::floor(r.x * sx)), y0 = image.frame.y + static_cast<int>(std::floor(r.y * sy));
        const int x1 = image.frame.x + static_cast<int>(std::ceil((r.x + r.width) * sx)), y1 = image.frame.y + static_cast<int>(std::ceil((r.y + r.height) * sy));
        return {x0, y0, x1 - x0, y1 - y0};
    }

    std::string utf8Of(uint32_t cp) {
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        return out;
    }

    TitleRectI grow(const TitleRectI& r, int by, const TitleRectI& within) {
        const int x0 = std::max(within.x, r.x - by), y0 = std::max(within.y, r.y - by);
        const int x1 = std::min(within.x + within.width, r.x + r.width + by), y1 = std::min(within.y + within.height, r.y + r.height + by);
        return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    }

    double srgbToL(double r, double g, double b) {
        const double y = 0.2126 * titleart::srgbToLinear(static_cast<float>(r)) + 0.7152 * titleart::srgbToLinear(static_cast<float>(g)) +
                         0.0722 * titleart::srgbToLinear(static_cast<float>(b));
        return y > 216.0 / 24389.0 ? 116.0 * std::cbrt(y) - 16.0 : y * 24389.0 / 27.0;
    }

    bool backgroundColor(const std::string& name, titleart::TitleColor& out) {
        if (name == "black") { out = {0, 0, 0}; return true; }
        if (name == "white") { out = {1, 1, 1}; return true; }
        return titleart::parseTitleColor(TJson(name), out);
    }

    // The renders of one check run, shared by the checks that need the same one
    class Renders {
    public:
        explicit Renders(TitleCheckContext& context) : ctx(context) {
            snapshot = std::make_shared<titleart::TitleArtDoc>(context.doc);
            const int side = std::max(context.doc.canvasWidth(), context.doc.canvasHeight());
            scale = std::min(1.0f, 2048.0f / static_cast<float>(std::max(side, 1))) * context.options.renderScale;
        }

        titleart::RenderRequest request() const {
            titleart::RenderRequest r;
            r.doc = snapshot;
            r.pxScale = scale;
            r.assets = ctx.assets;
            return r;
        }

        // a render by key (cached for this run unless `fresh`); false with `error`
        const TitleCheckImage* get(const std::string& key, const titleart::RenderRequest& request, std::string& error, bool fresh = false) {
            if (!fresh) {
                auto it = cache.find(key);
                if (it != cache.end()) return &it->second;
            }
            TitleCheckImage image;
            if (!ctx.render(request, image, error)) return nullptr;
            if (fresh) {
                freshImages.push_back(std::make_unique<TitleCheckImage>(std::move(image)));
                return freshImages.back().get();
            }
            return &(cache[key] = std::move(image));
        }
        const TitleCheckImage* base(std::string& error) { return get("base", request(), error); }
        const TitleCheckImage* isolated(const std::string& layerId, std::string& error) {
            titleart::RenderRequest r = request();
            r.isolateLayer = layerId;
            return get("isolated:" + layerId, r, error);
        }

        TitleCheckContext& ctx;
        std::shared_ptr<titleart::TitleArtDoc> snapshot;
        float scale = 1.0f;

    private:
        std::map<std::string, TitleCheckImage> cache;
        std::vector<std::unique_ptr<TitleCheckImage>> freshImages;
    };

    // one Renders per check run: the checks of one context share it (TitleCheckContext::renders: a
    // later call, whatever its options, starts with none)
    Renders& rendersOf(TitleCheckContext& context) {
        auto* renders = static_cast<Renders*>(context.renders.get());
        if (!renders) {
            auto made = std::make_shared<Renders>(context);
            renders = made.get();
            context.renders = std::move(made);
        }
        return *renders;
    }

    // a pixel check whose render failed (a run without a GPU never gets here: TitleArtChecks skips it)
    void renderFailed(Out& out, const std::string& error) {
        out.push_back(result(TitleCheckStatus::Fail, "the render failed: " + error, "render"));
    }

    std::vector<std::string> visibleTopLayers(const titleart::TitleArtDoc& doc, size_t cap) {
        std::vector<std::string> out;
        for (const TJson& layer : doc.layers()) {
            if (!layer.value("visible", true) || layer.value("guide", false)) continue;
            out.push_back(str(layer, "id"));
            if (out.size() >= cap) break;
        }
        return out;
    }

    // ---- document checks ----

    void documentValid(TitleCheckContext& ctx, Out& out) {
        if (!ctx.versionError.empty()) {
            TitleCheckResult r = result(TitleCheckStatus::Fail, "version_newer: " + ctx.versionError + "; it is read-only here", "parse");
            r.measured = TJson{{"version_newer", true}};
            out.push_back(std::move(r));
            return;
        }
        TJson unknown = TJson::array();
        for (const titleart::TitleProblem& p : titleart::titleArtDocumentProblems(ctx.doc)) {
            if (p.code == "unknown_type") unknown.push_back(p.toJson());
        }
        TJson warnings = TJson::array();
        for (const titleart::TitleWarning& w : ctx.loadWarnings) warnings.push_back(w.toString());
        TitleCheckResult r = result(unknown.empty() && warnings.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, "", "parse and normalize");
        r.measured = TJson{{"unknown_types", unknown.size()}, {"normalize_warnings", warnings.size()}};
        r.threshold = TJson{{"unknown_types", 0}, {"normalize_warnings", 0}};
        if (r.status == TitleCheckStatus::Pass) {
            r.message = "the document parses with no warnings and no unknown types";
        } else {
            r.message = std::to_string(unknown.size()) + " object(s) of an unknown type (kept, drawn as nothing) and " +
                        std::to_string(warnings.size()) + " value(s) the reader fixed";
            r.measured["details"] = TJson{{"unknown", unknown}, {"warnings", warnings}};
        }
        out.push_back(std::move(r));
    }

    void assetsPresent(TitleCheckContext& ctx, Out& out) {
        if (!ctx.assets) {
            out.push_back(result(TitleCheckStatus::Skip, "no asset resolver: not measured", "resolve"));
            return;
        }
        const std::vector<titleart::TitleProblem> missing = titleArtMissingAssets(ctx.doc, *ctx.assets);
        TitleCheckResult r = result(missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "", "every AssetRef resolved by GUID, then path");
        r.measured = TJson{{"missing", missing.size()}};
        r.threshold = TJson{{"missing", 0}};
        if (missing.empty()) {
            r.message = "every font, image, SVG and map resolves";
            out.push_back(std::move(r));
            return;
        }
        TJson list = TJson::array();
        TJson ops = TJson::array();
        const auto* editorAssets = dynamic_cast<const EditorTitleAssetResolver*>(ctx.assets);
        for (const titleart::TitleProblem& p : missing) {
            list.push_back(p.toJson());
            if (!editorAssets) continue;
            titleart::TitleAssetKind kind = titleart::TitleAssetKind::Image;
            const std::string kindName = p.detail.value("kind", std::string("image"));
            if (kindName == "font") kind = titleart::TitleAssetKind::Font;
            else if (kindName == "vector") kind = titleart::TitleAssetKind::Vector;
            else if (kindName == "environment") kind = titleart::TitleAssetKind::Environment;
            const std::string wanted = fs::path(p.detail.value("path", std::string())).stem().string();
            std::vector<std::string> names;
            std::map<std::string, titleart::ResolvedTitleAsset> byName;
            for (const titleart::ResolvedTitleAsset& a : editorAssets->projectAssets(kind)) {
                const std::string stem = fs::path(a.assetPath).stem().string();
                names.push_back(stem);
                byName[stem] = a;
            }
            const std::string nearest = titleart::nearestName(wanted, names);
            if (nearest.empty()) continue;
            const titleart::ResolvedTitleAsset& a = byName[nearest];
            TJson op = TJson{{"op", "set"}, {"values", TJson{{p.path, TJson{{"path", a.assetPath}, {"guid", a.guid}}}}}};
            if (!p.layer.empty()) op["layer"] = layerRef(p.layer);
            ops.push_back(std::move(op));
        }
        r.measured["details"] = list;
        r.message = std::to_string(missing.size()) + " reference(s) do not resolve: " + missing.front().message;
        if (!ops.empty()) r.fix = TJson{{"ops", ops}};
        out.push_back(std::move(r));
    }

    void glyphsPresent(TitleCheckContext& ctx, Out& out) {
        if (!ctx.assets) {
            out.push_back(result(TitleCheckStatus::Skip, "no asset resolver: not measured", "font coverage"));
            return;
        }
        const std::vector<titleart::TitleProblem> missing = titleArtMissingGlyphs(ctx.doc, *ctx.assets);
        size_t count = 0;
        for (const titleart::TitleProblem& p : missing) count += p.detail.contains("chars") ? p.detail["chars"].size() : 0;
        TitleCheckResult r = result(missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "",
                                    "each code point against the font, the fallback fonts, then the built-in face");
        r.measured = TJson{{"missing", count}};
        r.threshold = TJson{{"missing", 0}};
        if (missing.empty()) {
            r.message = "every character has a glyph";
            out.push_back(std::move(r));
            return;
        }
        TJson list = TJson::array(), ops = TJson::array();
        for (const titleart::TitleProblem& p : missing) {
            list.push_back(p.toJson());
            const TJson& fonts = p.detail.contains("fonts_with_it") ? p.detail["fonts_with_it"] : TJson::array();
            if (fonts.empty()) continue;
            const TJson* layer = titleart::findLayer(ctx.doc.root, p.layer);
            TJson fallbacks = layer && layer->contains("fallback_fonts") ? (*layer)["fallback_fonts"] : TJson::array();
            fallbacks.push_back(TJson{{"path", fonts[0].value("path", std::string())}, {"guid", fonts[0].value("guid", std::string())}});
            ops.push_back(TJson{{"op", "set"}, {"layer", layerRef(p.layer)}, {"values", TJson{{"fallback_fonts", fallbacks}}}});
        }
        r.measured["details"] = list;
        r.message = missing.front().message;
        if (!ops.empty()) r.fix = TJson{{"ops", ops}};
        out.push_back(std::move(r));
    }

    // mitred rims whose joins at sharp notches reach through the stroke as spikes (Canvas2D draws
    // them too: geometry::mitreSpike); the fix rounds that rim's joins
    void rimSpikes(TitleCheckContext& ctx, Out& out) {
        if (!ctx.assets) {
            out.push_back(result(TitleCheckStatus::Skip, "no asset resolver: not measured", "rim mitres against the outline"));
            return;
        }
        titleart::geometry::TitleGeometry geometry(ctx.assets);
        TJson found = TJson::array(), ops = TJson::array();
        double worstRatio = 0.0;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            const titleart::LayerType type = titleart::layerTypeOf(layer);
            if (type != titleart::LayerType::Text && type != titleart::LayerType::Shape && type != titleart::LayerType::Vector) return true;
            const titleart::geometry::LayerElements elements = geometry.elements(layer, ctx.doc);
            if (elements.fromAlpha) return true;
            const TJson& rims = layer.value("style", TJson::object()).value("rims", TJson::array());
            double inner = 0.0;
            for (const titleart::geometry::TitleElement& e : elements.elements) {
                if (e.kind != titleart::geometry::ElementKind::Rim) continue;
                const double band = e.grow - inner;
                inner = e.grow;
                const titleart::geometry::MitreSpike spike = titleart::geometry::mitreSpike(e);
                if (!(band > 0) || spike.length <= 0.0) continue;
                const double ratio = spike.length / band;
                worstRatio = std::max(worstRatio, ratio);
                if (ratio <= titleart::geometry::MITRE_SPIKE_WARN_RATIO) continue;
                int index = -1;
                for (size_t i = 0; i < rims.size(); i++) {
                    if (rims[i].is_object() && rims[i].value("id", std::string()) == e.rimId) index = (int)i;
                }
                if (index < 0) continue;
                const std::string id = str(layer, "id");
                found.push_back(TJson{{"layer", id}, {"rim", index}, {"spike", std::round(spike.length * 10.0) / 10.0},
                                      {"band", std::round(band * 10.0) / 10.0}, {"at", TJson::array({spike.at.x, spike.at.y})}});
                ops.push_back(TJson{{"op", "set"}, {"layer", layerRef(id)}, {"values", TJson{{"rim#" + std::to_string(index) + ".join", "round"}}}});
            }
            return true;
        });
        TitleCheckResult r = result(found.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, "",
                                    "each mitred rim's joins: how far a mitre at a concave vertex reaches beyond the rim's band, "
                                    "against the rim's own width (layer units)");
        r.measured = TJson{{"spike_to_band", std::round(worstRatio * 100.0) / 100.0}, {"rims", found}};
        r.threshold = TJson{{"spike_to_band", titleart::geometry::MITRE_SPIKE_WARN_RATIO}};
        if (found.empty()) {
            r.message = "no rim mitre reaches past its band";
        } else {
            const TJson& first = found[0];
            r.message = "layer " + layerName(ctx.doc, first["layer"].get<std::string>()) + ", rim " + std::to_string(first["rim"].get<int>()) +
                        ": mitres at sharp notches stick out " + std::to_string((int)std::lround(first["spike"].get<double>())) +
                        " units past the rim (as Canvas2D draws them; Pachislop's references keep mitres): round joins remove them";
            r.fix = TJson{{"ops", ops}};
        }
        out.push_back(std::move(r));
    }

    void exposedTargets(TitleCheckContext& ctx, Out& out) {
        std::vector<titleart::TitleProblem> missing;
        for (const titleart::TitleProblem& p : titleart::titleArtDocumentProblems(ctx.doc)) {
            if (p.code == "exposed_target_missing") missing.push_back(p);
        }
        TitleCheckResult r = result(missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "", "each exposed target's layer and field resolved");
        r.measured = TJson{{"missing", missing.size()}};
        r.threshold = TJson{{"missing", 0}};
        if (missing.empty()) {
            r.message = "every exposed parameter reaches its targets";
            out.push_back(std::move(r));
            return;
        }
        TJson list = TJson::array(), ops = TJson::array();
        std::set<std::string> names;
        const TJson& exposed = ctx.doc.root.contains("exposed") ? ctx.doc.root["exposed"] : TJson::array();
        for (const titleart::TitleProblem& p : missing) {
            list.push_back(p.toJson());
            // "exposed[i].targets[t]"
            std::vector<titleart::FieldSegment> segments;
            if (titleart::parseFieldPath(p.path, segments, nullptr) && segments.size() >= 2 && segments[1].isIndex && segments[1].index < exposed.size()) {
                const std::string name = str(exposed[segments[1].index], "name");
                if (!name.empty() && names.insert(name).second) ops.push_back(TJson{{"op", "unexpose"}, {"name", name}});
            }
        }
        r.measured["details"] = list;
        r.message = missing.front().message;
        if (!ops.empty()) r.fix = TJson{{"ops", ops}};
        out.push_back(std::move(r));
    }

    void bakeFresh(TitleCheckContext& ctx, Out& out) {
        if (!ctx.assets || ctx.file.empty()) {
            out.push_back(result(TitleCheckStatus::Skip, "no document file: not measured", "recipes"));
            return;
        }
        const TitleBakeStatus status = titleArtBakeStatus(ctx.file, ctx.doc, *ctx.assets);
        TJson outputs = TJson::array();
        size_t notFresh = 0;
        std::string first;
        for (const TitleOutputState& o : status.outputs) {
            TJson item = TJson{{"id", o.id}, {"status", titleOutputStatusName(o.status)}};
            if (!o.changed.empty()) item["changed"] = o.changed;
            outputs.push_back(item);
            if (o.status != TitleOutputStatus::Fresh) {
                notFresh++;
                if (first.empty()) first = "output '" + o.id + "' is " + titleOutputStatusName(o.status) + (o.reason.empty() ? "" : " (" + o.reason + ")");
            }
        }
        TitleCheckResult r = result(notFresh == 0 ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, "",
                                    "each output PNG's jasper.titleart recipe against the document, asset hashes and bake version");
        r.measured = TJson{{"outputs", outputs}, {"not_fresh", notFresh}};
        r.threshold = TJson{{"not_fresh", 0}};
        r.message = notFresh == 0 ? "every output is fresh" : first;
        if (notFresh) r.fix = TJson{{"tool", "export_title"}, {"arguments", TJson{{"path", ctx.path}}}};
        out.push_back(std::move(r));
    }

    // ---- pixel checks ----

    bool isFlat(const TitleCheckImage& image) {
        if (image.rgba.empty()) return true;
        const uint8_t* first = image.rgba.data();
        bool anyAlpha = false;
        bool uniform = true;
        for (size_t i = 0; i < image.rgba.size(); i += 4) {
            if (image.rgba[i + 3] > 0) anyAlpha = true;
            if (uniform && !std::equal(first, first + 4, image.rgba.data() + i)) uniform = false;
        }
        return !anyAlpha || uniform;
    }

    void notFlat(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        const bool flat = isFlat(*base);
        TitleCheckResult r = result(flat ? TitleCheckStatus::Fail : TitleCheckStatus::Pass, "", "the render's pixels: any alpha, more than one colour");
        r.measured = TJson{{"flat", flat}};
        r.threshold = TJson{{"flat", false}};
        if (!flat) {
            r.message = "the render shows something";
            out.push_back(std::move(r));
            return;
        }
        std::vector<std::string> empty;
        for (const std::string& id : visibleTopLayers(ctx.doc, 8)) {
            const TitleCheckImage* alone = renders.isolated(id, error);
            if (alone && alphaBox(*alone, 1).empty()) empty.push_back(layerName(ctx.doc, id));
        }
        r.message = "the render is blank or one colour";
        if (!empty.empty()) {
            std::string names;
            for (const std::string& n : empty) names += (names.empty() ? "" : ", ") + n;
            r.message += "; layers that draw nothing: " + names;
            r.measured["layers_drawing_nothing"] = empty;
        }
        out.push_back(std::move(r));
    }

    // does anything but a generator reach the canvas edge in this layer? A group is judged by its
    // children: its generators may bleed, its text may not
    bool contentReachesEdge(const titleart::TitleArtDoc& doc, Renders& renders, const std::string& id, int cw, int ch) {
        const TJson* layer = titleart::findLayer(doc.root, id);
        if (!layer) return true;
        const titleart::LayerType type = titleart::layerTypeOf(*layer);
        if (type == titleart::LayerType::Generator) return false;
        if (type == titleart::LayerType::Group && layer->contains("children")) {
            for (const TJson& child : (*layer)["children"]) {
                if (child.value("visible", true) && contentReachesEdge(doc, renders, str(child, "id"), cw, ch)) return true;
            }
            return false;
        }
        std::string error;
        const TitleCheckImage* alone = renders.isolated(id, error);
        if (!alone) return true;
        const TitleRectI b = alphaBox(*alone, 3);
        if (b.empty()) return false;
        const TitleRectI c = toCanvas(*alone, b);
        return c.x <= 0 || c.y <= 0 || c.x + c.width >= cw || c.y + c.height >= ch;
    }

    void notClipped(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        const int w = base->width, h = base->height;
        const TitleRectI box = alphaBox(*base, 3);
        TitleCheckResult r = result(TitleCheckStatus::Pass, "", "alpha > 2/255 within 1 px of each canvas edge; margins from the alpha box");
        r.threshold = TJson{{"margin_px", 8}};
        if (box.empty()) {
            r.message = "nothing visible";
            r.measured = TJson{{"touching", TJson::array()}};
            out.push_back(std::move(r));
            return;
        }
        const TitleRectI canvasBox = toCanvas(*base, box);
        const int cw = ctx.doc.canvasWidth(), ch = ctx.doc.canvasHeight();
        const TJson margins = TJson{{"left", canvasBox.x}, {"top", canvasBox.y}, {"right", cw - canvasBox.x - canvasBox.width},
                                  {"bottom", ch - canvasBox.y - canvasBox.height}};
        std::vector<std::string> touching;
        if (box.x <= 0) touching.push_back("left");
        if (box.y <= 0) touching.push_back("top");
        if (box.x + box.width >= w) touching.push_back("right");
        if (box.y + box.height >= h) touching.push_back("bottom");
        int minMargin = std::numeric_limits<int>::max();
        for (auto it = margins.begin(); it != margins.end(); ++it) minMargin = std::min(minMargin, it->get<int>());
        r.measured = TJson{{"margins", margins}, {"touching", touching}, {"min_margin_px", minMargin}};
        const TitleRectI canvas{0, 0, cw, ch};
        for (const std::string& side : touching) {
            if (side == "left") r.at.push_back(grow({0, canvasBox.y, 1, canvasBox.height}, 32, canvas));
            if (side == "top") r.at.push_back(grow({canvasBox.x, 0, canvasBox.width, 1}, 32, canvas));
            if (side == "right") r.at.push_back(grow({cw - 1, canvasBox.y, 1, canvasBox.height}, 32, canvas));
            if (side == "bottom") r.at.push_back(grow({canvasBox.x, ch - 1, canvasBox.width, 1}, 32, canvas));
        }
        if (!touching.empty()) {
            r.status = TitleCheckStatus::Fail;
            std::vector<std::string> culprits;
            bool generatorsOnly = true;
            for (const std::string& id : visibleTopLayers(ctx.doc, 8)) {
                const TitleCheckImage* alone = renders.isolated(id, error);
                if (!alone) continue;
                const TitleRectI b = alphaBox(*alone, 3);
                if (b.empty()) continue;
                const TitleRectI c = toCanvas(*alone, b);
                if (c.x <= 0 || c.y <= 0 || c.x + c.width >= cw || c.y + c.height >= ch) {
                    culprits.push_back(layerName(ctx.doc, id));
                    const bool fillsCanvas = c.x <= 0 && c.y <= 0 && c.x + c.width >= cw && c.y + c.height >= ch;
                    if (!fillsCanvas && contentReachesEdge(ctx.doc, renders, id, cw, ch)) generatorsOnly = false;
                }
            }
            std::string sides, names;
            for (const std::string& s : touching) sides += (sides.empty() ? "" : ", ") + s;
            for (const std::string& n : culprits) names += (names.empty() ? "" : ", ") + n;
            if (generatorsOnly) {
                r.status = TitleCheckStatus::Pass;
                r.message = "only full-bleed layers (generators, a layer filling the canvas) reach the canvas edge" + (names.empty() ? std::string() : " (" + names + ")") + ": they are full-bleed by design";
                r.measured["layers"] = culprits;
                r.at.clear();
                out.push_back(std::move(r));
                return;
            }
            r.message = "ink or light touches the canvas edge (" + sides + ")" + (names.empty() ? "" : " from " + names) +
                        ": it is cut there in every output";
            if (!culprits.empty()) r.measured["layers"] = culprits;
            const bool horizontal = std::find(touching.begin(), touching.end(), "left") != touching.end() ||
                                    std::find(touching.begin(), touching.end(), "right") != touching.end();
            const bool vertical = std::find(touching.begin(), touching.end(), "top") != touching.end() ||
                                  std::find(touching.begin(), touching.end(), "bottom") != touching.end();
            TJson values = TJson::object();
            if (horizontal) values["canvas.width"] = std::min(titleart::TITLE_MAX_CANVAS_SIDE, (int)std::ceil(cw * 1.25 / 16.0) * 16);
            if (vertical) values["canvas.height"] = std::min(titleart::TITLE_MAX_CANVAS_SIDE, (int)std::ceil(ch * 1.25 / 16.0) * 16);
            r.fix = TJson{{"ops", TJson::array({TJson{{"op", "set"}, {"values", values}}})}};
        } else if (minMargin < 8) {
            r.status = TitleCheckStatus::Warn;
            r.message = "the art comes within " + std::to_string(minMargin) + " px of the canvas edge";
        } else {
            r.message = "margins of at least " + std::to_string(minMargin) + " px on every side";
        }
        out.push_back(std::move(r));
    }

    void alphaBoundsCheck(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        const TitleRectI box = alphaBox(*base, 1);
        TitleCheckResult r = result(TitleCheckStatus::Pass, "", "the trim box of alpha >= 1/255 against the canvas");
        r.threshold = TJson{{"coverage_pct", TJson::array({10, 90})}};
        if (box.empty()) {
            r.status = TitleCheckStatus::Fail;
            r.message = "nothing visible: no trim box";
            r.measured = TJson{{"coverage_pct", 0}};
            out.push_back(std::move(r));
            return;
        }
        const TitleRectI c = toCanvas(*base, box);
        const int cw = ctx.doc.canvasWidth(), ch = ctx.doc.canvasHeight();
        const double coverage = 100.0 * c.width * c.height / std::max(1.0, static_cast<double>(cw) * ch);
        const double offX = c.x + c.width / 2.0 - cw / 2.0, offY = c.y + c.height / 2.0 - ch / 2.0;
        r.measured = TJson{{"trim", rect(c)}, {"coverage_pct", std::round(coverage * 10.0) / 10.0},
                          {"center_offset", TJson::array({offX, offY})}};
        if (coverage < 10.0 || coverage > 90.0) {
            r.status = TitleCheckStatus::Warn;
            r.message = "the art covers " + std::to_string(static_cast<int>(std::round(coverage))) + " % of the canvas (trim box), outside 10..90 %";
        } else {
            r.message = "the trim box covers " + std::to_string(static_cast<int>(std::round(coverage))) + " % of the canvas";
        }
        r.at.push_back(c);
        out.push_back(std::move(r));
    }

    // calibrated (S84 §6.14.3): red controls at most 4.7, the library at least 30.3 on its intended background
    constexpr double LEGIBILITY_MIN_DELTA = 22.0;

    void legibility(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        titleart::RenderRequest request = renders.request();
        std::string key = "base";
        if (ctx.options.displayHeightPx && *ctx.options.displayHeightPx > 0) {
            request.pxScale = static_cast<float>(*ctx.options.displayHeightPx) / std::max(1, ctx.doc.canvasHeight());
            key = "display:" + std::to_string(*ctx.options.displayHeightPx);
        }
        const TitleCheckImage* image = renders.get(key, request, error);
        if (!image) return renderFailed(out, error);
        // the bands of the silhouette, outer ring first: distance to the nearest transparent pixel
        // (city block), two pixels per band, down to 5 % of the image height
        const int w = image->width, h = image->height;
        const int depth = std::max(4, h / 20);
        const int bandCount = depth / 2 + 1;
        std::vector<int> distance(static_cast<size_t>(w) * h, 0);
        auto at = [&](int x, int y) { return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : distance[static_cast<size_t>(y) * w + x]; };
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (alphaAt(*image, x, y) < 128) continue;
                distance[static_cast<size_t>(y) * w + x] = std::min(at(x - 1, y), at(x, y - 1)) + 1;
            }
        }
        std::vector<std::vector<size_t>> bands(bandCount);
        for (int y = h - 1; y >= 0; y--) {
            for (int x = w - 1; x >= 0; x--) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (distance[i] == 0) continue;
                distance[i] = std::min(distance[i], std::min(at(x + 1, y), at(x, y + 1)) + 1);
                const int band = (distance[i] - 1) / 2;
                if (band < bandCount) bands[band].push_back(i);
            }
        }
        const size_t minBand = std::max<size_t>(16, bands[0].size() / 20);
        TitleCheckResult r = result(TitleCheckStatus::Pass, "", "CIE L* of the silhouette's bands composited on each background (outer ring inwards to 5 % of the height), the band that stands out most against the background");
        r.threshold = TJson{{"delta_l", LEGIBILITY_MIN_DELTA}};
        if (bands[0].empty()) {
            r.status = TitleCheckStatus::Skip;
            r.message = "no opaque silhouette to measure";
            out.push_back(std::move(r));
            return;
        }
        TJson measured = TJson::object();
        std::string worstBg;
        double worst = 1e9;
        for (const std::string& bgName : ctx.options.backgrounds) {
            titleart::TitleColor bg;
            if (!backgroundColor(bgName, bg)) continue;
            const double bgL = srgbToL(bg.r, bg.g, bg.b);
            double delta = 0.0;
            for (const std::vector<size_t>& band : bands) {
                if (band.size() < minBand) continue;
                double sum = 0.0;
                for (size_t i : band) {
                    const uint8_t* p = image->rgba.data() + i * 4;
                    const double a = p[3] / 255.0;
                    sum += srgbToL(p[0] / 255.0 * a + bg.r * (1 - a), p[1] / 255.0 * a + bg.g * (1 - a), p[2] / 255.0 * a + bg.b * (1 - a));
                }
                delta = std::max(delta, std::abs(sum / band.size() - bgL));
            }
            measured[bgName] = std::round(delta * 10.0) / 10.0;
            if (delta < worst) {
                worst = delta;
                worstBg = bgName;
            }
        }
        r.measured = TJson{{"delta_l", measured}};
        if (worst < LEGIBILITY_MIN_DELTA) {
            r.status = TitleCheckStatus::Fail;
            r.message = "the silhouette is hard to tell from the " + worstBg + " background (delta L* " + std::to_string(static_cast<int>(worst)) + ")";
            titleart::TitleColor bg;
            backgroundColor(worstBg, bg);
            const bool lightBg = srgbToL(bg.r, bg.g, bg.b) > 50.0;
            for (const TJson& layer : ctx.doc.layers()) {
                const titleart::LayerType type = titleart::layerTypeOf(layer);
                if (!titleart::layerTypeHasStyle(type) || !layer.value("visible", true)) continue;
                r.fix = TJson{{"ops", TJson::array({TJson{{"op", "add_rim"}, {"layer", layerRef(str(layer, "id"))},
                                                       {"values", TJson{{"width", 4}, {"material.fill.color", lightBg ? "#101010" : "#f4f4f4"}}}}})}};
                break;
            }
            const TitleRectI box = alphaBox(*image, 128);
            if (!box.empty()) r.at.push_back(toCanvas(*image, box));
        } else {
            r.message = "the silhouette stands out on every background (lowest delta L* " + std::to_string(static_cast<int>(worst)) + " on " + worstBg + ")";
        }
        out.push_back(std::move(r));
    }

    // holes: transparent 4-connected regions that do not reach the image border, >= 4 px
    std::vector<std::vector<size_t>> holesOf(const TitleCheckImage& image) {
        const int w = image.width, h = image.height;
        std::vector<int> label(static_cast<size_t>(w) * h, -1);
        std::vector<std::vector<size_t>> holes;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const size_t start = static_cast<size_t>(y) * w + x;
                if (label[start] != -1 || alphaAt(image, x, y) >= 128) continue;
                std::vector<size_t> region;
                bool border = false;
                std::deque<size_t> queue{start};
                label[start] = 0;
                while (!queue.empty()) {
                    const size_t i = queue.front();
                    queue.pop_front();
                    region.push_back(i);
                    const int px = static_cast<int>(i % w), py = static_cast<int>(i / w);
                    if (px == 0 || py == 0 || px == w - 1 || py == h - 1) border = true;
                    const int nx[4] = {px - 1, px + 1, px, px}, ny[4] = {py, py, py - 1, py + 1};
                    for (int k = 0; k < 4; k++) {
                        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                        const size_t n = static_cast<size_t>(ny[k]) * w + nx[k];
                        if (label[n] != -1 || alphaAt(image, nx[k], ny[k]) >= 128) continue;
                        label[n] = 0;
                        queue.push_back(n);
                    }
                }
                if (!border && region.size() >= 4) holes.push_back(std::move(region));
            }
        }
        return holes;
    }

    struct CounterLab {
        double L = 0.0, a = 0.0, b = 0.0;
    };

    // CIE Lab (D65) of a non-linear sRGB colour
    CounterLab srgbToLab(double r, double g, double b) {
        const double R = titleart::srgbToLinear(static_cast<float>(r)), G = titleart::srgbToLinear(static_cast<float>(g)), B = titleart::srgbToLinear(static_cast<float>(b));
        const auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
        const double fx = f((0.4124 * R + 0.3576 * G + 0.1805 * B) / 0.95047);
        const double fy = f(0.2126 * R + 0.7152 * G + 0.0722 * B);
        const double fz = f((0.0193 * R + 0.1192 * G + 0.9505 * B) / 1.08883);
        return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

    // calibrated (S84 §6.14.3): a counter survives when it is visibly distinct from the letter. The
    // red controls (a rim of the face's colour, a counter flooded by an outline of it) measure at
    // most 6.5; the library's counters that read as open start above 8
    constexpr double COUNTER_MIN_DELTA_E = 8.0;
    constexpr double COUNTER_OPEN_DELTA_E = 100.0;      // a transparent counter: the background shows

    // how visible a counter is: delta E between the mean colour of the rendered counter and of the
    // letter around it (the plain mask's opaque pixels within 2 px), both composited on `bg`
    double counterContrast(const TitleCheckImage& mask, const TitleCheckImage& styled, const std::vector<size_t>& hole, const titleart::TitleColor& bg) {
        CounterLab holeSum, letterSum;
        const auto add = [&](CounterLab& sum, size_t i) {
            const uint8_t* p = styled.rgba.data() + i * 4;
            const double a = p[3] / 255.0;
            const CounterLab c = srgbToLab(p[0] / 255.0 * a + bg.r * (1 - a), p[1] / 255.0 * a + bg.g * (1 - a), p[2] / 255.0 * a + bg.b * (1 - a));
            sum.L += c.L;
            sum.a += c.a;
            sum.b += c.b;
        };
        std::set<size_t> letter;
        for (size_t i : hole) {
            add(holeSum, i);
            const int x = static_cast<int>(i % mask.width), y = static_cast<int>(i / mask.width);
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= mask.width || ny >= mask.height) continue;
                    const size_t n = static_cast<size_t>(ny) * mask.width + nx;
                    if (alphaAt(mask, nx, ny) >= 128 && styled.rgba[n * 4 + 3] >= 128) letter.insert(n);
                }
            }
        }
        if (letter.empty() || hole.empty()) return 0.0;
        for (size_t n : letter) add(letterSum, n);
        const double h = static_cast<double>(hole.size()), l = static_cast<double>(letter.size());
        return std::hypot(holeSum.L / h - letterSum.L / l, holeSum.a / h - letterSum.a / l, holeSum.b / h - letterSum.b / l);
    }

    void counterSurvival(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::vector<std::string> textLayers;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && layer.value("visible", true)) textLayers.push_back(str(layer, "id"));
            return textLayers.size() < 4;
        });
        TitleCheckResult r = result(TitleCheckStatus::Pass, "", "holes of the plain text mask (no rims, no effects) still visible in the styled layer: transparent, or filled with a colour visibly unlike the letter around them (delta E on the background)");
        r.threshold = TJson{{"survival", 0.8}, {"delta_e", COUNTER_MIN_DELTA_E}};
        titleart::TitleColor background;
        backgroundColor("black", background);
        for (const std::string& name : ctx.options.backgrounds) {
            if (backgroundColor(name, background)) break;
        }
        std::vector<double> contrasts;
        if (textLayers.empty()) {
            r.status = TitleCheckStatus::Skip;
            r.message = "no text layer";
            out.push_back(std::move(r));
            return;
        }
        size_t total = 0, survived = 0;
        TJson perLayer = TJson::object();
        std::string worstLayer;
        double worstRatio = 2.0;
        for (const std::string& id : textLayers) {
            titleart::RenderRequest plain = renders.request();
            plain.isolateLayer = id;
            plain.overrides.push_back({id, "style.rims", TJson::array()});
            plain.overrides.push_back({id, "effects", TJson::array()});
            plain.overrides.push_back({id, "part_effects", TJson::object()});
            // a glass face is translucent and speckled: the mask needs the letter's silhouette
            plain.overrides.push_back({id, "style.face.glass.enabled", false});
            std::string error;
            const TitleCheckImage* mask = renders.get("plain:" + id, plain, error);
            if (!mask) return renderFailed(out, error);
            const TitleCheckImage* styled = renders.isolated(id, error);
            if (!styled) return renderFailed(out, error);
            if (styled->width != mask->width || styled->height != mask->height || !(styled->frame == mask->frame)) continue;
            const std::vector<std::vector<size_t>> holes = holesOf(*mask);
            size_t layerSurvived = 0;
            for (const std::vector<size_t>& hole : holes) {
                size_t open = 0;
                int minX = mask->width, minY = mask->height, maxX = 0, maxY = 0;
                for (size_t i : hole) {
                    if (styled->rgba[i * 4 + 3] < 128) open++;
                    const int x = static_cast<int>(i % mask->width), y = static_cast<int>(i / mask->width);
                    minX = std::min(minX, x); minY = std::min(minY, y); maxX = std::max(maxX, x); maxY = std::max(maxY, y);
                }
                // a counter survives while it can be seen: open, or filled with something visibly
                // unlike the letter around it (a rim of the face's own colour closes it)
                const double contrast = open * 2 >= hole.size() ? COUNTER_OPEN_DELTA_E : counterContrast(*mask, *styled, hole, background);
                contrasts.push_back(contrast);
                if (contrast >= COUNTER_MIN_DELTA_E) layerSurvived++;
                else r.at.push_back(toCanvas(*mask, {minX, minY, maxX - minX + 1, maxY - minY + 1}));
            }
            total += holes.size();
            survived += layerSurvived;
            const double ratio = holes.empty() ? 1.0 : static_cast<double>(layerSurvived) / holes.size();
            perLayer[layerName(ctx.doc, id)] = TJson{{"holes", holes.size()}, {"survived", layerSurvived}};
            if (ratio < worstRatio) {
                worstRatio = ratio;
                worstLayer = id;
            }
        }
        const double ratio = total == 0 ? 1.0 : static_cast<double>(survived) / total;
        // the contrast of the counter that decides the verdict (survival >= 0.8 lets a fifth fall short)
        std::sort(contrasts.begin(), contrasts.end());
        const double deciding = contrasts.empty() ? COUNTER_OPEN_DELTA_E : contrasts[static_cast<size_t>(contrasts.size() * 0.2)];
        r.measured = TJson{{"survival", std::round(ratio * 100.0) / 100.0}, {"holes", total}, {"layers", perLayer}, {"delta_e", std::round(deciding * 10.0) / 10.0}};
        if (total == 0) {
            r.message = "the text has no counters to keep";
        } else if (ratio < 0.8) {
            r.status = TitleCheckStatus::Fail;
            r.message = std::to_string(total - survived) + " of " + std::to_string(total) + " letter counters are filled in the render";
            const TJson* layer = titleart::findLayer(ctx.doc.root, worstLayer);
            if (layer && layer->contains("style") && (*layer)["style"].contains("rims")) {
                const TJson& rims = (*layer)["style"]["rims"];
                int widest = -1;
                double width = 0.0;
                for (size_t i = 0; i < rims.size(); i++) {
                    if (!rims[i].value("enabled", true)) continue;
                    const double w = rims[i].value("width", 0.0);
                    if (w > width) {
                        width = w;
                        widest = static_cast<int>(i);
                    }
                }
                if (widest >= 0) {
                    r.fix = TJson{{"ops", TJson::array({TJson{{"op", "set"}, {"layer", layerRef(worstLayer)},
                                                           {"values", TJson{{"rim#" + std::to_string(widest) + ".width", std::round(width * 50.0) / 100.0}}}}})}};
                }
            }
        } else {
            r.message = std::to_string(survived) + " of " + std::to_string(total) + " letter counters stay open";
        }
        out.push_back(std::move(r));
    }

    void noopEffects(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        struct Candidate { std::string layer, effect, type; size_t index; };
        std::vector<Candidate> candidates;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            const TJson& effects = layer.contains("effects") ? layer["effects"] : TJson::array();
            for (size_t i = 0; i < effects.size() && candidates.size() < 16; i++) {
                if (effects[i].value("enabled", true)) candidates.push_back({str(layer, "id"), str(effects[i], "id"), str(effects[i], "type"), i});
            }
            return candidates.size() < 16;
        });
        TitleCheckResult r = result(TitleCheckStatus::Pass, "", "each enabled layer effect turned off: the share of pixels that change");
        r.threshold = TJson{{"changed_pct_min", 0.01}};
        TJson noop = TJson::array(), ops = TJson::array();
        for (const Candidate& c : candidates) {
            titleart::RenderRequest without = renders.request();
            without.overrides.push_back({c.layer, "effects[" + std::to_string(c.index) + "].enabled", false});
            const TitleCheckImage* image = renders.get("without:" + c.effect, without, error);
            if (!image) return renderFailed(out, error);
            ImageDiffOptions options;
            options.tolerance = 1;
            const ImageDiffResult diff = diffImages(asImage(*base), asImage(*image), options);
            if (diff.sameSize && diff.fractionOver * 100.0 < 0.01) {
                noop.push_back(TJson{{"layer", layerName(ctx.doc, c.layer)}, {"effect", "fx:" + c.type}, {"changed_pct", diff.fractionOver * 100.0}});
                ops.push_back(TJson{{"op", "remove_effect"}, {"layer", layerRef(c.layer)}, {"effect", "id:" + c.effect}});
            }
        }
        r.measured = TJson{{"effects", candidates.size()}, {"noop", noop}};
        if (!noop.empty()) {
            r.status = TitleCheckStatus::Warn;
            r.message = std::to_string(noop.size()) + " enabled effect(s) change nothing visible (first: " + noop[0]["effect"].get<std::string>() +
                        " on " + noop[0]["layer"].get<std::string>() + ")";
            r.fix = TJson{{"ops", ops}};
        } else {
            r.message = candidates.empty() ? "no enabled effect" : "every enabled effect changes the render";
        }
        out.push_back(std::move(r));
    }

    void clippedHighlights(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        size_t ink = 0, clipped = 0;
        for (size_t i = 0; i < base->rgba.size(); i += 4) {
            if (base->rgba[i + 3] < 128) continue;
            ink++;
            if (base->rgba[i] >= 250 && base->rgba[i + 1] >= 250 && base->rgba[i + 2] >= 250) clipped++;
        }
        const double pct = ink ? 100.0 * clipped / ink : 0.0;
        TitleCheckResult r = result(pct > 5.0 ? TitleCheckStatus::Warn : TitleCheckStatus::Pass, "", "ink pixels (alpha >= 50 %) with every channel >= 250");
        r.measured = TJson{{"clipped_pct", std::round(pct * 10.0) / 10.0}};
        r.threshold = TJson{{"clipped_pct_max", 5}};
        if (pct > 5.0) {
            r.message = std::to_string(static_cast<int>(std::round(pct))) + " % of the ink is clipped to white: highlights lose their shape";
            for (const TJson& layer : ctx.doc.layers()) {
                const TJson* bevel = layer.contains("style") ? &layer["style"]["face"]["bevel"] : nullptr;
                if (!bevel || !bevel->is_object() || !bevel->value("enabled", false)) continue;
                const double opacity = bevel->value("highlight_opacity", 0.85);
                r.fix = TJson{{"ops", TJson::array({TJson{{"op", "set"}, {"layer", layerRef(str(layer, "id"))},
                                                       {"values", TJson{{"face.bevel.highlight_opacity", std::round(opacity * 70.0) / 100.0}}}}})}};
                break;
            }
        } else {
            r.message = "highlights keep their shape";
        }
        out.push_back(std::move(r));
    }

    void halo(TitleCheckContext& ctx, Out& out) {
        // the main output as baked when it exists (alpha bleed is a bake setting), else a render
        texsynth::Image image;
        std::string method = "faint edge pixels (0 < alpha < 16/255) whose colour differs from the nearest opaque colour by over 96 codes";
        int outputIndex = -1;
        const TJson& outputs = ctx.doc.root.contains("outputs") ? ctx.doc.root["outputs"] : TJson::array();
        if (!ctx.file.empty() && !outputs.empty()) {
            const fs::path png = titleArtOutputPath(ctx.file, outputs[0]);
            texsynth::Image loaded;
            std::string error;
            std::error_code ec;
            if (fs::is_regular_file(png, ec) && texsynth::loadImage(png, loaded, error)) {
                image = texsynth::toRgba8(loaded);
                outputIndex = 0;
                method += ", on the baked " + png.filename().string();
            }
        }
        if (image.empty()) {
            Renders& renders = rendersOf(ctx);
            std::string error;
            const TitleCheckImage* base = renders.base(error);
            if (!base) return renderFailed(out, error);
            image = asImage(*base);
            method += ", on a render";
        }
        size_t faint = 0, halos = 0;
        const int w = image.width, h = image.height;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint8_t* p = image.pixel(x, y);
                if (p[3] == 0 || p[3] >= 16) continue;
                faint++;
                const uint8_t* nearest = nullptr;
                int best = 1 << 30;
                for (int dy = -3; dy <= 3; dy++) {
                    for (int dx = -3; dx <= 3; dx++) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const uint8_t* q = image.pixel(nx, ny);
                        if (q[3] < 128 || dx * dx + dy * dy >= best) continue;
                        best = dx * dx + dy * dy;
                        nearest = q;
                    }
                }
                if (!nearest) continue;
                int d = 0;
                for (int k = 0; k < 3; k++) d = std::max(d, std::abs(int(p[k]) - int(nearest[k])));
                if (d > 96) halos++;
            }
        }
        const double share = faint ? static_cast<double>(halos) / faint : 0.0;
        TitleCheckResult r = result(share > 0.25 ? TitleCheckStatus::Warn : TitleCheckStatus::Pass, "", method);
        r.measured = TJson{{"halo_share", std::round(share * 100.0) / 100.0}, {"faint_pixels", faint}};
        r.threshold = TJson{{"halo_share_max", 0.25}};
        if (share > 0.25) {
            r.message = "edge pixels carry a colour unlike the art: dark or bright fringes when filtered or mipmapped";
            if (outputIndex >= 0 && !outputs[outputIndex].value("alpha_bleed", true)) {
                r.fix = TJson{{"ops", TJson::array({TJson{{"op", "set"}, {"values", TJson{{"outputs[" + std::to_string(outputIndex) + "].alpha_bleed", true}}}}})}};
            }
        } else {
            r.message = "edges carry the art's own colour";
        }
        out.push_back(std::move(r));
    }

    void deterministic(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* a = renders.get("det:a", renders.request(), error, true);
        if (!a) return renderFailed(out, error);
        const TitleCheckImage* b = renders.get("det:b", renders.request(), error, true);
        if (!b) return renderFailed(out, error);
        const bool same = a->width == b->width && a->height == b->height && a->rgba == b->rgba;
        TitleCheckResult r = result(same ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "", "two fresh renders compared byte for byte");
        r.measured = TJson{{"identical", same}};
        r.threshold = TJson{{"identical", true}};
        if (same) {
            r.message = "two renders are identical";
        } else {
            std::vector<std::string> culprits;
            for (const std::string& id : visibleTopLayers(ctx.doc, 8)) {
                titleart::RenderRequest alone = renders.request();
                alone.isolateLayer = id;
                const TitleCheckImage* x = renders.get("det:x:" + id, alone, error, true);
                const TitleCheckImage* y = renders.get("det:y:" + id, alone, error, true);
                if (x && y && x->rgba != y->rgba) culprits.push_back(layerName(ctx.doc, id));
            }
            std::string names;
            for (const std::string& n : culprits) names += (names.empty() ? "" : ", ") + n;
            r.message = "two renders differ" + (names.empty() ? std::string() : " (in " + names + "): an element is not seeded from the document");
            if (!culprits.empty()) r.measured["layers"] = culprits;
            ImageDiffOptions options;
            options.tolerance = 0;
            const ImageDiffResult diff = diffImages(asImage(*a), asImage(*b), options);
            if (!diff.bbox.empty()) r.at.push_back(toCanvas(*a, {diff.bbox.x, diff.bbox.y, diff.bbox.width, diff.bbox.height}));
        }
        out.push_back(std::move(r));
    }

    // the class a whole document is held to (S83 §6.5): its least exact element's; a 3D layer's
    // light is above 1 and compressed on the fallback, the F2 case
    titleart::FallbackClass documentFallbackClass(const titleart::TitleArtDoc& doc) {
        titleart::FallbackClass worst = titleart::FallbackClass::F0;
        const titleart::TitleEffectRegistry& registry = titleart::TitleEffectRegistry::get();
        auto raise = [&](titleart::FallbackClass c) { worst = std::max(worst, c); };
        auto effects = [&](const TJson& list) {
            if (!list.is_array()) return;
            for (const TJson& e : list) {
                if (!e.value("enabled", true)) continue;
                if (const titleart::EffectDef* def = registry.effect(str(e, "type"))) raise(def->fallback);
            }
        };
        titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
            if (!layer.value("visible", true) || layer.value("guide", false)) return true;
            if (layer.contains("effects")) effects(layer["effects"]);
            if (layer.contains("part_effects") && layer["part_effects"].is_object()) for (const auto& part : layer["part_effects"].items()) effects(part.value());
            if (layer.contains("generator") && layer["generator"].is_object()) {
                if (const titleart::GeneratorDef* def = registry.generator(str(layer["generator"], "type"))) raise(def->fallback);
            }
            if (layer.value("dimension", std::string()) == "3d") raise(titleart::FallbackClass::F2);
            return true;
        });
        return worst;
    }

    void fallbackParity(TitleCheckContext& ctx, Out& out) {
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* floatPath = renders.base(error);
        if (!floatPath) return renderFailed(out, error);
        titleart::RenderRequest forced = renders.request();
        forced.testFlags = titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS | titleart::TITLE_ART_TEST_NO_R16F | titleart::TITLE_ART_TEST_NO_RGBA32F;
        const TitleCheckImage* rgba8 = renders.get("rgba8", forced, error);
        if (!rgba8) return renderFailed(out, error);
        // the tolerance of the document's fallback class (S83 §6.5, what each effect's own test holds
        // it to); a 3D layer's RGBA8 path compresses its light, S86 AC9's own criteria
        const titleart::FallbackClass cls = documentFallbackClass(ctx.doc);
        bool has3D = false;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            if (layer.value("visible", true) && !layer.value("guide", false) && layer.value("dimension", std::string()) == "3d") has3D = true;
            return !has3D;
        });
        ImageDiffOptions options;
        options.premultiplied = true;
        options.tolerance = has3D ? 24 : 8;
        options.maxFraction = has3D ? 0.01 : cls == titleart::FallbackClass::F0 ? 0.005 : cls == titleart::FallbackClass::F1 ? 0.02 : 0.05;
        options.maxMean = has3D ? 3.0 : cls == titleart::FallbackClass::F0 ? 1.0 : cls == titleart::FallbackClass::F1 ? 1.5 : 3.0;
        // Lightning's edge cells and Backlight's shape centre read the followed layer's 8-bit grid: a
        // borderline cell flips between the paths, a few pixels differ by more than the max while
        // the mean and the share hold (S83's own TitleGenGpuTest makes the same exception)
        bool gridLayout = false;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            if (layer.contains("generator") && layer["generator"].is_object() && layer.value("visible", true)) {
                const std::string type = str(layer["generator"], "type");
                // Glints that follow a layer pick their stars from its 8-bit analysis grid the same way
                // (HighlightFinder over readGrid): a star at a borderline cell moves between the paths
                // (measured on the library, S83 Deviation 38: up to 253/255 on seven items, mean and share within class)
                const bool follows = !str(layer, "follow").empty();
                gridLayout = gridLayout || type == "lightning" || type == "backlight" || (type == "glints" && follows);
            }
            return true;
        });
        options.maxDiff = has3D || gridLayout ? 255 : cls == titleart::FallbackClass::F0 ? 48 : cls == titleart::FallbackClass::F1 ? 64 : 96;
        // a 3D glint places discrete stars from 8-bit light on the RGBA8 path (S86 Deviation 31): S86
        // AC9 holds a glint-on render to the mean alone; the same document with its glints off is held
        // to the 3D share and max, so everything but the stars stays judged per pixel
        std::vector<std::string> glintLayers;
        titleart::forEachLayer(ctx.doc.root, [&](const TJson& layer, const TJson*, int) {
            if (!layer.value("visible", true) || layer.value("guide", false) || layer.value("dimension", std::string()) != "3d") return true;
            const TJson& s3 = layer.value("style_3d", TJson::object());
            if (s3.is_object() && s3.value("glint", TJson::object()).is_object() && s3.value("glint", TJson::object()).value("enabled", false)) {
                glintLayers.push_back(str(layer, "id"));
            }
            return true;
        });
        if (!glintLayers.empty()) {
            auto glintsOff = std::make_shared<titleart::TitleArtDoc>(ctx.doc);
            for (const std::string& id : glintLayers) {
                if (TJson* layer = titleart::findLayer(glintsOff->root, id)) (*layer)["style_3d"]["glint"]["enabled"] = false;
            }
            titleart::RenderRequest offFloat = renders.request();
            offFloat.doc = glintsOff;
            titleart::RenderRequest offForced = forced;
            offForced.doc = glintsOff;
            const TitleCheckImage* floatOff = renders.get("glints off", offFloat, error);
            if (!floatOff) return renderFailed(out, error);
            const TitleCheckImage* rgba8Off = renders.get("glints off rgba8", offForced, error);
            if (!rgba8Off) return renderFailed(out, error);
            const ImageDiffResult off = diffImages(asImage(*floatOff), asImage(*rgba8Off), options);
            ImageDiffOptions onOptions = options;
            onOptions.maxFraction = 1.0;
            const ImageDiffResult on = diffImages(asImage(*floatPath), asImage(*rgba8), onOptions);
            const bool ok = off.ok && on.ok;
            TitleCheckResult r = result(ok ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "",
                                        "premultiplied comparison of the forced RGBA8 path with the float path: with the 3D glints off to S86 AC9's "
                                        "share and max, with them on (their stars) to its mean");
            r.measured = TJson{{"max", on.max}, {"mean", on.mean}, {"fraction_over", on.fractionOver}, {"class", "3D"},
                               {"glints_off", {{"max", off.max}, {"mean", off.mean}, {"fraction_over", off.fractionOver}}}};
            r.threshold = TJson{{"mean", onOptions.maxMean},
                                {"glints_off", {{"max", options.maxDiff}, {"mean", options.maxMean}, {"fraction_over_24", options.maxFraction}}}};
            r.message = ok ? "the RGBA8 path agrees with the float path"
                           : !off.ok ? (off.sameSize ? "with the 3D glints off, the RGBA8 path differs from the float path beyond §6.3's tolerance" : off.error)
                                     : (on.sameSize ? "the RGBA8 path's 3D glints differ from the float path's beyond S86 AC9's mean" : on.error);
            const ImageDiffResult& bad = !off.ok ? off : on;
            if (!ok && !bad.bbox.empty()) r.at.push_back(toCanvas(*floatPath, {bad.bbox.x, bad.bbox.y, bad.bbox.width, bad.bbox.height}));
            out.push_back(std::move(r));
            return;
        }
        const ImageDiffResult diff = diffImages(asImage(*floatPath), asImage(*rgba8), options);
        TitleCheckResult r = result(diff.ok ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "", "premultiplied comparison of the forced RGBA8 path with the float path, to the tolerance of the document's fallback class");
        r.measured = TJson{{"max", diff.max}, {"mean", diff.mean}, {"fraction_over", diff.fractionOver}, {"class", has3D ? "3D" : titleart::fallbackClassName(cls)}};
        r.threshold = TJson{{"max", options.maxDiff}, {"mean", options.maxMean}, {has3D ? "fraction_over_24" : "fraction_over_8", options.maxFraction}};
        r.message = diff.ok ? "the RGBA8 path agrees with the float path" : (diff.sameSize ? "the RGBA8 path differs from the float path beyond §6.3's tolerance" : diff.error);
        if (!diff.ok && !diff.bbox.empty()) r.at.push_back(toCanvas(*floatPath, {diff.bbox.x, diff.bbox.y, diff.bbox.width, diff.bbox.height}));
        out.push_back(std::move(r));
    }

    void golden(TitleCheckContext& ctx, Out& out) {
        if (ctx.options.baseline.empty()) {
            out.push_back(result(TitleCheckStatus::Skip, "no baseline given (baseline: a PNG or rev:<n>)", "premultiplied comparison"));
            return;
        }
        Renders& renders = rendersOf(ctx);
        std::string error;
        const TitleCheckImage* base = renders.base(error);
        if (!base) return renderFailed(out, error);
        texsynth::Image baseline;
        if (!texsynth::loadImage(fs::path(ctx.options.baseline), baseline, error)) {
            out.push_back(result(TitleCheckStatus::Fail, "the baseline does not load: " + error, "premultiplied comparison"));
            return;
        }
        ImageDiffOptions options;
        const ImageDiffResult diff = diffImages(asImage(*base), baseline, options);
        TitleCheckResult r = result(diff.ok ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, "", "premultiplied comparison with the baseline (ImageDiff)");
        r.measured = TJson{{"max", diff.max}, {"mean", diff.mean}, {"fraction_over", diff.fractionOver}, {"changed_pct", diff.changedPct}};
        r.threshold = TJson{{"max", options.maxDiff}, {"mean", options.maxMean}, {"fraction_over_8", options.maxFraction}};
        r.message = diff.ok ? "the render matches the baseline" : (diff.sameSize ? "the render differs from the baseline" : diff.error);
        if (!diff.ok && !diff.bbox.empty()) r.at.push_back(toCanvas(*base, {diff.bbox.x, diff.bbox.y, diff.bbox.width, diff.bbox.height}));
        out.push_back(std::move(r));
    }

    struct Builtin {
        const char* id;
        TitleCheckKind kind;
        const char* description;
        void (*run)(TitleCheckContext&, Out&);
    };

    const Builtin BUILTINS[] = {
        {"document_valid", TitleCheckKind::Document, "Parses, version, unknown types, normalize warnings", documentValid},
        {"assets_present", TitleCheckKind::Document, "Every AssetRef resolves", assetsPresent},
        {"glyphs_present", TitleCheckKind::Document, "No missing glyphs after the fallback chain", glyphsPresent},
        {"exposed_targets", TitleCheckKind::Document, "Every exposed target resolves", exposedTargets},
        {"rim_spikes", TitleCheckKind::Document, "No mitred rim join reaches past its band as a spike", rimSpikes},
        {"bake_fresh", TitleCheckKind::Document, "Every output fresh", bakeFresh},
        {"not_flat", TitleCheckKind::Pixels, "The render is not blank or one colour", notFlat},
        {"not_clipped", TitleCheckKind::Pixels, "No ink or light at the canvas edge; margins of 8 px", notClipped},
        {"alpha_bounds", TitleCheckKind::Pixels, "Trim box, coverage 10..90 %, centre offset", alphaBoundsCheck},
        {"legibility_contrast", TitleCheckKind::Pixels, "A band of the silhouette delta L* >= 22 on each background the item is made for", legibility},
        {"counter_survival", TitleCheckKind::Pixels, "Letter counters still open in the render", counterSurvival},
        {"noop_effects", TitleCheckKind::Pixels, "Enabled effects that change nothing", noopEffects},
        {"clipped_highlights", TitleCheckKind::Pixels, "Ink clipped to white <= 5 %", clippedHighlights},
        {"halo", TitleCheckKind::Pixels, "Edge colour fringes", halo},
        {"deterministic", TitleCheckKind::Pixels, "Two fresh renders identical", deterministic},
        {"fallback_parity", TitleCheckKind::Pixels, "RGBA8 path within tolerance of the float path", fallbackParity},
        {"golden", TitleCheckKind::Pixels, "Premultiplied comparison with a baseline", golden},
    };

}

const std::vector<std::string>& editor::builtinTitleCheckIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (const Builtin& b : BUILTINS) out.push_back(b.id);
        return out;
    }();
    return ids;
}

void editor::registerBuiltinTitleChecks() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const Builtin& b : BUILTINS) {
        CheckDef def;
        def.id = b.id;
        def.kind = b.kind;
        def.owner = "S80";
        def.description = b.description;
        auto run = b.run;
        def.run = [run](TitleCheckContext& context, std::vector<TitleCheckResult>& out) { run(context, out); };
        TitleArtChecks::registerCheck(std::move(def));
    }
    // S83's generator checks
    registerTitleGeneratorChecks();
    // the later specs' checks, so every surface that runs the checks has them (S81: glyph_atlas, characters R55)
    registerTitleArtRuntimeChecks();
    registerTitleCharactersCheck();
}

std::vector<titleart::TitleProblem> editor::titleArtMissingAssets(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets) {
    std::vector<titleart::TitleProblem> problems;
    titleart::visitTitleArtRefs(doc.root, [&](const titleart::TitleAssetRef& ref, const std::string& field, titleart::TitleAssetKind kind) {
        if (ref.empty()) return true;
        if (assets.resolve(ref, kind).found) return true;
        std::string layerId, path;
        splitLayerPath(doc.root, field, layerId, path);
        titleart::TitleProblem p;
        p.severity = titleart::TitleSeverity::Error;
        p.code = "asset_missing";
        p.layer = layerId;
        p.path = path.empty() ? field : path;
        p.message = std::string("missing ") + titleart::titleAssetKindName(kind) + " " + (ref.path.empty() ? ref.guid : ref.path) +
                    (layerId.empty() ? "" : " (layer " + layerName(doc, layerId) + ", " + path + ")");
        p.detail = TJson{{"kind", titleart::titleAssetKindName(kind)}, {"path", ref.path}, {"guid", ref.guid}, {"field", field}};
        problems.push_back(std::move(p));
        return true;
    });
    return problems;
}

std::vector<titleart::TitleProblem> editor::titleArtMissingGlyphs(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets) {
    std::vector<titleart::TitleProblem> problems;
    std::shared_ptr<FontFace> builtin = assets.builtinFont();
    std::vector<std::pair<titleart::ResolvedTitleAsset, std::shared_ptr<FontFace>>> projectFonts;
    bool projectFontsLoaded = false;
    titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text) return true;
        std::vector<std::shared_ptr<FontFace>> chain;
        const int faceIndex = layer.value("face_index", 0);
        const titleart::TitleAssetRef font = titleart::decodeAssetRef(layer.contains("font") ? layer["font"] : TJson());
        if (!font.empty()) {
            const titleart::ResolvedTitleAsset resolved = assets.resolve(font, titleart::TitleAssetKind::Font);
            if (resolved.found) {
                if (std::shared_ptr<FontFace> face = assets.loadFont(resolved, faceIndex, nullptr)) chain.push_back(face);
            }
        }
        if (layer.contains("fallback_fonts") && layer["fallback_fonts"].is_array()) {
            for (const TJson& ref : layer["fallback_fonts"]) {
                const titleart::ResolvedTitleAsset resolved = assets.resolve(titleart::decodeAssetRef(ref), titleart::TitleAssetKind::Font);
                if (!resolved.found) continue;
                if (std::shared_ptr<FontFace> face = assets.loadFont(resolved, 0, nullptr)) chain.push_back(face);
            }
        }
        if (builtin) chain.push_back(builtin);
        bool invalid = false;
        const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(layer.value("text", std::string()), invalid);
        TJson chars = TJson::array();
        std::vector<uint32_t> missing;
        for (size_t i = 0; i < codepoints.size(); i++) {
            const uint32_t cp = codepoints[i];
            // whitespace, controls, joiners and variation selectors draw nothing of their own
            if (cp < 0x21 || cp == 0x7f || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0x3000 || cp == 0xfeff) continue;
            bool covered = false;
            for (const std::shared_ptr<FontFace>& face : chain) {
                if (face->hasCodepoint(cp)) {
                    covered = true;
                    break;
                }
            }
            if (covered) continue;
            chars.push_back(TJson{{"index", i}, {"codepoint", cp}, {"char", utf8Of(cp)}});
            missing.push_back(cp);
        }
        if (missing.empty()) return true;
        if (!projectFontsLoaded) {
            projectFontsLoaded = true;
            for (const titleart::ResolvedTitleAsset& asset : assets.projectFonts()) {
                if (projectFonts.size() >= 64) break;
                if (std::shared_ptr<FontFace> face = assets.loadFont(asset, 0, nullptr)) projectFonts.push_back({asset, face});
            }
        }
        TJson fontsWithIt = TJson::array();
        for (const auto& [asset, face] : projectFonts) {
            bool all = true;
            for (uint32_t cp : missing) all = all && face->hasCodepoint(cp);
            if (all) fontsWithIt.push_back(TJson{{"path", asset.assetPath}, {"guid", asset.guid}});
        }
        std::string list;
        for (size_t i = 0; i < chars.size() && i < 8; i++) list += chars[i]["char"].get<std::string>();
        titleart::TitleProblem p;
        p.severity = titleart::TitleSeverity::Warning;
        p.code = "glyph_missing";
        p.layer = str(layer, "id");
        p.path = "text";
        p.message = "layer " + str(layer, "name") + ": no font of its chain has " + std::to_string(chars.size()) + " character(s) (" + list + ")" +
                    (fontsWithIt.empty() ? std::string(": add a font that has them to fallback_fonts")
                                         : ": " + fontsWithIt[0]["path"].get<std::string>() + " has them");
        p.detail = TJson{{"chars", chars}, {"fonts_with_it", fontsWithIt}};
        problems.push_back(std::move(p));
        return true;
    });
    return problems;
}
