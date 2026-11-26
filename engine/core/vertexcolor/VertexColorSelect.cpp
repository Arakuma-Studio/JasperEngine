// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorSelect.h"

#include "vertexcolor/VertexColorInternal.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace jasperengine::vertexcolor {

namespace {

    uint32_t selectLimit(const MeshView& mesh, ElementKind kind) {
        switch (kind) {
            case ElementKind::Vertex: return mesh.logicalCount();
            case ElementKind::Face: return mesh.triangleCount();
            case ElementKind::Corner: return mesh.triangleCount() * 3;
        }
        return 0;
    }

    // a membership mask of the selection's valid ids
    std::vector<uint8_t> selectMask(const MeshView& mesh, const Selection& selection) {
        std::vector<uint8_t> mask(selectLimit(mesh, selection.kind), 0);
        for (uint32_t id : selection.ids) {
            if (id < mask.size()) mask[id] = 1;
        }
        return mask;
    }

    Selection selectFromMask(ElementKind kind, const std::vector<uint8_t>& mask) {
        Selection out;
        out.kind = kind;
        for (uint32_t i = 0; i < (uint32_t)mask.size(); i++) {
            if (mask[i]) out.ids.push_back(i);
        }
        return out;
    }

    uint32_t selectCornerLogical(const MeshView& mesh, uint32_t corner) {
        return mesh.logical[mesh.indices[corner]];
    }

    // the logical vertices' connected components through edges (seams do not cut them)
    std::vector<uint32_t> selectComponents(const MeshView& mesh) {
        const uint32_t count = mesh.logicalCount();
        std::vector<uint32_t> component(count, UINT32_MAX);
        std::vector<uint32_t> stack;
        uint32_t next = 0;
        for (uint32_t seed = 0; seed < count; seed++) {
            if (component[seed] != UINT32_MAX) continue;
            component[seed] = next;
            stack.push_back(seed);
            while (!stack.empty()) {
                const uint32_t l = stack.back();
                stack.pop_back();
                for (uint32_t k = mesh.neighborStart[l]; k < mesh.neighborStart[l + 1]; k++) {
                    const uint32_t n = mesh.neighbors[k];
                    if (component[n] == UINT32_MAX) {
                        component[n] = next;
                        stack.push_back(n);
                    }
                }
            }
            next++;
        }
        return component;
    }

}

Selection selectAll(const MeshView& mesh, ElementKind kind) {
    Selection out;
    out.kind = kind;
    const uint32_t limit = selectLimit(mesh, kind);
    out.ids.resize(limit);
    for (uint32_t i = 0; i < limit; i++) out.ids[i] = i;
    return out;
}

Selection invert(const MeshView& mesh, const Selection& selection) {
    std::vector<uint8_t> mask = selectMask(mesh, selection);
    for (uint8_t& m : mask) m = m ? 0 : 1;
    return selectFromMask(selection.kind, mask);
}

Selection convert(const MeshView& mesh, const Selection& selection, ElementKind kind, bool anyVertex) {
    if (selection.kind == kind) {
        std::vector<uint8_t> mask = selectMask(mesh, selection);
        return selectFromMask(kind, mask);
    }
    const std::vector<uint8_t> mask = selectMask(mesh, selection);
    std::vector<uint8_t> out(selectLimit(mesh, kind), 0);
    const uint32_t triangles = mesh.triangleCount();
    switch (selection.kind) {
        case ElementKind::Vertex:
            if (kind == ElementKind::Face) {
                for (uint32_t t = 0; t < triangles; t++) {
                    int selected = 0;
                    for (uint32_t k = 0; k < 3; k++) selected += mask[selectCornerLogical(mesh, t * 3 + k)] ? 1 : 0;
                    out[t] = anyVertex ? (selected > 0) : (selected == 3);
                }
            } else {
                for (uint32_t c = 0; c < triangles * 3; c++) out[c] = mask[selectCornerLogical(mesh, c)];
            }
            break;
        case ElementKind::Face:
            for (uint32_t t = 0; t < triangles; t++) {
                if (!mask[t]) continue;
                for (uint32_t k = 0; k < 3; k++) {
                    if (kind == ElementKind::Vertex) out[selectCornerLogical(mesh, t * 3 + k)] = 1;
                    else out[t * 3 + k] = 1;
                }
            }
            break;
        case ElementKind::Corner:
            for (uint32_t c = 0; c < triangles * 3; c++) {
                if (!mask[c]) continue;
                if (kind == ElementKind::Vertex) out[selectCornerLogical(mesh, c)] = 1;
                else out[c / 3] = 1;
            }
            break;
    }
    return selectFromMask(kind, out);
}

