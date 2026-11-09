// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Title3DEditor.h"

#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/render/TitleContent.h"
#include "titleart/three_d/Title3DSchema.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleTierFeatures.h"
#include "titleart/three_d/TitleMeshFile.h"
#include "titleart/three_d/mesher/TitleMesher.h"
#include "asset/AssetFileWrite.h"
#include "texture/TexturePrepare.h"
#include "util/FileUtils.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    const Json& memberOf(const Json& object, const char* key) {
        static const Json none;
        auto it = object.find(key);
        return it != object.end() ? *it : none;
    }

    std::string titleCaseId(const std::string& id) {
        std::string out;
        bool upper = true;
        for (char c : id) {
            if (c == '_') {
                out += ' ';
                upper = true;
                continue;
            }
            out += upper ? (char)std::toupper((unsigned char)c) : c;
            upper = false;
        }
        return out;
    }

    // a 3D-capable layer of the op, its style_3d made (R2's derivation) when it has none
    Json* styledLayer3D(TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return nullptr;
        if (!titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer))) {
            ctx.fail("bad_layer", "only Text, Shape, Vector and Image layers have a 3D style", "layer");
            return nullptr;
        }
        if (!memberOf(*layer, "style_3d").is_object()) {
            (*layer)["style_3d"] = titleart::deriveStyle3D(*layer);
            TitleOpChange change;
            change.layer = layer->value("id", std::string());
            change.path = "style_3d";
            change.note = "derived the 3D style from the 2D style";
            ctx.change(std::move(change));
        }
        return layer;
    }

    // band#n, band:<id> or id:<id>; -1 after fail() when none
    long bandIndex(TitleOpContext& ctx, const Json& bands, const std::string& ref) {
        for (size_t i = 0; i < bands.size(); i++) {
            const std::string id = bands[i].value("id", std::string());
            if (ref == "band#" + std::to_string(i) || ref == "band:" + id || ref == "id:" + id) return (long)i;
        }
        std::vector<std::string> valid;
        for (size_t i = 0; i < bands.size(); i++) {
            valid.push_back("band#" + std::to_string(i));
            valid.push_back("band:" + bands[i].value("id", std::string()));
        }
        ctx.fail("bad_path", "no band '" + ref + "'", "band", valid, titleart::nearestName(ref, valid));
        return -1;
    }

    bool setDimension(TitleOpContext& ctx, const Json& op) {
        std::string dimension;
        if (!ctx.requireString(op, "dimension", dimension)) return false;
        if (dimension != "2d" && dimension != "3d") return ctx.fail("bad_value", "`dimension` is \"2d\" or \"3d\"", "dimension", {"2d", "3d"});
        Json* layer = nullptr;
        if (dimension == "3d") {
            layer = styledLayer3D(ctx, op);
            if (!layer) return false;
            if (!memberOf(ctx.doc.root, "scene_3d").is_object()) {
                Json scene = Json::object();
                titleart::NormalizeContext context;
                titleart::normalizeTitleArtObject(scene, "scene_3d", "scene_3d", context);
                ctx.doc.root["scene_3d"] = scene;
                TitleOpChange change;
                change.path = "scene_3d";
                change.note = "made the document's 3D scene";
                ctx.change(std::move(change));
            }
        } else {
            layer = ctx.layer(op);
            if (!layer) return false;
        }
        if (!ctx.set(layer, "dimension", dimension)) return false;
        ctx.undoName = dimension == "3d" ? "Set Dimension 3D" : "Set Dimension 2D";
        return true;
    }

    bool addBand(TitleOpContext& ctx, const Json& op) {
        Json* layer = styledLayer3D(ctx, op);
        if (!layer) return false;
        Json& style = (*layer)["style_3d"];
        Json& bands = style["bands"];
        if (!bands.is_array()) bands = Json::array();
        if (bands.size() >= titleart::TITLE_3D_MAX_BANDS) {
            return ctx.fail("limit", "a 3D layer has at most " + std::to_string(titleart::TITLE_3D_MAX_BANDS) + " bands");
        }
        size_t index = bands.size();
        if (op.contains("index")) {
            if (!op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 || (size_t)op["index"].get<int64_t>() > bands.size()) {
                return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(bands.size()) + " (0 = innermost)", "index");
            }
            index = (size_t)op["index"].get<int64_t>();
        }
        Json band = Json::object();
        if (op.contains("preset")) {
            if (!op["preset"].is_string()) return ctx.fail("bad_value", "`preset` is a band preset name", "preset", title3DBandPresetNames());
            const std::string preset = op["preset"].get<std::string>();
            const auto& names = title3DBandPresetNames();
            if (std::find(names.begin(), names.end(), preset) == names.end()) {
                return ctx.fail("bad_value", "no band preset '" + preset + "'", "preset", names, titleart::nearestName(preset, names));
            }
            band = title3DBandPreset(preset, style.value("depth", 14.0));
        }
        band["id"] = ctx.newId();
        titleart::NormalizeContext context;
        context.ids = &ctx.ids;
        titleart::normalizeTitleArtObject(band, "band_3d", "band", context);
        const std::string id = band.value("id", std::string());
        bands.insert(bands.begin() + (long)index, std::move(band));
        ctx.recordNewId("band", id);
        TitleOpChange change;
        change.layer = layer->value("id", std::string());
        change.path = "style_3d.bands[" + std::to_string(index) + "]";
        change.note = "added a band";
        ctx.change(std::move(change));
        if (op.contains("values")) {
            if (!op["values"].is_object()) return ctx.fail("bad_value", "`values` takes {\"<band field>\": value}", "values");
            for (auto it = op["values"].begin(); it != op["values"].end(); ++it) {
                if (!ctx.set(layer, "style_3d.bands[" + std::to_string(index) + "]." + it.key(), it.value())) return false;
            }
        }
        ctx.undoName = "Add Band";
        return true;
    }

    bool removeOrMoveBand(TitleOpContext& ctx, const Json& op, bool move) {
        Json* layer = styledLayer3D(ctx, op);
        if (!layer) return false;
        std::string ref;
        if (!ctx.requireString(op, "band", ref)) return false;
        Json& bands = (*layer)["style_3d"]["bands"];
        const long index = bandIndex(ctx, bands, ref);
        if (index < 0) return false;
        Json band = bands[(size_t)index];
        TitleOpChange change;
        change.layer = layer->value("id", std::string());
        if (move) {
            if (!op.contains("index") || !op["index"].is_number_integer() || op["index"].get<int64_t>() < 0 ||
                (size_t)op["index"].get<int64_t>() >= bands.size()) {
                return ctx.fail("out_of_range", "`index` is 0.." + std::to_string(bands.size() - 1) + " (0 = innermost)", "index");
            }
            const size_t to = (size_t)op["index"].get<int64_t>();
            bands.erase(bands.begin() + index);
            bands.insert(bands.begin() + (long)to, band);
            change.path = "style_3d.bands[" + std::to_string(to) + "]";
            change.note = "moved band " + std::to_string(index) + " to " + std::to_string(to);
            ctx.undoName = "Move Band";
        } else {
            bands.erase(bands.begin() + index);
            change.path = "style_3d.bands[" + std::to_string(index) + "]";
            change.note = "removed band " + std::to_string(index);
            ctx.undoName = "Remove Band";
        }
        ctx.change(std::move(change));
        return true;
    }

    bool applyMaterial(TitleOpContext& ctx, const Json& op) {
        Json* layer = styledLayer3D(ctx, op);
        if (!layer) return false;
        std::string part, preset;
        if (!ctx.requireString(op, "part", part) || !ctx.requireString(op, "preset", preset)) return false;
        const auto& presets = titleart::title3DMaterialPresetNames();
        if (std::find(presets.begin(), presets.end(), preset) == presets.end()) {
            return ctx.fail("bad_value", "no material preset '" + preset + "'", "preset", presets, titleart::nearestName(preset, presets));
        }
        Json& style = (*layer)["style_3d"];
        std::string path;
        Json base;
        if (part == "face") {
            path = "style_3d.face";
            base = style["face"];
        } else if (part == "bevel" || part == "side") {
            path = part == "bevel" ? "style_3d.bevel_material" : "style_3d.side";
            const Json& own = style[part == "bevel" ? "bevel_material" : "side"];
            base = own.is_object() ? own : style["face"];
        } else if (part.rfind("band#", 0) == 0 || part.rfind("band:", 0) == 0) {
            const long index = bandIndex(ctx, style["bands"], part);
            if (index < 0) return false;
            path = "style_3d.bands[" + std::to_string(index) + "].material";
            base = style["bands"][(size_t)index]["material"];
        } else {
            std::vector<std::string> valid = {"face", "bevel", "side"};
            for (size_t i = 0; i < style["bands"].size(); i++) {
                valid.push_back("band#" + std::to_string(i));
                valid.push_back("band:" + style["bands"][i].value("id", std::string()));
            }
            return ctx.fail("bad_path", "no 3D part '" + part + "'", "part", valid, titleart::nearestName(part, valid));
        }
        titleart::applyMaterialPreset(base, preset);
        if (!ctx.set(layer, path, base)) return false;
        ctx.undoName = "Apply Material " + titleCaseId(preset);
        return true;
    }

    bool fitToTier(TitleOpContext& ctx, const Json& op) {
        Json* layer = styledLayer3D(ctx, op);
        if (!layer) return false;
        std::string tier;
        if (!ctx.requireString(op, "tier", tier)) return false;
        if (tier != "high" && tier != "medium" && tier != "low") {
            return ctx.fail("bad_value", "`tier` is high, medium or low", "tier", {"high", "medium", "low"}, titleart::nearestName(tier, {"high", "medium", "low"}));
        }
        for (const Title3DFix& fix : title3DTierFixes(*layer, tier)) {
            if (!ctx.set(layer, fix.path, fix.value)) return false;
            ctx.note("fit_to_tier", fix.why, fix.path);
        }
        ctx.undoName = "Fit To Tier " + titleCaseId(tier);
        return true;
    }

    // ---- checks ----

    using editor::TitleCheckContext;
    using editor::TitleCheckResult;
    using editor::TitleCheckStatus;

    template <typename F>
    void forEach3DLayer(const titleart::TitleArtDoc& doc, F f) {
        titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
            if (titleart::layerTypeHasStyle(titleart::layerTypeOf(layer)) && layer.value("dimension", std::string("2d")) == "3d") f(layer);
            return true;
        });
    }

    TitleCheckResult result(const char* id, TitleCheckStatus status, const std::string& message) {
        TitleCheckResult r;
        r.id = id;
        r.status = status;
        r.message = message;
        return r;
    }

    void checkTierBudget(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        bool any = false;
        auto content = titleart::makeGeometryTitleContentProvider();
        forEach3DLayer(context.doc, [&](const Json& layer) {
            any = true;
            titleart::three_d::TitleMeshRequest request;
            request.source = titleart::three_d::collectTitleSource(layer, context.doc, *content, context.assets, nullptr);
            request.style = titleart::decodeStyle3D(layer);
            request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
            for (titleart::TitleTier tier : {titleart::TitleTier::High, titleart::TitleTier::Medium, titleart::TitleTier::Low}) {
                request.tier = tier;
                titleart::three_d::TitleMesher mesher;
                const auto mesh = mesher.build(request);
                const titleart::TitleTierFit fit = titleart::titleTierFit(mesh->stats, mesh->bytes(), tier);
                const std::string name = layer.value("name", std::string());
                TitleCheckResult r = result("3d_tier_budget", fit.fits ? TitleCheckStatus::Pass : TitleCheckStatus::Fail,
                                            "'" + name + "' at " + titleart::titleTierName(tier) + ": " +
                                                (fit.fits ? std::string("fits") : "over (" + fit.over.front() + ")"));
                r.id = std::string("3d_tier_budget.") + titleart::titleTierName(tier);
                r.measured = Json{{"layer", name}, {"triangles", mesh->stats.triangles}, {"solids", mesh->stats.solids}, {"mesh_bytes", mesh->bytes()}};
                const titleart::TitleTierLimits l = titleart::titleTierLimits(tier);
                r.threshold = Json{{"triangles", l.maxTriangles}, {"solids", l.maxSolids}, {"mesh_bytes", l.meshBytes}};
                r.method = "the layer meshed with the tier's reductions";
                if (!fit.fits) {
                    r.fix = Json{{"ops", Json::array({Json{{"op", "fit_to_tier"}, {"layer", "id:" + layer.value("id", std::string())},
                                                           {"tier", titleart::titleTierName(tier)}}})}};
                }
                out.push_back(std::move(r));
            }
        });
        if (!any) out.push_back(result("3d_tier_budget", TitleCheckStatus::Skip, "no 3D layer"));
    }

    void checkZFighting(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        bool found = false;
        forEach3DLayer(context.doc, [&](const Json& layer) {
            const titleart::Title3D style = titleart::decodeStyle3D(layer);
            for (size_t i = 0; i < style.bands.size(); i++) {
                const double offset = style.bands[i].offset;
                if (!(offset > -0.25 && offset < 0)) continue;
                found = true;
                TitleCheckResult r = result("3d_z_fighting", TitleCheckStatus::Warn,
                                            "band " + std::to_string(i) + " of '" + layer.value("name", std::string()) +
                                                "' sits less than 0.25 % of size into the solid in front: its faces may fight in depth");
                r.measured = offset;
                r.threshold = Json::array({-0.25, 0});
                r.fix = Json{{"ops", Json::array({Json{{"op", "set"}, {"layer", "id:" + layer.value("id", std::string())},
                                                       {"values", {{"band#" + std::to_string(i) + ".offset", 0}}}}})}};
                out.push_back(std::move(r));
            }
        });
        if (!found) out.push_back(result("3d_z_fighting", TitleCheckStatus::Pass, "no band offset in (-0.25 %, 0)"));
    }

    void checkEnvironment(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        bool found = false;
        const titleart::Scene3D scene = titleart::decodeScene3D(context.doc.root);
        forEach3DLayer(context.doc, [&](const Json& layer) {
            const titleart::Lighting3D lighting = titleart::resolveLighting(scene, titleart::decodeStyle3D(layer));
            const std::string& preset = lighting.environment.preset;
            const titleart::TitleEnvScene* env = titleart::titleEnvironmentPreset(preset);
            if (env && env->procedural) return;
            if (preset == "matcap" && !lighting.environment.matcap.empty()) return;
            found = true;
            TitleCheckResult r = result("3d_environment_missing", TitleCheckStatus::Warn,
                                        "'" + layer.value("name", std::string()) + "' asks for the environment '" + preset +
                                            "', which this editor cannot build: the studio lights it");
            r.measured = preset;
            r.fix = Json{{"ops", Json::array({Json{{"op", "set"}, {"values", {{"scene_3d.environment.preset", "studio"}}}}})}};
            out.push_back(std::move(r));
        });
        if (!found) out.push_back(result("3d_environment_missing", TitleCheckStatus::Pass, "every environment can be built"));
    }

    void checkLights(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        bool found = false;
        const titleart::Scene3D scene = titleart::decodeScene3D(context.doc.root);
        forEach3DLayer(context.doc, [&](const Json& layer) {
            const titleart::Lighting3D lighting = titleart::resolveLighting(scene, titleart::decodeStyle3D(layer));
            const int n = titleart::countNonAmbientLights(lighting.lights);
            if (n <= titleart::TITLE_3D_MAX_LIGHTS) return;
            found = true;
            TitleCheckResult r = result("3d_light_limit", TitleCheckStatus::Fail,
                                        "'" + layer.value("name", std::string()) + "' has " + std::to_string(n) +
                                            " directional or point lights; a 3D layer takes 4");
            r.measured = n;
            r.threshold = titleart::TITLE_3D_MAX_LIGHTS;
            out.push_back(std::move(r));
        });
        if (!found) out.push_back(result("3d_light_limit", TitleCheckStatus::Pass, "at most 4 lights per layer"));
    }

    void checkMeshFallback(TitleCheckContext& context, std::vector<TitleCheckResult>& out) {
        bool any = false, found = false;
        auto content = titleart::makeGeometryTitleContentProvider();
        forEach3DLayer(context.doc, [&](const Json& layer) {
            any = true;
            titleart::three_d::TitleMeshRequest request;
            request.source = titleart::three_d::collectTitleSource(layer, context.doc, *content, context.assets, nullptr);
            request.style = titleart::decodeStyle3D(layer);
            request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
            titleart::three_d::TitleMesher mesher;
            if (!mesher.build(request)->stats.fallbackUsed) return;
            found = true;
            out.push_back(result("3d_mesh_fallback", TitleCheckStatus::Warn,
                                 "'" + layer.value("name", std::string()) +
                                     "': touching contours made an outward bevel band take the Clipper difference fallback"));
        });
        if (!any) out.push_back(result("3d_mesh_fallback", TitleCheckStatus::Skip, "no 3D layer"));
        else if (!found) out.push_back(result("3d_mesh_fallback", TitleCheckStatus::Pass, "every band filled exactly"));
    }

}

