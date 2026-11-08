// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The layer plan is ported from Pachislop src/io/psd.ts (titleParts, siblingsPlan, nodePlan,
// stackPlan, build) by SafEight, used with the author's permission.

#include "PsdExport.h"

#include "PsStyleConvert.h"
#include "PsdAdjustments.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    std::string str(const Json& o, const char* key) {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    bool anyEnabled(const Json& list) {
        if (!list.is_array()) return false;
        for (const Json& e : list) if (e.value("enabled", true)) return true;
        return false;
    }

    bool masked(const Json& layer) {
        return anyEnabled(layer.value("masks", Json::array()));
    }

    // every id of a layer's subtree
    void subtree(const Json& layer, std::set<std::string>& out) {
        out.insert(str(layer, "id"));
        for (const Json& c : titleart::layerChildren(layer)) subtree(c, out);
    }

    struct Planner {
        const TitleArtDoc& doc;
        const PsdPlanOptions& options;
        PsdExportPlan plan;

        struct Inherited {
            bool excluded = false;
            bool alternate = false;     // inside a group whose baked render stands in for it
            std::vector<int> clipBases;
        };

        int addJob(std::shared_ptr<const TitleArtDoc> d, const std::string& isolate, const char* purpose) {
            plan.jobs.push_back({std::move(d), isolate, purpose});
            return (int)plan.jobs.size() - 1;
        }

        // the document with layer `id` changed by `edit` (a copy; `doc` untouched)
        std::shared_ptr<TitleArtDoc> variant(const std::string& id, const std::function<void(Json&)>& edit) const {
            auto copy = std::make_shared<TitleArtDoc>(doc);
            if (Json* l = titleart::findLayer(copy->root, id)) edit(*l);
            return copy;
        }

        // the layer alone, drawn whole: its masks off (they go on the record), unclipped, visible
        int alone(const Json& layer, bool dropMasks, const std::function<void(Json&)>& edit = {}) {
            auto d = variant(str(layer, "id"), [&](Json& l) {
                l["clip"] = false;
                l["visible"] = true;
                if (dropMasks) l["masks"] = Json::array();
                if (edit) edit(l);
            });
            return addJob(d, str(layer, "id"), "layer");
        }

        // a layer's combined masks as alpha: a white full-canvas fill wearing them
        int maskJob(const Json& layer) {
            auto d = std::make_shared<TitleArtDoc>(doc);
            Json fill = Json{{"type", "generator"}, {"name", "mask"}, {"id", str(layer, "id")},
                             {"generator", Json{{"type", "fill"}, {"params", Json{{"kind", "solid"}, {"color", "#ffffff"}}}}},
                             {"transform", layer.value("transform", Json::object())}, {"masks", layer.value("masks", Json::array())}};
            d->root["layers"] = Json::array({fill});
            titleart::normalizeTitleArt(*d, nullptr);
            return addJob(d, "", "mask");
        }

        // only `keep` (and their ancestors) visible, drawn whole: a baked adjustment
        int keepJob(const std::set<std::string>& keep) {
            auto d = std::make_shared<TitleArtDoc>(doc);
            std::function<bool(Json&)> walk = [&](Json& l) {
                bool any = keep.count(str(l, "id")) > 0;
                if (l.contains("children")) {
                    for (Json& c : l["children"]) any = walk(c) || any;
                }
                if (!any) l["visible"] = false;
                return any;
            };
            for (Json& l : d->root["layers"]) walk(l);
            return addJob(d, "", "layer");
        }

        std::string blendKey(const Json& layer) {
            const std::string blend = layer.value("blend", std::string("normal"));
            const std::string key = psKeyOfTitleBlend(blend);
            if (key.empty()) {
                plan.warnings.push_back("blend_unavailable: the layer \"" + str(layer, "name") + "\" blends " + blend +
                                        ", which Photoshop lacks: written as Normal");
                return "norm";
            }
            return key;
        }

        static uint8_t byteOf(double v) {
            return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255);
        }

        bool hiddenOf(const Json& layer, const Inherited& f) const {
            return f.excluded || f.alternate || !layer.value("visible", true);
        }

        bool excludedOf(const Json& layer, const Inherited& f) const {
            return f.excluded || (!options.includeGuides && layer.value("guide", false));
        }

        // the native Photoshop record of an adjustment, false when it is baked; `skip` for an
        // Effects Only adjustment without enabled effects (it changes nothing)
        bool nativeAdjustment(const Json& layer, PsdPlanNode& node, bool& skip) {
            skip = false;
            const Json adjustment = layer.value("adjustment", Json::object());
            const bool effects = anyEnabled(layer.value("effects", Json::array()));
            if (adjustment.value("type", std::string()) == "effects_only" && !effects) {
                skip = true;
                return false;
            }
            if (effects) return false;
            std::string key;
            std::vector<uint8_t> data;
            if (!titleAdjustmentToPsd(adjustment, key, data)) return false;
            node.kind = PsdPlanNode::Kind::Adjust;
            node.adjustmentKey = key;
            node.adjustmentData = std::move(data);
            return true;
        }

        void bakedWarning(const Json& layer) {
            plan.warnings.push_back("adjustment_baked: the adjustment \"" + str(layer, "name") + "\" (" +
                                    layer.value("adjustment", Json::object()).value("type", std::string()) +
                                    ") has no Photoshop adjustment layer here: written as the pixel layer \"" + str(layer, "name") +
                                    " (baked)\" with what it covers hidden");
        }

        static void hideAll(std::vector<PsdPlanNode>& nodes) {
            for (PsdPlanNode& n : nodes) n.hidden = true;
        }

        // the drawn siblings (bottom first) with the layers clipped to each
        struct Node {
            const Json* layer;
            std::vector<const Json*> clipped;
        };

        static std::vector<Node> group(const Json& list) {
            std::vector<Node> nodes;
            long base = -1;
            for (const Json& l : list) {
                const bool clip = l.value("clip", false);
                if (clip && base >= 0) {
                    nodes[(size_t)base].clipped.push_back(&l);
                    continue;
                }
                nodes.push_back({&l, {}});
                base = titleart::layerTypeOf(l) == titleart::LayerType::Adjustment ? -1 : (long)nodes.size() - 1;
            }
            return nodes;
        }

        std::vector<PsdPlanNode> siblings(const Json& list, const Inherited& inherited) {
            std::vector<PsdPlanNode> out;
            const std::vector<Node> nodes = group(list);
            for (size_t i = 0; i < nodes.size(); i++) {
                const Json& l = *nodes[i].layer;
                Inherited f = inherited;
                f.excluded = excludedOf(l, inherited);
                if (titleart::layerTypeOf(l) != titleart::LayerType::Adjustment) {
                    for (PsdPlanNode& n : node(nodes[i], f, false)) out.push_back(std::move(n));
                    continue;
                }
                PsdPlanNode a;
                a.name = str(l, "name");
                a.hidden = hiddenOf(l, f);
                a.blendKey = blendKey(l);
                a.opacity = byteOf(l.value("opacity", 1.0));
                if (masked(l)) a.maskJob = maskJob(l);
                bool skip = false;
                if (nativeAdjustment(l, a, skip)) {
                    out.push_back(std::move(a));
                    continue;
                }
                if (skip) continue;
                bakedWarning(l);
                // everything below it in its parent, adjusted: it stands in for those layers, now hidden
                hideAll(out);
                std::set<std::string> keep;
                for (size_t k = 0; k <= i; k++) {
                    subtree(*nodes[k].layer, keep);
                    for (const Json* c : nodes[k].clipped) subtree(*c, keep);
                }
                PsdPlanNode baked;
                baked.name = str(l, "name") + " (baked)";
                baked.hidden = f.excluded || f.alternate;
                baked.job = keepJob(keep);
                out.push_back(std::move(baked));
            }
            return out;
        }

        // the parts of a 2D title, bottom first: each enabled rim alone (outermost at the bottom),
        // the face alone (hidden; their own part effects only), then the whole layer ("fx")
        std::vector<PsdPlanNode> titleParts(const Json& l, bool dropMasks) {
            std::vector<PsdPlanNode> parts;
            const Json rims = l["style"].value("rims", Json::array());
            const Json partEffects = l.value("part_effects", Json::object());
            const auto onlyPart = [&](const std::string& key) {
                Json out = Json::object();
                if (partEffects.contains(key)) out[key] = partEffects[key];
                return out;
            };
            double grown = 0;
            for (size_t i = 0; i < rims.size(); i++) {
                const Json& rim = rims[i];
                if (!rim.value("enabled", true) || !(rim.value("width", 0.0) > 0)) continue;
                grown += rim.value("width", 0.0);
                Json single = rim;
                single["width"] = grown;
                const std::string key = "rim:" + str(rim, "id");
                PsdPlanNode part;
                part.name = "rim " + std::to_string(i + 1);
                part.hidden = true;
                part.blendKey = blendKey(l);
                part.opacity = byteOf(l.value("opacity", 1.0));
                part.job = alone(l, dropMasks, [&](Json& v) {
                    v["effects"] = Json::array();
                    v["part_effects"] = onlyPart(key);
                    v["style"]["rims"] = Json::array({single});
                    v["style"]["face"]["opacity"] = 0;
                });
                parts.push_back(std::move(part));
            }
            std::reverse(parts.begin(), parts.end());
            PsdPlanNode face;
            face.name = "face";
            face.hidden = true;
            face.blendKey = blendKey(l);
            face.opacity = byteOf(l.value("opacity", 1.0));
            face.job = alone(l, dropMasks, [&](Json& v) {
                v["effects"] = Json::array();
                v["part_effects"] = onlyPart("face");
                v["style"]["rims"] = Json::array();
            });
            parts.push_back(std::move(face));
            PsdPlanNode fx;
            fx.name = "fx";
            fx.hidden = false;
            fx.blendKey = blendKey(l);
            fx.opacity = byteOf(l.value("opacity", 1.0));
            fx.job = alone(l, dropMasks);
            parts.push_back(std::move(fx));
            return parts;
        }

        std::vector<PsdPlanNode> node(const Node& n, const Inherited& f, bool clipping) {
            const Json& l = *n.layer;
            const titleart::LayerType type = titleart::layerTypeOf(l);
            const bool isMasked = masked(l);
            PsdPlanNode own;
            own.name = str(l, "name");
            own.hidden = hiddenOf(l, f);
            own.opacity = byteOf(l.value("opacity", 1.0));
            own.clipping = clipping;
            own.clipBases = f.clipBases;
            if (isMasked) own.maskJob = maskJob(l);
            std::vector<PsdPlanNode>* host = nullptr;
            const bool split = options.splitTitles && titleart::layerTypeHasStyle(type) && type != titleart::LayerType::Image &&
                               l.value("dimension", std::string("2d")) == "2d";
            if (type == titleart::LayerType::Group) {
                const bool baked = anyEnabled(l.value("effects", Json::array()));
                Inherited inner = f;
                inner.alternate = f.alternate || baked;
                own.kind = PsdPlanNode::Kind::Group;
                own.blendKey = blendKey(l);
                own.children = siblings(titleart::layerChildren(l), inner);
                if (baked) {
                    PsdPlanNode fx;
                    fx.name = "fx";
                    fx.hidden = f.excluded || f.alternate;
                    fx.job = alone(l, isMasked);
                    own.children.push_back(std::move(fx));
                }
            } else if (split) {
                own.kind = PsdPlanNode::Kind::Group;
                own.blendKey = "pass";
                own.children = titleParts(l, isMasked);
                host = &own.children;
            } else {
                own.blendKey = blendKey(l);
                own.job = alone(l, isMasked);
            }
            std::vector<PsdPlanNode> out;
            if (n.clipped.empty()) {
                out.push_back(std::move(own));
                return out;
            }
            // Photoshop clips to a pixel layer only (a split title's fx part); a group cannot be a base
            const bool pixelBase = type != titleart::LayerType::Group;
            if (!pixelBase) {
                plan.warnings.push_back("clip_to_group: layers clipped to the group \"" + own.name +
                                        "\" are written already masked, without a clipping mask: Photoshop cannot clip to a group");
            }
            // the base's alpha before its effects (a pixel base clips with its own layer mask in Photoshop)
            const int base = alone(l, pixelBase, [](Json& v) {
                v["effects"] = Json::array();
                v["part_effects"] = Json::object();
            });
            plan.jobs[(size_t)base].purpose = "clip_base";
            Inherited cf = f;
            cf.clipBases.push_back(base);
            std::vector<PsdPlanNode> clipped;
            for (size_t i = 0; i < n.clipped.size(); i++) {
                const Json& c = *n.clipped[i];
                Inherited ff = cf;
                ff.excluded = excludedOf(c, cf);
                if (titleart::layerTypeOf(c) != titleart::LayerType::Adjustment) {
                    for (PsdPlanNode& p : node(Node{&c, {}}, ff, pixelBase)) clipped.push_back(std::move(p));
                    continue;
                }
                PsdPlanNode a;
                a.name = str(c, "name");
                a.hidden = hiddenOf(c, ff);
                a.blendKey = blendKey(c);
                a.opacity = byteOf(c.value("opacity", 1.0));
                a.clipping = true;
                if (masked(c)) a.maskJob = maskJob(c);
                bool skip = false;
                if (pixelBase && nativeAdjustment(c, a, skip)) {
                    clipped.push_back(std::move(a));
                    continue;
                }
                if (skip) continue;
                bakedWarning(c);
                // its clip group up to it, adjusted, opaque inside the base
                std::set<std::string> keep;
                subtree(l, keep);
                for (size_t k = 0; k <= i; k++) subtree(*n.clipped[k], keep);
                PsdPlanNode baked;
                baked.name = str(c, "name") + " (baked)";
                baked.hidden = ff.excluded || ff.alternate;
                baked.job = keepJob(keep);
                baked.clipping = pixelBase;
                baked.opaque = true;
                baked.clipBases = cf.clipBases;
                clipped.push_back(std::move(baked));
            }
            if (host) {
                for (PsdPlanNode& p : clipped) host->push_back(std::move(p));
                out.push_back(std::move(own));
                return out;
            }
            out.push_back(std::move(own));
            for (PsdPlanNode& p : clipped) out.push_back(std::move(p));
            return out;
        }
    };

    // the alpha of sample i (0..1)
    double alphaAt(const texsynth::Image& img, size_t i) {
        if (img.bitDepth == 16) {
            uint16_t v;
            std::memcpy(&v, img.data.data() + (i * 4 + 3) * 2, 2);
            return v / 65535.0;
        }
        return img.data[i * 4 + 3] / 255.0;
    }

    void setAlpha(texsynth::Image& img, size_t i, double a) {
        if (img.bitDepth == 16) {
            const uint16_t v = (uint16_t)std::lround(std::clamp(a, 0.0, 1.0) * 65535);
            std::memcpy(img.data.data() + (i * 4 + 3) * 2, &v, 2);
        } else {
            img.data[i * 4 + 3] = (uint8_t)std::lround(std::clamp(a, 0.0, 1.0) * 255);
        }
    }

    bool assembleNodes(const PsdExportPlan& plan, const std::vector<texsynth::Image>& renders, const std::vector<PsdPlanNode>& nodes,
                       std::vector<PsdWriteLayer>& out, int w, int h, int bps, std::string& error) {
        for (const PsdPlanNode& n : nodes) {
            PsdWriteLayer l;
            l.name = n.name;
            l.visible = !n.hidden;
            l.blendKey = n.blendKey;
            l.opacity = n.opacity;
            l.fillOpacity = n.fillOpacity;
            l.clipping = n.clipping;
            if (n.maskJob >= 0) {
                const texsynth::Image& m = renders[(size_t)n.maskJob];
                l.hasMask = true;
                l.maskWidth = w;
                l.maskHeight = h;
                l.mask.assign((size_t)w * h * bps, 0);
                for (size_t i = 0; i < (size_t)w * h; i++) {
                    const double a = alphaAt(m, i);
                    if (bps == 2) {
                        const uint16_t v = (uint16_t)std::lround(a * 65535);
                        std::memcpy(l.mask.data() + i * 2, &v, 2);
                    } else {
                        l.mask[i] = (uint8_t)std::lround(a * 255);
                    }
                }
            }
            if (n.kind == PsdPlanNode::Kind::Group) {
                l.group = true;
                if (!assembleNodes(plan, renders, n.children, l.children, w, h, bps, error)) return false;
            } else if (n.kind == PsdPlanNode::Kind::Adjust) {
                l.adjustmentKey = n.adjustmentKey;
                l.adjustmentData = n.adjustmentData;
            } else {
                texsynth::Image img = renders[(size_t)n.job];
                for (size_t i = 0; i < (size_t)w * h; i++) {
                    double a = n.opaque ? (alphaAt(img, i) > 0 ? 1.0 : 0.0) : alphaAt(img, i);
                    for (int b : n.clipBases) a *= alphaAt(renders[(size_t)b], i);
                    setAlpha(img, i, a);
                }
                l.width = w;
                l.height = h;
                l.rgba = std::move(img.data);
            }
            out.push_back(std::move(l));
        }
        return true;
    }

}

