// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R1, R5, R29, R34, R40, R45)
// - Purpose: the planner's side of a Dimension 3D layer (TitlePlanner::title3D*): at planning, its
//   source outlines, mesh (the mesher's caches), materials and slot table, lighting, rigs, layer
//   matrix, camera, region (the projected bounds of the mesh, clamped to the request's region)
//   and k; then one step that renders the region into a node target the S80 compositor takes
//   like any layer's (premultiplied, display referred).
// - k is the largest of {the tier's k (Bake: the document's `supersample`), 2, 1} whose region
//   px x k^2 fits the tier's cap and whose sides fit the device's largest texture.
// - A document without 3D layers never reaches this file (R45).
// - Thread-safety: main thread (the planner's).

#include "titleart/render/TitlePlanner.h"

#include "render/SystemRender.h"
#include "titleart/TitleArtHash.h"
#include "titleart/render/TitleContent.h"
#include "titleart/render/TitleFrame.h"
#include "titleart/three_d/Title3DMath.h"
#include "titleart/three_d/Title3DRenderer.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleTierFeatures.h"
#if defined(JASPERENGINE_TITLE_ART_MESHER) && JASPERENGINE_TITLE_ART_MESHER
#include "titleart/three_d/mesher/TitleMesher.h"
#endif

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    TitleTier tierOf(int requestTier) {
        switch (requestTier) {
            case 1: return TitleTier::High;
            case 2: return TitleTier::Medium;
            case 3: return TitleTier::Low;
            default: return TitleTier::Bake;
        }
    }

    geometry::Affine inverseAffine(const geometry::Affine& a) {
        const double det = a.a * a.d - a.b * a.c;
        geometry::Affine r;
        if (std::abs(det) < 1e-30) return r;
        r.a = a.d / det;
        r.b = -a.b / det;
        r.c = -a.c / det;
        r.d = a.a / det;
        r.e = -(r.a * a.e + r.c * a.f);
        r.f = -(r.b * a.e + r.d * a.f);
        return r;
    }

#if defined(JASPERENGINE_TITLE_ART_MESHER) && JASPERENGINE_TITLE_ART_MESHER
    // the process's mesher: its shape, mesh and inflate caches are CPU data any renderer shares
    three_d::TitleMesher& mesher() {
        static three_d::TitleMesher instance(32, 64);
        return instance;
    }
#endif

    const Json& memberOf(const Json& object, const char* key) {
        static const Json none;
        auto it = object.find(key);
        return it != object.end() ? *it : none;
    }

    // the Title Lit output of a debug view (TitleLitBits::OUTPUT_SHIFT)
    uint32_t debugOutput(Title3DDebugView view) {
        switch (view) {
            case Title3DDebugView::Normals: return 1;
            case Title3DDebugView::Environment: return 2;
            default: return 0;
        }
    }

}

bool TitlePlanner::is3D(const DrawNode& node) const {
    return node.layer && layerTypeHasStyle(node.type) && node.layer->value("dimension", std::string("2d")) == "3d";
}

std::vector<std::string> TitlePlanner::title3DParts(const Json& layer) {
    std::vector<std::string> parts;
    const Title3D style = decodeStyle3D(layer);
    for (auto it = style.bands.rbegin(); it != style.bands.rend(); ++it) {
        if (it->enabled) parts.push_back("band:" + it->id);
    }
    parts.push_back("side");
    if (style.bevel.width > 0 && style.mode == Mode3D::Extrude) parts.push_back("bevel");
    parts.push_back("face");
    return parts;
}

namespace jasperengine::titleart {