Selection grow(const MeshView& mesh, const Selection& selection) {
    if (selection.kind == ElementKind::Face) {
        const std::vector<uint8_t> mask = selectMask(mesh, selection);
        std::vector<uint8_t> out = mask;
        for (uint32_t t = 0; t < (uint32_t)mask.size(); t++) {
            if (!mask[t]) continue;
            for (uint32_t k = mesh.faceNeighborStart[t]; k < mesh.faceNeighborStart[t + 1]; k++) out[mesh.faceNeighbors[k]] = 1;
        }
        return selectFromMask(ElementKind::Face, out);
    }
    // corners grow as their logical vertices do: a ring of vertices, lifted back to every corner
    const Selection vertices = selection.kind == ElementKind::Vertex ? selection : convert(mesh, selection, ElementKind::Vertex);
    const std::vector<uint8_t> mask = selectMask(mesh, vertices);
    std::vector<uint8_t> out = mask;
    for (uint32_t l = 0; l < (uint32_t)mask.size(); l++) {
        if (!mask[l]) continue;
        for (uint32_t k = mesh.neighborStart[l]; k < mesh.neighborStart[l + 1]; k++) out[mesh.neighbors[k]] = 1;
    }
    const Selection grown = selectFromMask(ElementKind::Vertex, out);
    return selection.kind == ElementKind::Vertex ? grown : convert(mesh, grown, ElementKind::Corner);
}

Selection shrink(const MeshView& mesh, const Selection& selection) {
    if (selection.kind == ElementKind::Face) {
        const std::vector<uint8_t> mask = selectMask(mesh, selection);
        std::vector<uint8_t> out = mask;
        for (uint32_t t = 0; t < (uint32_t)mask.size(); t++) {
            if (!mask[t]) continue;
            for (uint32_t k = mesh.faceNeighborStart[t]; k < mesh.faceNeighborStart[t + 1]; k++) {
                if (!mask[mesh.faceNeighbors[k]]) {
                    out[t] = 0;
                    break;
                }
            }
        }
        return selectFromMask(ElementKind::Face, out);
    }
    std::vector<uint8_t> mask;
    if (selection.kind == ElementKind::Vertex) {
        mask = selectMask(mesh, selection);
    } else {
        // a logical vertex counts as selected only with all of its corners
        const std::vector<uint8_t> corners = selectMask(mesh, selection);
        mask.assign(mesh.logicalCount(), 1);
        std::vector<uint8_t> used(mesh.logicalCount(), 0);
        for (uint32_t c = 0; c < (uint32_t)corners.size(); c++) {
            const uint32_t l = selectCornerLogical(mesh, c);
            used[l] = 1;
            if (!corners[c]) mask[l] = 0;
        }
        for (uint32_t l = 0; l < mesh.logicalCount(); l++) mask[l] = mask[l] && used[l];
    }
    std::vector<uint8_t> out = mask;
    for (uint32_t l = 0; l < (uint32_t)mask.size(); l++) {
        if (!mask[l]) continue;
        for (uint32_t k = mesh.neighborStart[l]; k < mesh.neighborStart[l + 1]; k++) {
            if (!mask[mesh.neighbors[k]]) {
                out[l] = 0;
                break;
            }
        }
    }
    const Selection shrunk = selectFromMask(ElementKind::Vertex, out);
    if (selection.kind == ElementKind::Vertex) return shrunk;
    const Selection corners = convert(mesh, shrunk, ElementKind::Corner);
    // never adds: only corners the selection had
    const std::vector<uint8_t> had = selectMask(mesh, selection);
    Selection kept;
    kept.kind = ElementKind::Corner;
    for (uint32_t c : corners.ids) {
        if (had[c]) kept.ids.push_back(c);
    }
    return kept;
}