PsdExportPlan psd::planPsdExport(const TitleArtDoc& doc, const PsdPlanOptions& options) {
    Planner p{doc, options, {}};
    p.plan.mergedJob = p.addJob(std::make_shared<TitleArtDoc>(doc), "", "merged");
    p.plan.nodes = p.siblings(doc.layers(), {});
    return std::move(p.plan);
}

bool psd::assemblePsd(const PsdExportPlan& plan, const std::vector<texsynth::Image>& renders, int bitDepth, double dpi, PsdWriteDocument& out,
                      std::string& error) {
    if (renders.size() != plan.jobs.size() || renders.empty()) {
        error = "render_failed: " + std::to_string(renders.size()) + " renders for " + std::to_string(plan.jobs.size()) + " jobs";
        return false;
    }
    const texsynth::Image& merged = renders[(size_t)plan.mergedJob];
    const int bps = bitDepth == 16 ? 2 : 1;
    for (const texsynth::Image& r : renders) {
        if (r.width != merged.width || r.height != merged.height || r.channels != 4 || r.bitDepth != bitDepth) {
            error = "render_failed: the renders are not all " + std::to_string(merged.width) + " x " + std::to_string(merged.height) + " RGBA " +
                    std::to_string(bitDepth) + "-bit";
            return false;
        }
    }
    out = PsdWriteDocument();
    out.width = merged.width;
    out.height = merged.height;
    out.bitDepth = bitDepth;
    out.dpi = dpi;
    out.merged = merged.data;
    return assembleNodes(plan, renders, plan.nodes, out.layers, merged.width, merged.height, bps, error);
}