    // the plan of a layer (made once per planner)
    static std::shared_ptr<Title3DPlan> makeTitle3DPlan(TitlePlanner& planner, const DrawNode& node, const geometry::Affine& parent,
                                                        TitleRenderServices& svc, const RenderRequest& req) {
        auto plan = std::make_shared<Title3DPlan>();
        const Json& layer = *node.layer;
        const TitleArtDoc& doc = planner.doc();
        plan->layerId = node.layerId;
        plan->style = decodeStyle3D(layer);
        plan->tier = tierOf(req.tier);
        plan->debugView = (Title3DDebugView)std::clamp(req.debugView3D, 0, (int)Title3DDebugView::Wireframe);
        plan->canvasWidth = doc.canvasWidth();
        plan->canvasHeight = doc.canvasHeight();
        plan->size = layerSourceSize(layer);
        const Scene3D scene = decodeScene3D(doc.root);
        plan->supersample = scene.supersample;
        plan->lighting = resolveLighting(scene, plan->style);
        if (debugOutput(plan->debugView) != 0 || plan->debugView == Title3DDebugView::Wireframe) {
            // the geometry views show their values as they are
            plan->lighting.toneMapping = ToneMapping3D::None;
            plan->lighting.exposure = 1;
        }
        plan->camera = plan->style.camera ? *plan->style.camera : scene.camera;
        if (countNonAmbientLights(plan->lighting.lights) > TITLE_3D_MAX_LIGHTS) {
            TitleProblem p;
            p.code = "3d_light_limit";
            p.layer = node.layerId;
            p.path = "style_3d.lighting.lights";
            p.message = "more than " + std::to_string(TITLE_3D_MAX_LIGHTS) + " directional or point lights: only the first " +
                        std::to_string(TITLE_3D_MAX_LIGHTS) + " light the layer";
            planner.problem(p);
        }
        plan->model = title3DLayerMatrix(memberOf(layer, "transform"), parent);
        plan->layerScale = std::max(1e-6, std::sqrt((double)plan->model.at(0, 0) * plan->model.at(0, 0) + (double)plan->model.at(1, 0) * plan->model.at(1, 0) +
                                                    (double)plan->model.at(2, 0) * plan->model.at(2, 0)));

        // the mesh: a cooked one at this tier (R38), else the mesher's
        const auto cooked = req.cookedMeshes3D.find(node.layerId);
        const bool useCooked = cooked != req.cookedMeshes3D.end() && cooked->second && cooked->second->tier == plan->tier;
        bool mesherAvailable = (req.testFlags & TITLE_ART_TEST_NO_MESHER) == 0;
#if !(defined(JASPERENGINE_TITLE_ART_MESHER) && JASPERENGINE_TITLE_ART_MESHER)
        mesherAvailable = false;
#endif
        if (useCooked) {
            plan->mesh = cooked->second;
        } else if (!mesherAvailable) {
            TitleProblem p;
            p.code = "mesher_unavailable";
            p.layer = node.layerId;
            p.message = "this build has no 3D mesher and the layer has no cooked mesh at the " + std::string(titleTierName(plan->tier)) + " tier";
            planner.problem(p);
            return plan;
        }
#if defined(JASPERENGINE_TITLE_ART_MESHER) && JASPERENGINE_TITLE_ART_MESHER
        if (!useCooked) {
            if (!svc.content) return plan;
            std::vector<TitleProblem> problems;
            three_d::TitleMeshRequest meshRequest;
            meshRequest.source = three_d::collectTitleSource(layer, doc, *svc.content, req.assets, &problems);
            for (TitleProblem& p : problems) {
                if (p.layer.empty()) p.layer = node.layerId;
                planner.problem(p);
            }
            meshRequest.style = plan->style;
            meshRequest.blocks = three_d::collectCharBlocks(layer, plan->style.bands.size());
            meshRequest.tier = plan->tier;
            meshRequest.outputScale = planner.pxScale() * planner.placementOf(layer).scale;
            // the wrap stream only when a material projects its base texture `wrap`
            auto wraps = [](const TitleMaterial3D& m) { return m.baseWrap && !m.baseTexture.empty(); };
            meshRequest.wrap = wraps(plan->style.face) || (plan->style.bevelMaterial && wraps(*plan->style.bevelMaterial)) ||
                               (plan->style.side && wraps(*plan->style.side));
            for (const Band3D& b : plan->style.bands) meshRequest.wrap = meshRequest.wrap || wraps(b.material) || (b.side && wraps(*b.side));
            if (meshRequest.source.glyphs.empty()) return plan;
            plan->mesh = mesher().build(meshRequest);
            if (plan->mesh->stats.fallbackUsed) {
                TitleProblem p;
                p.severity = TitleSeverity::Info;
                p.code = "mesh_fallback";
                p.layer = node.layerId;
                p.message = "an outward bevel band was cut by the Clipper difference (touching contours)";
                planner.problem(p);
            }
        }
#endif
        if (!plan->mesh) return plan;
        const TitleMesh& mesh = *plan->mesh;

        // the environments: the layer's, then each part's own (a material with environment overrides)
        auto environmentOf = [&](const Environment3D& env, const std::string& path) {
            Title3DEnvPlan e;
            e.environment = env;
            if (env.preset == "matcap") {
                e.matcap = planner.title3DAsset(encodeAssetRef(env.matcap), node.layerId, path + ".matcap");
            }
            const TitleEnvScene* scene3d = titleEnvironmentPreset(env.preset);
            if (scene3d && !scene3d->procedural) {
                e.hdri = loadTitleHdri(scene3d->hdri);
                e.hdriYaw = titleHdriYaw(env.preset);
            }
            // a photo, softbox or custom HDRI not built into this renderer lights with the studio
            // and reports it (unless the request brings its cooked cube)
            const bool cookedHere = req.cookedEnvironments3D.count(env.preset) > 0;
            if (!cookedHere && (!scene3d || (!scene3d->procedural && !e.hdri))) {
                if (env.preset != "matcap") {
                    TitleProblem p;
                    p.code = "environment_missing";
                    p.layer = node.layerId;
                    p.path = path + ".preset";
                    p.message = "the environment '" + env.preset + "' has no HDRI here: the studio lights the layer";
                    planner.problem(p);
                    e.missing = true;
                }
                scene3d = titleEnvironmentPreset("studio");
                e.hdri.reset();
            }
            if (!scene3d) scene3d = titleEnvironmentPreset("studio");
            e.scene = scene3d;
            // a cooked cube of the preset (a player's): uploaded as it is
            const auto cookedEnv = req.cookedEnvironments3D.find(env.preset);
            if (cookedEnv != req.cookedEnvironments3D.end() && cookedEnv->second) {
                e.cooked = cookedEnv->second;
                e.missing = false;
                e.hdri.reset();
                e.key = hashCombine(fnv1a64("cooked:" + env.preset), (uint64_t)(uintptr_t)cookedEnv->second.get());
                return e;
            }
            e.face = titleEnvironmentFace(env, planner.pxScale(), plan->tier);
            if (e.missing) e.face = std::min(e.face, 1024);
            e.key = hashCombine(hashCombine(fnv1a64(scene3d->id + (e.hdri ? ":" + scene3d->hdri : std::string())), (uint64_t)e.face),
                                TITLE_ENV_BUILDER_VERSION);
            return e;
        };
        plan->environments.push_back(environmentOf(plan->lighting.environment, "scene_3d.environment"));

        // the materials and the slot table
        const three_d::TitleSlotMaterials materials = three_d::resolveSlotMaterials(layer, plan->style, mesh);
        plan->materials = materials.materials;
        std::vector<uint8_t> materialEnvironment(plan->materials.size(), 0);
        for (size_t i = 0; i < plan->materials.size(); i++) {
            const TitleMaterial3D& m = plan->materials[i];
            if (!m.environment.is_object() || m.environment.empty()) continue;
            const Lighting3D partLighting = resolveLighting(scene, plan->style, &m);
            Title3DEnvPlan e = environmentOf(partLighting.environment, "style_3d.environment");
            size_t index = plan->environments.size();
            for (size_t j = 0; j < plan->environments.size(); j++) {
                const Environment3D& a = plan->environments[j].environment;
                const Environment3D& b = e.environment;
                if (plan->environments[j].key == e.key && a.intensity == b.intensity && a.rotation == b.rotation && a.tint.r == b.tint.r &&
                    a.tint.g == b.tint.g && a.tint.b == b.tint.b && a.contrast == b.contrast && a.saturation == b.saturation && a.blur == b.blur &&
                    a.repeatX == b.repeatX && a.repeatY == b.repeatY && a.matcap.path == b.matcap.path && a.matcap.guid == b.matcap.guid) {
                    index = j;
                    break;
                }
            }
            if (index == plan->environments.size()) {
                if (plan->environments.size() >= 255) continue;
                plan->environments.push_back(std::move(e));
            }
            materialEnvironment[i] = (uint8_t)index;
        }
        const bool lights = countNonAmbientLights(plan->lighting.lights) > 0;
        const Json& chars = memberOf(layer, "chars");
        // R1a: an Image or artwork Vector layer whose face fill is `source` projects its picture
        const LayerType type = layerTypeOf(layer);
        const bool ownPixels = type == LayerType::Image || (type == LayerType::Vector && layer.value("mode", std::string()) == "artwork");
        const Json& faceFill = memberOf(memberOf(memberOf(layer, "style"), "face"), "fill");
        plan->faceImage = std::clamp(plan->style.faceImage, 0.0, 1.0);
        plan->edgeImage = std::clamp(plan->style.edgeImage, 0.0, 1.0);
        const bool picture = ownPixels && svc.content && req.doc && faceFill.value("kind", std::string("source")) == "source" &&
                             (plan->faceImage > 0 || plan->edgeImage > 0);
        if (picture) {
            const Json layerCopy = layer;
            const std::shared_ptr<const TitleArtDoc> docHeld = req.doc;
            const std::shared_ptr<TitleContentProvider> content = svc.content;
            const AssetResolver* assets = req.assets;
            const double pxPerUnit = std::max(1.0, planner.pxScale() * 2.0);
            jobs::SubmitOptions options;
            options.lane = jobs::Lane::Background;
            options.name = "title-3d-picture";
            plan->picture = std::make_shared<jobs::Task<std::shared_ptr<TitlePicture3D>>>(jobs::submit(
                [layerCopy, docHeld, content, assets, pxPerUnit]() { return makeTitlePicture3D(layerCopy, *docHeld, *content, assets, pxPerUnit); },
                options));
        }
        plan->slots.resize(mesh.slots.size());
        for (size_t s = 0; s < mesh.slots.size(); s++) {
            const uint8_t materialIndex = materials.slotMaterial[s];
            plan->slots[s].material = materialIndex;
            plan->slots[s].environment = materialIndex < materialEnvironment.size() ? materialEnvironment[materialIndex] : 0;
            const TitleTierFeatures features = titleTierFeatures(plan->materials[materialIndex], plan->style, plan->tier);
            uint32_t bits = features.litBits;
            if (lights) bits |= TitleLitBits::LIGHTS;
            if (!mesh.rigs.empty()) bits |= TitleLitBits::RIGS;
            if (plan->environments[plan->slots[s].environment].environment.preset == "matcap") bits |= TitleLitBits::MATCAP;
            if (bits & TitleLitBits::WRAP_UV && mesh.wrap.empty()) bits &= ~TitleLitBits::WRAP_UV;
            bits |= debugOutput(plan->debugView) << TitleLitBits::OUTPUT_SHIFT;
            const TitleSlotRole role = mesh.slots[s].role;
            if (picture && ((role == TitleSlotRole::Face && plan->faceImage > 0) ||
                            ((role == TitleSlotRole::Bevel || role == TitleSlotRole::Side) && plan->edgeImage > 0))) {
                bits |= TitleLitBits::FACE_IMAGE;
            }
            plan->slots[s].bits = bits;
            plan->behind = plan->behind || features.behind;
            plan->ao = features.ao;
            plan->glint = features.glint;
        }
        plan->aoParams = plan->style.ao;
        plan->glintParams = plan->style.glint;
        // base textures per material
        plan->baseTextures.resize(plan->materials.size());
        for (size_t i = 0; i < plan->materials.size(); i++) {
            if (plan->materials[i].baseTexture.empty()) continue;
            plan->baseTextures[i] = planner.title3DAsset(encodeAssetRef(plan->materials[i].baseTexture), node.layerId, "style_3d.face.base_texture");
        }
        // custom patterns' images per material
        plan->patternTextures.resize(plan->materials.size());
        for (size_t i = 0; i < plan->materials.size(); i++) {
            const TitleMaterial3D& m = plan->materials[i];
            if (!m.hasPattern() || m.pattern.value("kind", std::string()) != "custom") continue;
            plan->patternTextures[i] = planner.title3DAsset(m.pattern["image"], node.layerId, "style_3d.face.pattern.image");
        }

        // per-part rigs: the rigged character's matrix, identity elsewhere
        plan->partRigs.resize(mesh.parts.size());
        for (size_t p = 0; p < mesh.parts.size(); p++) {
            const uint16_t r = mesh.parts[p].rig;
            if (!r || r > mesh.rigs.size()) continue;
            const TitleMeshRig& rig = mesh.rigs[r - 1];
            const Json& adjust = memberOf(chars, std::to_string(rig.charIndex).c_str());
            plan->partRigs[p] = title3DRigMatrix(decodeCharRig(rig.charIndex, adjust), {rig.pivot[0], rig.pivot[1], rig.pivot[2]}, plan->size);
        }

        plan->valid = !mesh.vertices.empty();
        return plan;
    }

}