const std::vector<std::string>& editor::title3DBandPresetNames() {
    static const std::vector<std::string> names = {"bright_ring", "dark_step", "plate", "tube_rim"};
    return names;
}

Json editor::title3DBandPreset(const std::string& name, double letterDepth) {
    if (name == "bright_ring") {
        // a ring: the band wraps the letter's side (offset = -front)
        return Json{{"name", "Bright Ring"}, {"grow", 1.5}, {"offset", -letterDepth}, {"depth", letterDepth},
                    {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "chamfer"}}}, {"material", titleart::materialPreset("gold")}};
    }
    if (name == "dark_step") {
        return Json{{"name", "Dark Step"}, {"grow", 1}, {"depth", 4}, {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "step"}}},
                    {"material", titleart::materialPreset("black_gloss")}};
    }
    if (name == "plate") {
        return Json{{"name", "Plate"}, {"grow", 4}, {"depth", 6}, {"plate", {{"kind", "contour"}, {"close", 2}}},
                    {"bevel", {{"width", 1}, {"depth", 1}, {"profile", "round"}}}, {"material", titleart::materialPreset("black_gloss")}};
    }
    return Json{{"name", "Tube Rim"}, {"grow", 2}, {"join", "round"}, {"depth", 2}, {"bevel", {{"width", 2}, {"depth", 2}, {"profile", "tube"}}},
                {"material", titleart::materialPreset("chrome")}};
}

