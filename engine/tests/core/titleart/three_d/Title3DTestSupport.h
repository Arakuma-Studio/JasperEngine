// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The 3D title tests' helpers: glyph outlines of the test fonts as mesher sources, and the
// mesh checks of S86 AC3 (closed 2-manifold, Euler characteristic, signed volume, normals).

#pragma once

#include "titleart/support/TitleTestAssets.h"

#include "titleart/geometry/TitleGeometry.h"
#include "titleart/render/TitleContent.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"
#include "titleart/three_d/mesher/RawMesh.h"
#include "titleart/three_d/mesher/TitleMesher.h"
#include "titleart/three_d/mesher/TitleOutline3D.h"

#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace jasperengine::titleart::test {

    // a text layer in a test font, normalized
    inline Json textLayer3D(const std::string& text, const char* font = DELA_GOTHIC, double size = 200, Json style3d = nullptr) {
        Json layer = {{"type", "text"}, {"text", text}, {"font", fontRef(font)}, {"size", size}, {"dimension", "3d"}};
        if (!style3d.is_null()) layer["style_3d"] = style3d;
        return layer;
    }

    // the mesher's source of a layer of `doc` through the module's content provider
    inline three_d::TitleSource3D sourceOf(const TitleArtDoc& doc, size_t layerIndex, const TestAssetResolver& assets) {
        auto content = makeGeometryTitleContentProvider();
        return three_d::collectTitleSource(doc.layers()[layerIndex], doc, *content, &assets, nullptr);
    }

    // a square polygon (counter-clockwise) centred at (cx, cy)
    inline three_d::Contour squareContour(double cx, double cy, double half, bool clockwise = false) {
        three_d::Contour c = {{cx - half, cy - half}, {cx + half, cy - half}, {cx + half, cy + half}, {cx - half, cy + half}};
        if (clockwise) std::reverse(c.begin(), c.end());
        return c;
    }

    inline three_d::Contour circleContour(double cx, double cy, double r, int n) {
        three_d::Contour c;
        for (int i = 0; i < n; i++) {
            const double a = 2 * 3.14159265358979323846 * i / n;
            c.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
        return c;
    }

    struct MeshCheck {
        bool closed = true;             // every undirected edge used exactly twice, once each way
        size_t badEdges = 0;
        std::string firstBad;           // the first open edges' end points, for the failure message
        int euler = 0;                  // V - E + F over positions
        double volume = 0;              // signed, positive when the triangles face out
        size_t triangles = 0;
    };

    // positions of a triangle soup (3 per triangle) checked as one closed surface; triangles with
    // two corners at one point are skipped (they cover nothing)
    inline MeshCheck checkTriangles(const std::vector<std::array<float, 3>>& corners) {
        MeshCheck out;
        std::map<std::array<float, 3>, uint32_t> ids;
        std::vector<std::array<float, 3>> points;
        auto id = [&](const std::array<float, 3>& p) {
            auto it = ids.find(p);
            if (it != ids.end()) return it->second;
            const uint32_t n = (uint32_t)ids.size();
            ids[p] = n;
            points.push_back(p);
            return n;
        };
        std::map<std::pair<uint32_t, uint32_t>, int> directed;
        for (size_t t = 0; t + 2 < corners.size(); t += 3) {
            const uint32_t a = id(corners[t]), b = id(corners[t + 1]), c = id(corners[t + 2]);
            if (a == b || b == c || a == c) continue;
            out.triangles++;
            directed[{a, b}]++;
            directed[{b, c}]++;
            directed[{c, a}]++;
            const auto& p = corners[t];
            const auto& q = corners[t + 1];
            const auto& r = corners[t + 2];
            out.volume += (double)p[0] * ((double)q[1] * r[2] - (double)q[2] * r[1]) - (double)p[1] * ((double)q[0] * r[2] - (double)q[2] * r[0]) +
                          (double)p[2] * ((double)q[0] * r[1] - (double)q[1] * r[0]);
        }
        out.volume /= 6;
        std::set<std::pair<uint32_t, uint32_t>> edges;
        for (const auto& [e, count] : directed) {
            const auto back = directed.find({e.second, e.first});
            if (count != 1 || back == directed.end() || back->second != 1) {
                out.closed = false;
                if (out.badEdges++ < 4) {
                    for (uint32_t v : {e.first, e.second}) {
                        out.firstBad += "(" + std::to_string(points[v][0]) + ", " + std::to_string(points[v][1]) + ", " +
                                        std::to_string(points[v][2]) + ")";
                    }
                    out.firstBad += " x" + std::to_string(count) + "/" + std::to_string(back == directed.end() ? 0 : back->second) + "; ";
                }
            }
            edges.insert({std::min(e.first, e.second), std::max(e.first, e.second)});
        }
        std::set<uint32_t> used;
        for (const auto& e : edges) {
            used.insert(e.first);
            used.insert(e.second);
        }
        out.euler = (int)used.size() - (int)edges.size() + (int)out.triangles;
        return out;
    }

    inline MeshCheck checkRaw(const three_d::RawMesh& mesh) {
        std::vector<std::array<float, 3>> corners;
        for (const auto& slot : mesh.slots) {
            for (const three_d::RawVertex& v : slot) corners.push_back({v.x, v.y, v.z});
        }
        return checkTriangles(corners);
    }

    inline MeshCheck checkMesh(const TitleMesh& mesh) {
        std::vector<std::array<float, 3>> corners;
        for (size_t i = 0; i < mesh.indexCount(); i++) {
            const TitleMeshVertex& v = mesh.vertices[mesh.index(i)];
            corners.push_back({v.position[0], v.position[1], v.position[2]});
        }
        return checkTriangles(corners);
    }

}