std::shared_ptr<TitleCacheEntry> TitlePlanner::title3DAsset(const Json& ref, const std::string& layerId, const std::string& path) {
    return assetTexture(ref, TitleAssetKind::Image, TextureWrap::REPEAT, 0, layerId, path);
}

TitleRectI TitlePlanner::title3DRect(const DrawNode& node) {
    std::shared_ptr<void>& slot = title3DPlans[node.layerId];
    if (!slot) {
        auto parentIt = parents.find(node.layerId);
        const geometry::Affine parent = parentIt != parents.end() && parentIt->second ? placementOf(*parentIt->second).toDocument : geometry::Affine();
        slot = makeTitle3DPlan(*this, node, parent, svc, req);
    }
    const Title3DPlan& plan = *static_cast<Title3DPlan*>(slot.get());
    if (!plan.valid) return {};
    const TitleMesh& mesh = *plan.mesh;
    const TitleCamera3D camera = title3DCamera(plan.camera, plan.canvasWidth, plan.canvasHeight, scale, outRegion, false);
    // the mesh box with the skew's reach and every rig's swing (a rigged letter may turn anywhere
    // within its bounding sphere)
    std::array<float, 3> lo = mesh.boundsMin, hi = mesh.boundsMax;
    const float depth = std::max(0.0f, -lo[2]);
    const float sx = (float)plan.style.skew.x * depth, sy = (float)plan.style.skew.y * depth;
    lo[0] += std::min(0.0f, sx);
    hi[0] += std::max(0.0f, sx);
    lo[1] += std::min(0.0f, sy);
    hi[1] += std::max(0.0f, sy);
    if (!mesh.rigs.empty()) {
        const float r = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
        const float u = (float)(plan.size / 100.0);
        float reach = 0;
        for (const TitleMeshRig& rig : mesh.rigs) {
            const CharRig3D a = decodeCharRig(rig.charIndex, memberOf(memberOf(*node.layer, "chars"), std::to_string(rig.charIndex).c_str()));
            reach = std::max(reach, (float)(std::abs(a.dz) + std::hypot(a.offset.x, a.offset.y)) * u);
        }
        for (int c = 0; c < 3; c++) {
            lo[c] -= reach + r * 0.25f;
            hi[c] += reach + r * 0.25f;
        }
    }
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    bool any = false;
    for (int i = 0; i < 8; i++) {
        const std::array<float, 3> corner = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
        const std::array<float, 4> w = plan.model.apply({corner[0], corner[1], corner[2], 1});
        std::array<double, 2> px;
        if (!title3DProject(camera, {w[0], w[1], w[2]}, plan.canvasWidth, plan.canvasHeight, scale, px)) return outRegion;
        minX = std::min(minX, px[0]);
        minY = std::min(minY, px[1]);
        maxX = std::max(maxX, px[0]);
        maxY = std::max(maxY, px[1]);
        any = true;
    }
    if (!any) return {};
    // two output px of antialiasing margin
    const int x0 = (int)std::floor(minX) - 2, y0 = (int)std::floor(minY) - 2;
    return intersectRect({x0, y0, (int)std::ceil(maxX) + 2 - x0, (int)std::ceil(maxY) + 2 - y0}, outRegion);
}