Selection linked(const MeshView& mesh, const Selection& selection) {
    const std::vector<uint32_t> component = selectComponents(mesh);
    std::vector<uint8_t> picked(mesh.logicalCount(), 0);
    const Selection vertices = convert(mesh, selection, ElementKind::Vertex);
    for (uint32_t l : vertices.ids) picked[component[l]] = 1;
    std::vector<uint8_t> out(selectLimit(mesh, selection.kind), 0);
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
        uint32_t l = i;
        if (selection.kind == ElementKind::Face) l = selectCornerLogical(mesh, i * 3);
        else if (selection.kind == ElementKind::Corner) l = selectCornerLogical(mesh, i);
        out[i] = picked[component[l]];
    }
    return selectFromMask(selection.kind, out);
}

Selection flatRegion(const MeshView& mesh, uint32_t seedTriangle, float angle) {
    Selection out;
    out.kind = ElementKind::Face;
    const uint32_t triangles = mesh.triangleCount();
    if (seedTriangle >= triangles) return out;
    const float cosLimit = (float)detail::detCos((double)angle * detail::kPi / 180.0) - 1e-6f;
    std::vector<uint8_t> mask(triangles, 0);
    std::vector<uint32_t> stack{seedTriangle};
    mask[seedTriangle] = 1;
    while (!stack.empty()) {
        const uint32_t t = stack.back();
        stack.pop_back();
        const Vector3& n = mesh.faceNormals[t];
        for (uint32_t k = mesh.faceNeighborStart[t]; k < mesh.faceNeighborStart[t + 1]; k++) {
            const uint32_t u = mesh.faceNeighbors[k];
            if (mask[u]) continue;
            // a degenerate face has no normal: the region never crosses it
            if (detail::dot3(mesh.faceNormals[u], mesh.faceNormals[u]) <= 0.0f) continue;
            if (detail::dot3(n, mesh.faceNormals[u]) >= cosLimit) {
                mask[u] = 1;
                stack.push_back(u);
            }
        }
    }
    return selectFromMask(ElementKind::Face, mask);
}

Selection similarColor(const TargetData& data, const MeshView& mesh, ElementKind kind, const ColorMatch& match) {
    std::vector<Vector4> base, corners;
    detail::denseValues(data, mesh, base, corners);
    std::vector<uint8_t> out(selectLimit(mesh, kind), 0);
    switch (kind) {
        case ElementKind::Vertex:
            for (uint32_t l = 0; l < mesh.logicalCount(); l++) {
                bool all = true;
                bool any = false;
                for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1] && all; m++) {
                    const uint32_t v = mesh.members[m];
                    for (uint32_t k = mesh.vertexCornerStart[v]; k < mesh.vertexCornerStart[v + 1] && all; k++) {
                        any = true;
                        all = detail::colorMatches(corners[mesh.vertexCorners[k]], match);
                    }
                }
                if (!any) {
                    for (uint32_t m = mesh.memberStart[l]; m < mesh.memberStart[l + 1] && all; m++) all = detail::colorMatches(base[mesh.members[m]], match);
                }
                out[l] = all ? 1 : 0;
            }
            break;
        case ElementKind::Face: {
            const float coverage = std::max(0.0f, std::min(1.0f, match.coverage));
            for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
                int matched = 0;
                for (uint32_t k = 0; k < 3; k++) matched += detail::colorMatches(corners[t * 3 + k], match) ? 1 : 0;
                out[t] = matched > 0 && (float)matched / 3.0f + 1e-6f >= coverage;
            }
            break;
        }
        case ElementKind::Corner:
            for (uint32_t c = 0; c < (uint32_t)corners.size(); c++) out[c] = detail::colorMatches(corners[c], match) ? 1 : 0;
            break;
    }
    return selectFromMask(kind, out);
}