std::vector<Title3DFix> editor::title3DTierFixes(const Json& layer, const std::string& tierName) {
    std::vector<Title3DFix> out;
    titleart::TitleTier tier;
    if (!titleart::titleTierFromName(tierName, tier)) return out;
    const titleart::Title3D style = titleart::decodeStyle3D(layer);
    const titleart::TitleTierGeometry g = titleart::titleTierGeometry(tier);
    const int curve = titleart::titleTierCurveSegments(style.curveSegments, tier, false);
    if (curve < style.curveSegments) out.push_back({"style_3d.curve_segments", curve, "curve segments for the " + tierName + " tier"});
    if (style.bevel.shape.segments > g.maxBevelSegments) {
        out.push_back({"style_3d.bevel.segments", g.maxBevelSegments, "bevel segments for the " + tierName + " tier"});
    }
    const titleart::TitleTierLimits l = titleart::titleTierLimits(tier);
    size_t enabled = 0;
    for (size_t i = 0; i < style.bands.size(); i++) {
        const titleart::Band3D& b = style.bands[i];
        if (!b.enabled) continue;
        enabled++;
        const std::string at = "style_3d.bands[" + std::to_string(i) + "]";
        if (l.maxSolids && enabled + 1 > l.maxSolids) {
            out.push_back({at + ".enabled", false, "the " + tierName + " tier draws at most " + std::to_string(l.maxSolids) + " solids"});
            continue;
        }
        if (b.bevel.shape.segments > g.maxBevelSegments) {
            out.push_back({at + ".bevel.segments", g.maxBevelSegments, "band bevel segments for the " + tierName + " tier"});
        }
    }
    return out;
}