bool psd::exportTitlePsd(Project* project, const TitleArtDoc& doc, const PsdExportOptions& options, PsdExportResult& out, std::string& error) {
    if (options.bitDepth != 8 && options.bitDepth != 16) {
        error = "bit_depth_unsupported: a PSD is 8- or 16-bit";
        return false;
    }
    PsdPlanOptions planOptions;
    planOptions.splitTitles = options.splitTitles;
    const PsdExportPlan plan = planPsdExport(doc, planOptions);
    std::vector<texsynth::Image> renders;
    renders.reserve(plan.jobs.size());
    const titleart::TitleRectI frame = options.frame.empty() ? titleart::TitleRectI{0, 0, doc.canvasWidth(), doc.canvasHeight()} : options.frame;
    for (const PsdRenderJob& job : plan.jobs) {
        titleart::RenderRequest request;
        request.doc = job.doc;
        request.isolateLayer = job.isolateLayer;
        request.pxScale = (float)options.scale;
        request.region = frame;
        request.outputBitDepth = options.bitDepth;
        request.testFlags = options.testFlags;
        TitleRenderedImage image;
        std::string renderError;
        if (!TitleArtRenderHost::editor().render(project, std::move(request), image, renderError)) {
            error = renderError.rfind("bit_depth_unsupported", 0) == 0 ? renderError : "render_failed: " + renderError;
            return false;
        }
        renders.push_back(std::move(image.image));
        out.renders++;
    }
    PsdWriteDocument psd;
    const double dpi = doc.root.value("dpi", 72.0);
    if (!assemblePsd(plan, renders, options.bitDepth, dpi, psd, error)) return false;
    renders.clear();
    if (!writePsd(psd, out.bytes, error)) return false;
    out.warnings = plan.warnings;
    out.warnings.push_back("psd_blending: Photoshop blends layers in sRGB and the engine in linear light, so the layers recombined in Photoshop "
                           "differ where they overlap with partial alpha; the merged image is the true render");
    return true;
}
