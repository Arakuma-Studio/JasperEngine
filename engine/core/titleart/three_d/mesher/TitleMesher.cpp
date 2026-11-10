// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleMesher.h"

#include "RingExtruder.h"
#include "TitleMeshBuilder.h"
#include "titleart/TitleArtHash.h"
#include "titleart/three_d/TitleTierFeatures.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    uint64_t mix(uint64_t h, double v) {
        if (v == 0) v = 0;  // -0
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        return hashCombine(h, bits);
    }
    uint64_t mix(uint64_t h, uint64_t v) { return hashCombine(h, v); }

    uint64_t hashPath(uint64_t h, const geometry::Path& path) {
        const auto& verbs = path.verbList();
        const auto& pts = path.pointList();
        h = mix(h, (uint64_t)verbs.size());
        if (!verbs.empty()) h = fnv1a64(std::string_view(reinterpret_cast<const char*>(verbs.data()), verbs.size() * sizeof(verbs[0])), h);
        if (!pts.empty()) h = fnv1a64(std::string_view(reinterpret_cast<const char*>(pts.data()), pts.size() * sizeof(pts[0])), h);
        return mix(h, (uint64_t)path.evenOdd);
    }

    uint64_t hashBevel(uint64_t h, const Bevel3D& b, int segments) {
        h = mix(h, b.width);
        h = mix(h, b.depth);
        h = mix(h, (uint64_t)b.shape.profile);
        h = mix(h, (uint64_t)segments);
        h = mix(h, b.shape.curve);
        if (b.shape.profile == BevelProfileKind::Custom) {
            for (const TitleCurvePoint& p : b.shape.custom.points) {
                h = mix(h, p.x);
                h = mix(h, p.y);
                h = mix(h, (uint64_t)p.corner);
            }
        }
        return h;
    }

    // the tier budget's coarsening aims this far under the limit (the grid's crossings and the
    // welded count do not scale exactly) and stops at these floors
    constexpr double BUDGET_AIM = 0.95;
    constexpr double MAX_RING_STEP_SCALE = 8.0;
    constexpr int MIN_BUDGET_BEVEL_SEGMENTS = 2;
    constexpr int MIN_BUDGET_CURVE_SEGMENTS = 3;

    int bevelSegmentsAt(const Bevel3D& b, const TitleMeshRequest& r) {
        return std::min(titleTierGeometry(r.tier).maxBevelSegments, effectiveSegments(b.shape.segments, r.outputScale));
    }

}

uint64_t jasperengine::titleart::three_d::titleShapeKey(const TitleSource3D& s, int curveSegments) {
    uint64_t h = fnv1a64("title3d-shapes");
    h = mix(h, (uint64_t)s.text);
    h = mix(h, s.size);
    h = mix(h, s.shear);
    h = mix(h, (uint64_t)s.orb);
    h = mix(h, (uint64_t)curveSegments);
    for (const TitleGlyph3D& g : s.glyphs) {
        h = hashPath(h, g.path);
        h = mix(h, (uint64_t)g.charIndex);
        h = mix(h, (uint64_t)g.rigged);
        h = mix(h, g.pivot.x);
        h = mix(h, g.pivot.y);
        for (const TitleRect* r : {&g.lineBox, &g.charBox}) {
            h = mix(h, r->x);
            h = mix(h, r->y);
            h = mix(h, r->width);
            h = mix(h, r->height);
        }
    }
    return h;
}

uint64_t jasperengine::titleart::three_d::titleGeometryKey(const TitleMeshRequest& r) {
    const Title3D& p = r.style;
    uint64_t h = titleShapeKey(r.source, titleTierCurveSegments(p.curveSegments, r.tier, r.draft));
    h = mix(h, (uint64_t)TITLE_3D_VERSION);
    h = mix(h, (uint64_t)p.mode);
    h = mix(h, p.depth);
    h = hashBevel(h, p.bevel, bevelSegmentsAt(p.bevel, r));
    h = mix(h, (uint64_t)p.bevelPosition);
    if (p.mode == Mode3D::Inflate) {
        h = mix(h, p.inflate.height);
        h = mix(h, p.inflate.smoothness);
        h = mix(h, (uint64_t)p.inflate.detail);
        for (const TitleCurvePoint& q : p.inflate.profile.points) {
            h = mix(h, q.x);
            h = mix(h, q.y);
            h = mix(h, (uint64_t)q.corner);
        }
    }
    h = mix(h, (uint64_t)p.bands.size());
    for (const Band3D& b : p.bands) {
        h = mix(h, (uint64_t)b.enabled);
        h = mix(h, b.grow);
        h = mix(h, (uint64_t)b.roundJoin);
        h = mix(h, (uint64_t)b.fillCounters);
        h = mix(h, (uint64_t)b.plate.kind);
        h = mix(h, b.plate.close);
        h = mix(h, b.plate.corner);
        h = mix(h, b.plate.slant);
        h = mix(h, b.depth);
        h = mix(h, b.offset);
        h = hashBevel(h, b.bevel, bevelSegmentsAt(b.bevel, r));
    }
    for (const auto& [ch, block] : r.blocks) {
        h = mix(h, (uint64_t)ch);
        h = mix(h, (uint64_t)block.slot);
        for (bool b : block.bands) h = mix(h, (uint64_t)b);
    }
    h = mix(h, (uint64_t)r.tier);
    h = mix(h, inflateScale(r.outputScale));
    h = mix(h, (uint64_t)r.draft);
    h = mix(h, (uint64_t)r.wrap);
    h = mix(h, (uint64_t)r.testFlags);
    if (r.source.orb) {
        // the orb's tessellation follows its radius on screen
        h = mix(h, r.outputScale);
    }
    return h;
}

