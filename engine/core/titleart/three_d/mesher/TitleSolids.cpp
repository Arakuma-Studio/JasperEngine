// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/title.ts by SafEight, used with the author's permission.

#include "TitleSolids.h"

#include "RingExtruder.h"
#include "titleart/geometry/Plate.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    constexpr uint32_t FACE_SLOT = 0, BEVEL_SLOT = 1, SIDE_SLOT = 2;
    uint32_t bandSlot(size_t i) { return 3 + 2 * (uint32_t)i; }

    // z lift of a rigged glyph's band stack and the seat of a band slid forward (fractions of size)
    constexpr double RIG_BAND_LIFT = 0.0025;
    constexpr double BAND_SEAT = 0.0025;

    double outsideShare(BevelPosition3D p) {
        return p == BevelPosition3D::Outside ? 1.0 : p == BevelPosition3D::Center ? 0.5 : 0.0;
    }

    SpanBox boxOf(const TitleRect& r) { return {r.x, r.y, r.x + r.width, r.y + r.height}; }

    // the union entry of span entries `indices` (appended), the single one as is, none = the layer's
    uint16_t unionSpan(std::vector<SpanBox>& line, std::vector<SpanBox>& chr, const std::vector<uint16_t>& indices) {
        if (indices.size() <= 1) return indices.empty() ? 0 : indices[0];
        auto join = [&](const std::vector<SpanBox>& boxes) {
            SpanBox b{INFINITY, INFINITY, -INFINITY, -INFINITY};
            for (uint16_t k : indices) {
                b.x0 = std::min(b.x0, boxes[k].x0);
                b.y0 = std::min(b.y0, boxes[k].y0);
                b.x1 = std::max(b.x1, boxes[k].x1);
                b.y1 = std::max(b.y1, boxes[k].y1);
            }
            return b;
        };
        const SpanBox l = join(line), c = join(chr);
        line.push_back(l);
        chr.push_back(c);
        return (uint16_t)(line.size() - 1);
    }

    std::vector<uint16_t> marksIn(const std::vector<GlyphMark>& marks, const ShapeContours& shape) {
        std::set<uint16_t> out;
        for (const GlyphMark& m : marks) {
            if (shapeContains(shape, m.p.x, m.p.y)) out.insert(m.span);
        }
        return {out.begin(), out.end()};
    }

    // a bevel profile cut where it crosses the outline (offset 0): the inward part for the ring
    // stack, the outward part for the outward rings; offsets within 1e-9 snap onto the outline
    void splitAtOutline(const std::vector<ProfileSpan>& spans, std::vector<ProfileSpan>& inner, std::vector<ProfileSpan>& outer) {
        auto snap = [](ProfilePoint q) {
            if (q.off != 0 && std::abs(q.off) < 1e-9) q.off = 0;
            return q;
        };
        for (const ProfileSpan& s : spans) {
            const ProfilePoint a = snap(s.a), b = snap(s.b);
            if (b.off <= 0) {
                inner.push_back({s.slot, a, b});
            } else if (a.off >= 0) {
                outer.push_back({s.slot, a, b});
            } else {
                const double k = -a.off / (b.off - a.off);
                ProfilePoint m;
                m.off = 0;
                m.z = a.z + (b.z - a.z) * k;
                if (a.hasNormal && b.hasNormal) {
                    const double nx = a.nr + (b.nr - a.nr) * k, nz = a.nz + (b.nz - a.nz) * k;
                    double l = std::hypot(nx, nz);
                    if (l == 0) l = 1;
                    m.hasNormal = true;
                    m.nr = nx / l;
                    m.nz = nz / l;
                }
                inner.push_back({s.slot, a, m});
                outer.push_back({s.slot, m, b});
            }
        }
    }

    std::vector<ProfileSpan> slotted(const std::vector<ProfileSpan>& list, uint32_t base) {
        if (!base) return list;
        std::vector<ProfileSpan> out = list;
        for (ProfileSpan& s : out) s.slot += (int)base;
        return out;
    }

    // the part entry of a solid (its z range, rig and shear), shared by equal solids
    class PartTable {
    public:
        std::vector<TitleMeshPart> parts;
        uint16_t of(double zFront, double zBack, int rig, uint16_t flags) {
            for (size_t i = 0; i < parts.size(); i++) {
                const TitleMeshPart& p = parts[i];
                if (p.zFront == (float)zFront && p.zBack == (float)zBack && p.rig == (uint16_t)(rig + 1) && p.flags == flags) return (uint16_t)i;
            }
            parts.push_back({(float)zFront, (float)zBack, (uint16_t)(rig + 1), flags});
            return (uint16_t)(parts.size() - 1);
        }
    };

    Shapes flattenNormalized(const geometry::Path& path, int curveSegments, double eps) {
        return normalizeContours(flattenContours(path, curveSegments), path.evenOdd, eps);
    }

}