TitlePx TitlePlanner::title3D(const DrawNode& node, const std::vector<std::string>& parts, const TitleRectI* fixedRect) {
    const TitleRectI rect = fixedRect ? *fixedRect : nodeRect(node);
    title3DRect(node);
    std::shared_ptr<void> held = title3DPlans[node.layerId];
    auto plan = std::static_pointer_cast<Title3DPlan>(held);
    if (!plan || !plan->valid || rect.empty()) return {};
    // k: the tier's (Bake: the document's supersample), else 2, else 1, within the render px cap
    // and the device's texture size
    const TitleTierLimits limits = titleTierLimits(plan->tier);
    const int maxSide = std::max(1024, SystemRender::getFeatures().maxTextureSize2D);
    const uint64_t regionPx = (uint64_t)rect.width * (uint64_t)rect.height;
    const int first = plan->tier == TitleTier::Bake ? std::clamp(plan->supersample, 1, 3) : limits.k;
    plan->k = 1;
    for (int k : {first, 2, 1}) {
        if (k > first) continue;
        if (regionPx * (uint64_t)(k * k) <= limits.renderPxCap && rect.width * k <= maxSide && rect.height * k <= maxSide) {
            plan->k = k;
            break;
        }
    }
    // the perspective depends on where the layer sits: its cached pixels are keyed by position
    if (canvasReads) *canvasReads = true;
    TitlePx out{newSlot(TitleBuffer::Accumulator, rect.width, rect.height), rect};
    const double pxScale = scale;
    const uint32_t testFlags = req.testFlags;
    const int slot = out.slot;
    const std::vector<std::string> coloured = parts;
    // one GPU step that waits while a program of the plan compiles (the previous image stays)
    TitleRenderStep step;
    step.kind = StepKind::Gpu;
    step.label = "3d " + node.layerId;
    step.layerId = node.layerId;
    step.scissor = {0, 0, rect.width, rect.height};
    step.passes = Title3DRenderer::passCount(*plan);
    step.costWeight = 40.0f * (float)(plan->k * plan->k) * (float)step.passes;
    step.splittable = false;
    TitleRenderServices* services = &svc;
    step.ready = [services, plan, testFlags]() { return title3DRenderer(*services).ready(*plan, testFlags); };
    std::shared_ptr<TitleJobState> state = st;
    step.run = [state, plan, rect, pxScale, testFlags, slot, coloured](const TitleRectI&, std::string& error) {
        if (!state->alive(error)) return false;
        TitleTarget* target = state->target(slot, error);
        if (!target) return false;
        return title3DRenderer(state->services).render(*plan, rect, pxScale, coloured, state->services, target, testFlags, error);
    };
    steps.add(std::move(step));
    return out;
}