TitleMesher::TitleMesher(size_t meshCapacity, size_t shapeCapacity) : meshCapacity(meshCapacity), shapeCapacity(shapeCapacity) {}

std::shared_ptr<const TitleMesh> TitleMesher::cached(uint64_t key) {
    std::lock_guard<std::mutex> guard(lock);
    for (auto it = meshes.begin(); it != meshes.end(); ++it) {
        if (it->first == key) {
            meshes.splice(meshes.begin(), meshes, it);
            return meshes.front().second;
        }
    }
    return nullptr;
}

std::shared_ptr<const TitleShapes3D> TitleMesher::shapesFor(const TitleSource3D& source, int curveSegments) {
    const uint64_t shapeKey = titleShapeKey(source, curveSegments);
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto it = shapes.begin(); it != shapes.end(); ++it) {
            if (it->first == shapeKey) {
                shapes.splice(shapes.begin(), shapes, it);
                return shapes.front().second;
            }
        }
    }
    auto built = std::make_shared<const TitleShapes3D>(buildTitleShapes(source, curveSegments));
    shapeCount++;
    std::lock_guard<std::mutex> guard(lock);
    shapes.emplace_front(shapeKey, built);
    while (shapes.size() > shapeCapacity) shapes.pop_back();
    return built;
}