Json editor::title3DStats(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, titleart::TitleTier tier) {
    Json out = Json::array();
    auto content = titleart::makeGeometryTitleContentProvider();
    forEach3DLayer(doc, [&](const Json& layer) {
        titleart::three_d::TitleMeshRequest request;
        request.source = titleart::three_d::collectTitleSource(layer, doc, *content, assets, nullptr);
        request.style = titleart::decodeStyle3D(layer);
        request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
        Json tiers = Json::object();
        Json row = Json{{"layer", layer.value("id", std::string())}, {"name", layer.value("name", std::string())}};
        for (titleart::TitleTier t : {tier, titleart::TitleTier::High, titleart::TitleTier::Medium, titleart::TitleTier::Low}) {
            request.tier = t;
            titleart::three_d::TitleMesher mesher;
            const auto mesh = mesher.build(request);
            if (t == tier && !row.contains("triangles")) {
                row["triangles"] = mesh->stats.triangles;
                row["vertices"] = mesh->stats.vertices;
                row["solids"] = mesh->stats.solids;
                row["build_ms"] = std::round(mesh->stats.buildMs * 10) / 10;
                row["tier"] = titleart::titleTierName(tier);
                // the supersampling the tier asks for (a render lowers it when region x k^2 passes the tier's cap)
                row["k"] = tier == titleart::TitleTier::Bake ? std::clamp(titleart::decodeScene3D(doc.root).supersample, 1, 3)
                                                              : titleart::titleTierLimits(tier).k;
                row["environment_face"] = titleart::titleEnvironmentFace(
                    titleart::resolveLighting(titleart::decodeScene3D(doc.root), request.style).environment, 1.0, tier);
                row["fallback_used"] = mesh->stats.fallbackUsed;
            }
            if (t == titleart::TitleTier::Bake) continue;
            const titleart::TitleTierFit fit = titleart::titleTierFit(mesh->stats, mesh->bytes(), t);
            tiers[titleart::titleTierName(t)] = Json{{"fits", fit.fits}, {"over", fit.over}};
        }
        row["tiers"] = tiers;
        out.push_back(row);
    });
    return out;
}