TitleShapes3D jasperengine::titleart::three_d::buildTitleShapes(const TitleSource3D& source, int curveSegments) {
    TitleShapes3D t;
    t.size = source.size;
    t.eps = outlineEps(source.size);
    t.shear = source.shear;
    t.curveSegments = curveSegments;
    const double eps = t.eps;
    if (!source.text) {
        // shapes, SVGs, traces: every fill group normalised under its own rule, then unioned
        Shapes all;
        for (const TitleGlyph3D& g : source.glyphs) {
            Shapes s = flattenNormalized(g.path, curveSegments, eps);
            all.insert(all.end(), s.begin(), s.end());
            t.allPath.append(g.path);
        }
        t.outline = source.glyphs.size() > 1 ? unionShapes(all, eps) : all;
        t.outlinePath = t.allPath;
        const TitleRect b = shapesBounds(t.outline);
        t.line = {boxOf(b)};
        t.chr = {boxOf(b)};
        if (source.orb && !t.outline.empty()) {
            const TitleRect pb = t.allPath.bounds();
            t.orb = std::array<double, 4>{pb.width / 2, pb.height / 2, pb.x + pb.width / 2, pb.y + pb.height / 2};
        } else {
            TitlePart3D part;
            part.shapes = t.outline;
            part.owners.assign(t.outline.size(), -1);
            part.span = 0;
            t.parts.push_back(std::move(part));
        }
        return t;
    }

    // text: entry 0 the layer's ink bounds, 1 + g glyph g's boxes
    SpanBox layer{INFINITY, INFINITY, -INFINITY, -INFINITY};
    for (const TitleGlyph3D& g : source.glyphs) {
        const SpanBox c = boxOf(g.charBox);
        layer = {std::min(layer.x0, c.x0), std::min(layer.y0, c.y0), std::max(layer.x1, c.x1), std::max(layer.y1, c.y1)};
    }
    if (source.glyphs.empty()) layer = {};
    t.line.push_back(layer);
    t.chr.push_back(layer);
    for (const TitleGlyph3D& g : source.glyphs) {
        t.line.push_back(boxOf(g.lineBox));
        t.chr.push_back(boxOf(g.charBox));
    }
    std::vector<std::vector<Contour>> groups;
    std::vector<Shapes> pieces;
    groups.reserve(source.glyphs.size());
    for (const TitleGlyph3D& g : source.glyphs) {
        groups.push_back(flattenContours(g.path, curveSegments));
        pieces.push_back(normalizeContours(groups.back(), false, eps));
        t.allPath.append(g.path);
    }
    // unrigged glyphs: their union (also the shared bands' outline); each island spans the glyphs it is made of
    std::vector<size_t> flatIdx;
    for (size_t g = 0; g < source.glyphs.size(); g++) {
        if (!source.glyphs[g].rigged) flatIdx.push_back(g);
    }
    std::vector<Contour> flatContours;
    std::vector<std::vector<Contour>> flatGroups;
    for (size_t g : flatIdx) {
        flatContours.insert(flatContours.end(), groups[g].begin(), groups[g].end());
        flatGroups.push_back(groups[g]);
        t.outlinePath.append(source.glyphs[g].path);
        for (const ShapeContours& s : pieces[g]) t.marks.push_back({interiorPoint(s), (uint16_t)(1 + g)});
    }
    t.outline = normalizeContours(flatContours, false, eps);
    const std::vector<int> owners = shapeOwners(t.outline, flatGroups);
    for (size_t s = 0; s < t.outline.size(); s++) {
        TitlePart3D part;
        part.shapes = {t.outline[s]};
        part.owners = {owners[s] < 0 ? -1 : (int64_t)source.glyphs[flatIdx[(size_t)owners[s]]].charIndex};
        part.span = unionSpan(t.line, t.chr, marksIn(t.marks, t.outline[s]));
        t.parts.push_back(std::move(part));
    }
    // rigged glyphs: one part each, pivoting on the glyph's em-cell centre
    for (size_t g = 0; g < source.glyphs.size(); g++) {
        if (!source.glyphs[g].rigged || pieces[g].empty() || t.rigs.size() >= TITLE_3D_MAX_RIGS) continue;
        TitleMeshRig rig;
        rig.charIndex = source.glyphs[g].charIndex;
        rig.pivot[0] = (float)source.glyphs[g].pivot.x;
        rig.pivot[1] = (float)source.glyphs[g].pivot.y;
        TitlePart3D part;
        part.shapes = pieces[g];
        part.owners.assign(pieces[g].size(), (int64_t)source.glyphs[g].charIndex);
        part.span = (uint16_t)(1 + g);
        part.rig = (int)t.rigs.size();
        part.outline = source.glyphs[g].path;
        t.rigs.push_back(rig);
        t.parts.push_back(std::move(part));
    }
    return t;
}