std::shared_ptr<const TitleMesh> TitleMesher::build(const TitleMeshRequest& r) {
    const uint64_t key = titleGeometryKey(r);
    if (auto hit = cached(key)) return hit;
    const auto start = std::chrono::steady_clock::now();
    int curveSegments = titleTierCurveSegments(r.style.curveSegments, r.tier, r.draft);
    std::shared_ptr<const TitleShapes3D> shapes = shapesFor(r.source, curveSegments);
    const TitleTierGeometry tierGeometry = titleTierGeometry(r.tier);
    TitleSolidsOptions options;
    options.outputScale = r.outputScale;
    options.draft = r.draft;
    options.maxBevelSegments = tierGeometry.maxBevelSegments;
    options.inflateMaxNodes = tierGeometry.inflateNodeCap;
    options.inflateStepScale = tierGeometry.inflateStepScale;
    options.testFlags = r.testFlags;
    // the tier's solid budget is met here, on every device (the authored style keeps every band,
    // Bake and higher tiers still draw them): past it the bands that add least to the outline go,
    // the thinnest grow first (the inner one of equals), each handing its grow to the next band out
    // so the silhouette and every kept rim keep their place
    Title3D tierStyle = r.style;
    const TitleTierLimits limits = titleTierLimits(r.tier);
    std::vector<size_t> kept;
    for (size_t i = 0; i < tierStyle.bands.size(); i++) if (tierStyle.bands[i].enabled) kept.push_back(i);
    while (limits.maxSolids && kept.size() + 1 > limits.maxSolids) {
        size_t thinnest = 0;
        for (size_t k = 1; k < kept.size(); k++) {
            if (tierStyle.bands[kept[k]].grow < tierStyle.bands[kept[thinnest]].grow) thinnest = k;
        }
        Band3D& dropped = tierStyle.bands[kept[thinnest]];
        dropped.enabled = false;
        if (thinnest + 1 < kept.size()) tierStyle.bands[kept[thinnest + 1]].grow += dropped.grow;
        kept.erase(kept.begin() + (std::ptrdiff_t)thinnest);
    }
    // the most bevel segments any bevel of the style takes at the tier
    auto bevelSegments = [&]() {
        int n = 0;
        auto add = [&](const Bevel3D& b) {
            if (b.width > 0) n = std::max(n, std::min(options.maxBevelSegments, effectiveSegments(b.shape.segments, r.outputScale)));
        };
        if (tierStyle.mode == Mode3D::Extrude) add(tierStyle.bevel);
        for (const Band3D& band : tierStyle.bands) if (band.enabled) add(band.bevel);
        return n;
    };
    // §6.7's triangle and mesh-memory budgets are met by coarsening, never by simplifying (R13), so
    // the silhouette, the stack and the facets stay: first the inflate grid (its front is clipped to
    // the exact outline at any step), then the outward rings' refinement, then the bevels' segments
    // (down to two, still a curve), last the curve segments (down to three)
    enum class Coarsen { InflateGrid, Rings, Bevels, Curves, Spent };
    Coarsen stage = tierStyle.mode == Mode3D::Inflate ? Coarsen::InflateGrid : Coarsen::Rings;
    const bool budgeted = (limits.maxTriangles || limits.meshBytes) && (r.testFlags & MESHER_TEST_NO_TIER_BUDGET) == 0;
    std::shared_ptr<TitleMesh> mesh;
    size_t previous = SIZE_MAX;
    for (;;) {
        const TitleSolids solids = buildTitleSolids(*shapes, tierStyle, r.blocks, options, &inflate);
        const size_t triangles = solids.mesh.triangleCount();
        // the soup's count bounds the welded one (the builder only drops collapsed triangles)
        const bool overTriangles = budgeted && limits.maxTriangles && triangles > limits.maxTriangles;
        if (!overTriangles) {
            mesh = buildTitleMeshFromSolids(solids, r.wrap, r.source.size);
            if (!budgeted || ((!limits.maxTriangles || mesh->stats.triangles <= limits.maxTriangles) && (!limits.meshBytes || mesh->bytes() <= limits.meshBytes))) {
                break;
            }
        }
        // what to aim for: the triangle limit, or the count whose bytes fit
        double target = limits.maxTriangles ? (double)limits.maxTriangles : (double)triangles;
        if (mesh && limits.meshBytes && mesh->bytes() > limits.meshBytes) target = std::min(target, (double)triangles * limits.meshBytes / mesh->bytes());
        target *= BUDGET_AIM;
        mesh.reset();
        // a step that took off less than 1 % spends its stage
        if (previous != SIZE_MAX && (double)triangles > 0.99 * (double)previous) stage = (Coarsen)((int)stage + 1);
        previous = triangles;
        if (stage == Coarsen::InflateGrid) {
            // the grid's triangles go with 1 / step^2: scale the step for the share over the target
            size_t front = 0;
            for (size_t i = 0; i < solids.slots.size() && i < solids.mesh.slots.size(); i++) {
                if (solids.slots[i].role == TitleSlotRole::Face) front += solids.mesh.slots[i].size() / 3;
            }
            const double rest = (double)(triangles - front);
            const double want = std::max(target - rest, 0.25 * (double)front);
            options.inflateBudgetScale *= std::clamp(std::sqrt((double)front / std::max(want, 1.0)), 1.1, 3.0);
            continue;
        }
        if (stage == Coarsen::Rings) {
            if (options.ringStepScale < MAX_RING_STEP_SCALE && tierStyle.mode == Mode3D::Extrude && tierStyle.bevel.width > 0 &&
                tierStyle.bevelPosition != BevelPosition3D::Inside) {
                options.ringStepScale *= 2;
                continue;
            }
            stage = Coarsen::Bevels;
        }
        if (stage == Coarsen::Bevels) {
            const int n = bevelSegments();
            if (n > MIN_BUDGET_BEVEL_SEGMENTS) {
                options.maxBevelSegments = n - 1;
                continue;
            }
            stage = Coarsen::Curves;
        }
        if (stage == Coarsen::Curves && curveSegments > MIN_BUDGET_CURVE_SEGMENTS) {
            curveSegments = std::max(MIN_BUDGET_CURVE_SEGMENTS, curveSegments * 3 / 4);
            shapes = shapesFor(r.source, curveSegments);
            continue;
        }
        // nothing left to coarsen: the last build is the mesh (over-budget titles still render)
        mesh = buildTitleMeshFromSolids(solids, r.wrap, r.source.size);
        break;
    }
    mesh->tier = r.tier;
    mesh->sourceHash = key;
    mesh->stats.buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    meshCount++;
    std::shared_ptr<const TitleMesh> out = mesh;
    std::lock_guard<std::mutex> guard(lock);
    meshes.emplace_front(key, out);
    while (meshes.size() > meshCapacity) meshes.pop_back();
    return out;
}

void TitleMesher::clear() {
    std::lock_guard<std::mutex> guard(lock);
    meshes.clear();
    shapes.clear();
    inflate.clear();
}