bool editor::cookTitleMeshes(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, titleart::TitleTier tier,
                             std::map<std::string, std::vector<uint8_t>>& out, std::string& error) {
    auto content = titleart::makeGeometryTitleContentProvider();
    bool ok = true;
    forEach3DLayer(doc, [&](const Json& layer) {
        if (!ok) return;
        titleart::three_d::TitleMeshRequest request;
        std::vector<titleart::TitleProblem> problems;
        request.source = titleart::three_d::collectTitleSource(layer, doc, *content, assets, &problems);
        request.style = titleart::decodeStyle3D(layer);
        request.blocks = titleart::three_d::collectCharBlocks(layer, request.style.bands.size());
        request.tier = tier;
        const Json& transform = layer.contains("transform") ? layer["transform"] : Json();
        request.outputScale = transform.is_object() ? std::abs(transform.value("scale", 1.0)) : 1.0;
        const std::string id = layer.value("id", std::string());
        if (request.source.glyphs.empty()) {
            error = "the 3D layer " + id + " has nothing to mesh" + (problems.empty() ? std::string() : ": " + problems.front().message);
            ok = false;
            return;
        }
        titleart::three_d::TitleMesher mesher;
        out[id] = titleart::writeTitleMesh(*mesher.build(request));
    });
    return ok;
}