Selection byNormal(const MeshView& mesh, ElementKind kind, const Vector3& direction, float angle) {
    const Vector3 d = detail::normalize3(direction);
    const float cosLimit = (float)detail::detCos((double)angle * detail::kPi / 180.0) - 1e-6f;
    std::vector<uint8_t> out(selectLimit(mesh, kind), 0);
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
        const Vector3& n = kind == ElementKind::Vertex ? mesh.logicalNormals[i] : mesh.faceNormals[kind == ElementKind::Face ? i : i / 3];
        out[i] = detail::dot3(n, d) >= cosLimit ? 1 : 0;
    }
    return selectFromMask(kind, out);
}

Selection byChannelRange(const TargetData& data, const MeshView& mesh, ElementKind kind, int channel, float min, float max) {
    std::vector<Vector4> base, corners;
    detail::denseValues(data, mesh, base, corners);
    const size_t c = (size_t)std::max(0, std::min(3, channel));
    std::vector<uint8_t> out(selectLimit(mesh, kind), 0);
    for (uint32_t i = 0; i < (uint32_t)out.size(); i++) {
        float value;
        if (kind == ElementKind::Vertex) {
            value = detail::logicalMean(mesh, base, corners, i)[c];
        } else if (kind == ElementKind::Face) {
            value = (corners[i * 3][c] + corners[i * 3 + 1][c] + corners[i * 3 + 2][c]) / 3.0f;
        } else {
            value = corners[i][c];
        }
        out[i] = value >= min && value <= max ? 1 : 0;
    }
    return selectFromMask(kind, out);
}

Selection unite(const Selection& a, const Selection& b) {
    if (a.kind != b.kind) return a;
    Selection out;
    out.kind = a.kind;
    std::vector<uint32_t> x(a.ids), y(b.ids);
    detail::sortedUniqueCount(x);
    detail::sortedUniqueCount(y);
    std::set_union(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(out.ids));
    return out;
}

Selection subtract(const Selection& a, const Selection& b) {
    if (a.kind != b.kind) return a;
    Selection out;
    out.kind = a.kind;
    std::vector<uint32_t> x(a.ids), y(b.ids);
    detail::sortedUniqueCount(x);
    detail::sortedUniqueCount(y);
    std::set_difference(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(out.ids));
    return out;
}

Vector4 averageValue(const TargetData& data, const MeshView& mesh, const Selection& selection) {
    std::vector<Vector4> base, corners;
    detail::denseValues(data, mesh, base, corners);
    const Selection asCorners = convert(mesh, selection, ElementKind::Corner);
    if (asCorners.ids.empty()) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    double sum[4] = {0.0, 0.0, 0.0, 0.0};
    for (uint32_t c : asCorners.ids) {
        sum[0] += corners[c].x;
        sum[1] += corners[c].y;
        sum[2] += corners[c].z;
        sum[3] += corners[c].w;
    }
    const double n = (double)asCorners.ids.size();
    return Vector4((float)(sum[0] / n), (float)(sum[1] / n), (float)(sum[2] / n), (float)(sum[3] / n));
}

Target toTarget(const Selection& selection) {
    Target target;
    switch (selection.kind) {
        case ElementKind::Vertex: target.kind = TargetKind::Vertices; break;
        case ElementKind::Face: target.kind = TargetKind::Faces; break;
        case ElementKind::Corner: target.kind = TargetKind::Corners; break;
    }
    target.ids = selection.ids;
    return target;
}

}