uint32_t jasperengine::titleart::three_d::titleBlockSize(size_t bandCount) {
    return bandSlot(bandCount);
}

std::vector<TitleMeshSlot> jasperengine::titleart::three_d::titleSlots(size_t bandCount, const std::map<uint32_t, CharBlock3D>& blocks) {
    const uint32_t size = titleBlockSize(bandCount);
    std::set<uint16_t> bases = {0};
    for (const auto& [ch, block] : blocks) bases.insert(block.slot);
    std::vector<TitleMeshSlot> out;
    for (uint16_t base : bases) {
        if (out.size() < (size_t)base + size) out.resize((size_t)base + size);
        for (uint32_t k = 0; k < size; k++) {
            TitleMeshSlot& s = out[base + k];
            s.block = base;
            if (k == FACE_SLOT) {
                s.role = TitleSlotRole::Face;
            } else if (k == BEVEL_SLOT) {
                s.role = TitleSlotRole::Bevel;
            } else if (k == SIDE_SLOT) {
                s.role = TitleSlotRole::Side;
            } else {
                s.band = (int16_t)((k - 3) / 2);
                s.role = (k - 3) % 2 == 0 ? TitleSlotRole::BandFront : TitleSlotRole::BandSide;
            }
        }
    }
    return out;
}

