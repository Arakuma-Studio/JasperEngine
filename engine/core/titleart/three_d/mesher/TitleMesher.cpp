// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleMesher.h"

#include "RingExtruder.h"
#include "TitleMeshBuilder.h"
#include "titleart/TitleArtHash.h"
#include "titleart/three_d/TitleTierFeatures.h"

#include <chrono>
#include <cstring>

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

std::shared_ptr<const TitleMesh> TitleMesher::build(const TitleMeshRequest& r) {
    const uint64_t key = titleGeometryKey(r);
    if (auto hit = cached(key)) return hit;
    const auto start = std::chrono::steady_clock::now();
    const int curveSegments = titleTierCurveSegments(r.style.curveSegments, r.tier, r.draft);
    const uint64_t shapeKey = titleShapeKey(r.source, curveSegments);
    std::shared_ptr<const TitleShapes3D> shapes;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto it = this->shapes.begin(); it != this->shapes.end(); ++it) {
            if (it->first == shapeKey) {
                this->shapes.splice(this->shapes.begin(), this->shapes, it);
                shapes = this->shapes.front().second;
                break;
            }
        }
    }
    if (!shapes) {
        shapes = std::make_shared<const TitleShapes3D>(buildTitleShapes(r.source, curveSegments));
        shapeCount++;
        std::lock_guard<std::mutex> guard(lock);
        this->shapes.emplace_front(shapeKey, shapes);
        while (this->shapes.size() > shapeCapacity) this->shapes.pop_back();
    }
    const TitleTierGeometry tierGeometry = titleTierGeometry(r.tier);
    TitleSolidsOptions options;
    options.outputScale = r.outputScale;
    options.draft = r.draft;
    options.maxBevelSegments = tierGeometry.maxBevelSegments;
    options.inflateMaxNodes = tierGeometry.inflateNodeCap;
    options.inflateStepScale = tierGeometry.inflateStepScale;
    options.testFlags = r.testFlags;
    const TitleSolids solids = buildTitleSolids(*shapes, r.style, r.blocks, options, &inflate);
    std::shared_ptr<TitleMesh> mesh = buildTitleMeshFromSolids(solids, r.wrap, r.source.size);
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