Json editor::title3DLayerInfo(const Json& layer, const titleart::TitleArtDoc& doc) {
    const titleart::Title3D style = titleart::decodeStyle3D(layer);
    const titleart::Scene3D scene = titleart::decodeScene3D(doc.root);
    const titleart::Lighting3D lighting = titleart::resolveLighting(scene, style);
    Json sources = Json::object();
    for (const auto& [key, level] : lighting.sources) sources[key] = titleart::lightingLevelName(level);
    const titleart::Environment3D& e = lighting.environment;
    Json environment = Json{{"preset", e.preset}, {"intensity", e.intensity}, {"rotation", e.rotation}, {"tint", titleart::formatTitleColor(e.tint)},
                            {"contrast", e.contrast}, {"saturation", e.saturation}, {"blur", e.blur}, {"repeat_x", e.repeatX},
                            {"repeat_y", e.repeatY}};
    return Json{{"style_3d", titleart::effectiveStyle3DJson(layer)},
                {"derived", style.derived},
                {"lighting", {{"environment", environment},
                              {"tone_mapping", titleart::toneMappingName(lighting.toneMapping)},
                              {"exposure", lighting.exposure},
                              {"lights", (int)lighting.lights.size()},
                              {"sources", sources}}}};
}

std::shared_ptr<const titleart::TitleHdri> editor::loadBundledTitleHdri(const std::string& slug, const std::filesystem::path& dir) {
    // the manifest names each slug's file (fetch_hdris.py writes it)
    std::string text;
    if (!assetio::readFile(dir / "manifest.json", text)) return nullptr;
    const Json manifest = Json::parse(text, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("environments")) return nullptr;
    for (const auto& [env, entry] : manifest["environments"].items()) {
        if (entry.value("slug", std::string()) != slug) continue;
        int w = 0, h = 0, n = 0;
        float* pixels = stbi_loadf((dir / entry.value("file", std::string())).string().c_str(), &w, &h, &n, 4);
        if (!pixels) return nullptr;
        auto hdri = std::make_shared<titleart::TitleHdri>();
        hdri->width = w;
        hdri->height = h;
        hdri->rgba.resize((size_t)w * h * 4);
        for (size_t i = 0; i < hdri->rgba.size(); i++) hdri->rgba[i] = floatToHalf(pixels[i]);
        stbi_image_free(pixels);
        return hdri;
    }
    return nullptr;
}