TitleSolids jasperengine::titleart::three_d::buildTitleSolids(const TitleShapes3D& t, const Title3D& p, const std::map<uint32_t, CharBlock3D>& chars,
                                                               const TitleSolidsOptions& options, InflateFieldCache* fields) {
    TitleSolids out;
    out.slots = titleSlots(p.bands.size(), chars);
    out.rigs = t.rigs;
    RawMesh& mesh = out.mesh;
    PartTable parts;
    const double size = t.size;
    const double eps = t.eps;
    auto px = [&](double v) { return std::max(0.0, v) / 100 * size; };
    auto segmentsOf = [&](const three_d::BevelShape& shape) {
        return std::min(options.maxBevelSegments, effectiveSegments(shape.segments, options.outputScale));
    };
    // bands append union entries: work on a copy so rebuilds keep the cached table
    std::vector<SpanBox> line = t.line, chr = t.chr;
    const bool inflate = p.mode == Mode3D::Inflate;
    const double bw = t.orb || inflate ? 0 : px(p.bevel.width);
    const double outW = bw * outsideShare(p.bevelPosition);
    auto grown = [&](const SpanBox& b) { return SpanBox{b.x0 - outW, b.y0 - outW, b.x1 + outW, b.y1 + outW}; };
    if (outW > 0 && !line.empty()) {
        line[0] = grown(line[0]);
        chr[0] = grown(chr[0]);
    }
    std::map<uint16_t, uint16_t> grownSpans;
    auto grownSpan = [&](uint16_t k) -> uint16_t {
        if (k == 0) return 0;
        auto it = grownSpans.find(k);
        if (it != grownSpans.end()) return it->second;
        line.push_back(grown(line[k]));
        chr.push_back(grown(chr[k]));
        const uint16_t g = (uint16_t)(line.size() - 1);
        grownSpans[k] = g;
        return g;
    };
    auto blockOf = [&](int64_t ch) -> uint32_t {
        if (ch < 0) return 0;
        auto it = chars.find((uint32_t)ch);
        return it != chars.end() ? it->second.slot : 0;
    };
    // a part's shapes by material block (parts are built independently, so splitting is exact)
    auto blocks = [&](const TitlePart3D& part) {
        std::map<uint32_t, Shapes> byBlock;
        for (size_t i = 0; i < part.shapes.size(); i++) byBlock[blockOf(part.owners[i])].push_back(part.shapes[i]);
        return byBlock;
    };
    ExtrudeStats stats;
    double wallTop = 0, zBack = 0;
    if (t.orb) {
        // the orb: front pole at z = 0; its equator is where the bands start; no bevel
        const auto& o = *t.orb;
        const double rx = o[0], ry = o[1], rz = ry;
        mesh.span = 0;
        mesh.part = parts.of(0, -2 * rz, -1, TITLE_PART_NO_SHEAR);
        extrudeOrb(mesh, (int)FACE_SLOT, rx, ry, rz, o[2], o[3], -rz, orbGridFor(std::max(rx, ry) * options.outputScale));
        out.solids++;
        wallTop = -rz;
        zBack = -rz;
    } else if (inflate) {
        // one distance-field height map per shape, all normalised by the title's largest inset
        const double height = px(p.inflate.height);
        wallTop = inflateWallTop(height, p.inflate.profile);
        zBack = wallTop - px(p.depth);
        Shapes all;
        for (const TitlePart3D& part : t.parts) all.insert(all.end(), part.shapes.begin(), part.shapes.end());
        // the output scale matters in powers of two only, so zooming the preview rarely rebuilds
        const double tierStep = inflateStep(all, inflateScale(options.outputScale), std::clamp(p.inflate.detail, 1, 4), options.inflateMaxNodes) *
                                (options.draft ? 2 : 1) * options.inflateStepScale;
        const double step = options.inflateBudgetScale > 1
                                ? std::max(tierStep, std::min(tierStep * options.inflateBudgetScale, inflateInset(all) / INFLATE_MIN_NODES_PER_INSET))
                                : tierStep;
        std::vector<std::vector<std::shared_ptr<const InflateField>>> partFields;
        double maxDist = 0;
        for (const TitlePart3D& part : t.parts) {
            std::vector<std::shared_ptr<const InflateField>> list;
            for (const ShapeContours& s : part.shapes) {
                list.push_back(fields ? fields->get(s, step) : buildInflateField(s, step));
                maxDist = std::max(maxDist, list.back()->maxDist);
            }
            partFields.push_back(std::move(list));
        }
        for (size_t k = 0; k < t.parts.size(); k++) {
            const TitlePart3D& part = t.parts[k];
            mesh.span = part.span;
            mesh.part = parts.of(0, zBack, part.rig, 0);
            for (size_t i = 0; i < partFields[k].size(); i++) {
                const uint32_t base = blockOf(part.owners[i]);
                InflateSpec spec;
                spec.faceSlot = (int)(base + FACE_SLOT);
                spec.wallSlot = spec.backSlot = (int)(base + SIDE_SLOT);
                spec.height = height;
                spec.profile = p.inflate.profile;
                spec.maxDist = maxDist;
                spec.zBack = zBack;
                spec.smoothness = p.inflate.smoothness;
                emitInflate(mesh, *partFields[k][i], spec);
                out.solids++;
            }
        }
    } else {
        // extrude: the face at z = 0 and the bevel from its edge out to the side wall, starting
        // outW - bw from the glyph outline
        std::vector<ProfileSpan> spans;
        ProfilePoint end;
        if (bw > 0) end = bandSpans(spans, (int)BEVEL_SLOT, outW - bw, 0, bw, px(p.bevel.depth), p.bevel.shape, segmentsOf(p.bevel.shape));
        wallTop = end.z;
        zBack = wallTop - px(p.depth);
        double zTop = 0;
        for (const ProfileSpan& s : spans) zTop = std::max({zTop, s.a.z, s.b.z});
        if (outW <= 0) {
            for (const TitlePart3D& part : t.parts) {
                mesh.span = part.span;
                mesh.part = parts.of(zTop, zBack, part.rig, 0);
                for (const auto& [base, shapes] : blocks(part)) {
                    SolidSpec spec;
                    spec.capOff = -bw;
                    spec.capZ = 0;
                    spec.capSlot = (int)(base + FACE_SLOT);
                    spec.spans = slotted(spans, base);
                    spec.wallSlot = spec.backSlot = (int)(base + SIDE_SLOT);
                    spec.zBack = zBack;
                    spec.testFlags = options.testFlags;
                    extrudeSolid(mesh, shapes, spec);
                    out.solids += (uint32_t)shapes.size();
                }
            }
        } else {
            // the face (and the inward half of `center`) per part; the outward bevel per group of
            // touching glyphs, so their rings merge instead of crossing
            std::vector<ProfileSpan> inner, outer;
            splitAtOutline(spans, inner, outer);
            const double ringStep = (options.draft ? 2.0 : 1.0) * options.ringStepScale / inflateScale(options.outputScale);
            auto outward = [&](const Shapes& shapes, uint32_t base, int rig, std::function<uint16_t(const ShapeContours&)> span) {
                mesh.part = parts.of(zTop, zBack, rig, 0);
                OutwardSpec spec;
                spec.spans = slotted(outer, base);
                spec.wallSlot = spec.backSlot = (int)(base + SIDE_SLOT);
                spec.zBack = zBack;
                spec.step = ringStep;
                spec.eps = eps;
                spec.span = std::move(span);
                spec.testFlags = options.testFlags;
                extrudeOutward(mesh, shapes, spec, &stats);
            };
            std::map<uint32_t, Shapes> flat;
            for (const TitlePart3D& part : t.parts) {
                for (const auto& [base, shapes] : blocks(part)) {
                    mesh.span = part.span;
                    mesh.part = parts.of(zTop, zBack, part.rig, 0);
                    SolidSpec spec;
                    spec.capOff = outW - bw;
                    spec.capZ = 0;
                    spec.capSlot = (int)(base + FACE_SLOT);
                    spec.spans = slotted(inner, base);
                    spec.wallSlot = spec.backSlot = (int)(base + SIDE_SLOT);
                    spec.zBack = zBack;
                    spec.open = true;
                    spec.testFlags = options.testFlags;
                    extrudeSolid(mesh, shapes, spec);
                    out.solids += (uint32_t)shapes.size();
                    if (part.rig >= 0) {
                        const uint16_t g = grownSpan(part.span);
                        outward(shapes, base, part.rig, [g](const ShapeContours&) { return g; });
                    } else {
                        Shapes& list = flat[base];
                        list.insert(list.end(), shapes.begin(), shapes.end());
                    }
                }
            }
            // unrigged glyphs share bevel rings per material block
            for (const auto& [base, shapes] : flat) {
                outward(shapes, base, -1, [&](const ShapeContours& c) { return grownSpan(unionSpan(line, chr, marksIn(t.marks, c))); });
            }
        }
    }
    out.pivotZ = (float)(zBack / 2);
    for (TitleMeshRig& rig : out.rigs) rig.pivot[2] = out.pivotZ;

    // the base outline of a band: `path` reshaped by its plate; `shapes` are `path` normalised
    auto plateBase = [&](const geometry::Path& path, const Shapes& shapes, const geometry::PlateParams& plate) -> Shapes {
        if (geometry::plateIsIdentity(plate)) return shapes;
        return flattenNormalized(geometry::plateBase(path, plate, size, t.shear), t.curveSegments, eps);
    };
    // an outline pushed out to the letter's silhouette (where the outward bevel ends)
    auto banded = [&](const geometry::Path& path, const Shapes& shapes) -> std::pair<geometry::Path, Shapes> {
        if (outW <= 0) return {path, shapes};
        Shapes grownShapes = offsetShapes(shapes, outW, false, false, eps);
        return {shapesToPath(grownShapes), grownShapes};
    };
    struct Piece {
        uint32_t block;
        Shapes base;
    };
    // a band stack behind the previous solid: band i's base outlines (and their blocks) from
    // `pieces`, each band shape's span from `span`, lifted toward the viewer by `lift`
    auto stack = [&](const std::function<std::vector<Piece>(size_t, const geometry::PlateParams&)>& pieces,
                     const std::function<uint16_t(const ShapeContours&)>& span, double lift, int rig, uint16_t flags) {
        double top = wallTop, back = zBack, grow = 0;
        for (size_t i = 0; i < p.bands.size(); i++) {
            const Band3D& b = p.bands[i];
            if (!b.enabled) continue;
            grow += px(b.grow);
            const double zFront = std::min(back - b.offset / 100 * size, top - (b.offset < 0 ? BAND_SEAT * size : 0));
            const double w = px(b.bevel.width);
            std::vector<ProfileSpan> profile;
            ProfilePoint end;
            end.z = zFront + lift;
            if (w > 0) end = bandSpans(profile, 0, -w, zFront + lift, w, px(b.bevel.depth), b.bevel.shape, segmentsOf(b.bevel.shape));
            top = end.z - lift;
            back = top - px(b.depth);
            mesh.part = parts.of(zFront + lift, back + lift, rig, flags);
            for (const Piece& piece : pieces(i, b.plate)) {
                const uint32_t front = piece.block + bandSlot(i);
                SolidSpec spec;
                spec.capOff = -w;
                spec.capZ = zFront + lift;
                spec.capSlot = (int)front;
                spec.spans = profile;
                for (ProfileSpan& s : spec.spans) s.slot = (int)front;
                spec.wallSlot = spec.backSlot = (int)front + 1;
                spec.zBack = back + lift;
                spec.testFlags = options.testFlags;
                const Shapes merged = offsetShapes(piece.base, grow, b.roundJoin, b.fillCounters, eps);
                if (b.roundJoin && piece.base.size() > 1) {
                    // round bands grow from each island on its own and overlap in depth; each piece
                    // keeps the span of the merged band shape it lies in
                    for (const ShapeContours& island : piece.base) {
                        for (const ShapeContours& shape : offsetShapes({island}, grow, true, b.fillCounters, eps)) {
                            const TitleVec2 q = interiorPoint(shape);
                            const ShapeContours* host = &shape;
                            for (const ShapeContours& m : merged) {
                                if (shapeContains(m, q.x, q.y)) {
                                    host = &m;
                                    break;
                                }
                            }
                            mesh.span = span(*host);
                            extrudeSolid(mesh, {shape}, spec);
                            out.solids++;
                        }
                    }
                    continue;
                }
                for (const ShapeContours& shape : merged) {
                    mesh.span = span(shape);
                    extrudeSolid(mesh, {shape}, spec);
                    out.solids++;
                }
            }
        }
    };

    // the shared stack backs the unrigged glyphs (or the shape / orb); a hull / box base spans the
    // whole title in the layer's block; a contour band i is one mesh in the layer's block for the
    // glyphs that don't override it, one mesh per block for those that do
    std::vector<const TitlePart3D*> riggedParts;
    for (const TitlePart3D& part : t.parts) {
        if (part.rig >= 0) riggedParts.push_back(&part);
    }
    // the owner of each shape of the shared outline (its unrigged part has that one shape)
    std::vector<int64_t> outlineOwner(t.outline.size(), -1);
    {
        size_t k = 0;
        for (const TitlePart3D& part : t.parts) {
            if (part.rig >= 0) continue;
            for (size_t j = 0; j < part.shapes.size() && k < outlineOwner.size(); j++) outlineOwner[k++] = part.owners[j];
        }
    }
    std::optional<std::pair<geometry::Path, Shapes>> sharedOutline;
    std::optional<geometry::Path> sharedAll;
    auto sharedPieces = [&](size_t i, const geometry::PlateParams& plate) -> std::vector<Piece> {
        if (plate.kind != geometry::PlateKind::Contour) {
            if (!sharedAll) {
                if (outW > 0) {
                    Shapes all = t.outline;
                    for (const TitlePart3D* part : riggedParts) all.insert(all.end(), part->shapes.begin(), part->shapes.end());
                    sharedAll = banded(geometry::Path(), all).first;
                } else {
                    sharedAll = t.allPath;
                }
            }
            return {{0, plateBase(*sharedAll, t.outline, plate)}};
        }
        std::map<uint32_t, Shapes> own;
        Shapes rest;
        for (size_t k = 0; k < t.outline.size(); k++) {
            const int64_t ch = outlineOwner[k];
            auto it = ch >= 0 ? chars.find((uint32_t)ch) : chars.end();
            if (it == chars.end() || i >= it->second.bands.size() || !it->second.bands[i]) rest.push_back(t.outline[k]);
            else own[it->second.slot].push_back(t.outline[k]);
        }
        if (own.empty()) {
            if (!sharedOutline) sharedOutline = banded(t.outlinePath, t.outline);
            return {{0, plateBase(sharedOutline->first, sharedOutline->second, plate)}};
        }
        std::vector<Piece> list;
        auto add = [&](uint32_t block, const Shapes& shapes) {
            if (shapes.empty()) return;
            const auto b = banded(shapesToPath(shapes), shapes);
            list.push_back({block, plateBase(b.first, b.second, plate)});
        };
        add(0, rest);
        for (const auto& [block, shapes] : own) add(block, shapes);
        return list;
    };
    stack(sharedPieces, [&](const ShapeContours& shape) { return unionSpan(line, chr, marksIn(t.marks, shape)); }, 0, -1,
          t.orb ? TITLE_PART_NO_SHEAR : 0);
    // a rigged glyph carries its own stack on its own contour, in its own block, lifted a hair
    for (const TitlePart3D* part : riggedParts) {
        const uint32_t block = blockOf(part->owners.empty() ? -1 : part->owners[0]);
        std::optional<std::pair<geometry::Path, Shapes>> own;
        auto glyphPieces = [&](size_t, const geometry::PlateParams& plate) -> std::vector<Piece> {
            if (!own) own = banded(part->outline, part->shapes);
            geometry::PlateParams contour = plate;
            contour.kind = geometry::PlateKind::Contour;
            return {{block, plateBase(own->first, own->second, contour)}};
        };
        const uint16_t span = part->span;
        stack(glyphPieces, [span](const ShapeContours&) { return span; }, RIG_BAND_LIFT * size, part->rig, 0);
    }

    out.fallbackUsed = stats.fallbackUsed;
    out.ringLevels = stats.ringLevels;
    out.parts = std::move(parts.parts);
    out.spans.reserve(line.size());
    for (size_t i = 0; i < line.size(); i++) {
        out.spans.push_back({{(float)line[i].x0, (float)line[i].y0, (float)line[i].x1, (float)line[i].y1},
                             {(float)chr[i].x0, (float)chr[i].y0, (float)chr[i].x1, (float)chr[i].y1}});
    }
    return out;
}