void editor::registerTitle3DEditor() {
    static bool done = false;
    if (done) return;
    done = true;
    // the bundled HDRIs beside the editor (macOS: in the bundle's Resources)
    titleart::setTitleHdriLoader([](const std::string& slug) -> std::shared_ptr<const titleart::TitleHdri> {
        const std::filesystem::path exe = FileUtils::getExecutableDir();
        for (const std::filesystem::path& dir : {exe / "titleart" / "hdri", exe.parent_path() / "Resources" / "titleart" / "hdri"}) {
            if (auto hdri = loadBundledTitleHdri(slug, dir)) return hdri;
        }
        return nullptr;
    });
    TitleArtOps::registerOp("set_dimension", setDimension);
    TitleArtOps::registerOp("add_band", addBand);
    TitleArtOps::registerOp("remove_band", [](TitleOpContext& ctx, const Json& op) { return removeOrMoveBand(ctx, op, false); });
    TitleArtOps::registerOp("move_band", [](TitleOpContext& ctx, const Json& op) { return removeOrMoveBand(ctx, op, true); });
    TitleArtOps::registerOp("apply_material", applyMaterial);
    TitleArtOps::registerOp("fit_to_tier", fitToTier);
    struct Row {
        const char* id;
        const char* description;
        void (*run)(TitleCheckContext&, std::vector<TitleCheckResult>&);
    };
    const Row rows[] = {
        {"3d_tier_budget", "3D layers against the Realtime tiers' triangle, solid and mesh budgets", checkTierBudget},
        {"3d_z_fighting", "band offsets that let coplanar faces fight in depth", checkZFighting},
        {"3d_environment_missing", "environments this editor cannot build", checkEnvironment},
        {"3d_light_limit", "more than 4 directional or point lights on a 3D layer", checkLights},
        {"3d_mesh_fallback", "outward bevel bands cut by the Clipper difference fallback", checkMeshFallback},
    };
    for (const Row& r : rows) {
        CheckDef def;
        def.id = r.id;
        def.kind = TitleCheckKind::Document;
        def.owner = "S86";
        def.group = "3d";
        def.description = r.description;
        auto run = r.run;
        def.run = [run](TitleCheckContext& context, std::vector<TitleCheckResult>& out) { run(context, out); };
        TitleArtChecks::registerCheck(std::move(def));
    }
}

std::string editor::copyTitleHdriIntoAssets(const std::filesystem::path& source, const std::filesystem::path& assets, std::string* error) {
    namespace fs = std::filesystem;
    const auto refuse = [error](const std::string& message) {
        if (error) *error = message;
        return std::string();
    };
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (extension != ".hdr" && extension != ".exr") return refuse("'" + source.filename().string() + "' is not an .hdr or .exr file");
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) return refuse("'" + source.string() + "' does not exist");
    const fs::path inside = fs::weakly_canonical(source, ec).lexically_relative(fs::weakly_canonical(assets, ec));
    if (!inside.empty() && *inside.begin() != "..") return source.string();
    const fs::path folder = assets / "TitleArt" / "HDRI";
    fs::create_directories(folder, ec);
    if (ec) return refuse("could not create '" + folder.string() + "': " + ec.message());
    const auto sameFile = [&source](const fs::path& other) {
        std::error_code sizeError;
        if (fs::file_size(source, sizeError) != fs::file_size(other, sizeError) || sizeError) return false;
        std::ifstream a(source, std::ios::binary), b(other, std::ios::binary);
        return std::equal(std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(b));
    };
    fs::path destination = folder / source.filename();
    for (int n = 2; fs::exists(destination, ec); n++) {
        if (sameFile(destination)) return destination.string();
        destination = folder / (source.stem().string() + " " + std::to_string(n) + source.extension().string());
    }
    if (!FileUtils::copyFileAtomic(source, destination, ec)) return refuse("could not copy '" + source.filename().string() + "': " + ec.message());
    return destination.string();
}

