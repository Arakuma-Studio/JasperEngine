// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// glTF models written for the scene-side vertex colour tests (VertexColorScene, the render path,
// Lua): nodes of grid primitives facing +Z, optionally with COLOR_0, a two-joint skin (weights by
// height) and a morph target pushing the grid along +Z.

#include "TempDir.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vctest {

    struct GridPrimitive {
        int cells = 2;              // cells x cells quads, (cells + 1)^2 vertices
        float size = 1.0f;
        float x = 0.0f, y = 0.0f;   // its centre
        bool colors = false;        // COLOR_0 (VEC4 float): red rising with x, green with y
        bool skin = false;          // JOINTS_0 / WEIGHTS_0 (the node gets skin 0)
        bool morph = false;         // one POSITION target
    };

    struct GltfNode {
        std::string name;
        std::vector<GridPrimitive> primitives;
        float x = 0.0f;             // node translation
    };

    // <dir>/<name>.gltf with its .bin; returns the .gltf path
    inline std::string writeGridModel(const jasperengine::test::TempDir& dir, const std::string& name, const std::vector<GltfNode>& nodes) {
        std::vector<uint8_t> bin;
        std::ostringstream views, accessors, meshes, nodeList, sceneNodes;
        int viewCount = 0, accessorCount = 0;
        bool anySkin = false;
        auto addView = [&](const void* data, size_t bytes, int target) {
            while (bin.size() % 4) bin.push_back(0);
            const size_t offset = bin.size();
            bin.resize(offset + bytes);
            std::memcpy(bin.data() + offset, data, bytes);
            views << (viewCount ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << bytes;
            if (target) views << ",\"target\":" << target;
            views << "}";
            return viewCount++;
        };
        auto addAccessor = [&](int view, int componentType, size_t count, const char* type, const std::string& extra) {
            accessors << (accessorCount ? "," : "") << "{\"bufferView\":" << view << ",\"componentType\":" << componentType
                      << ",\"count\":" << count << ",\"type\":\"" << type << "\"" << extra << "}";
            return accessorCount++;
        };
        for (size_t n = 0; n < nodes.size(); n++) {
            meshes << (n ? "," : "") << "{\"name\":\"" << nodes[n].name << "\",\"primitives\":[";
            bool nodeSkin = false, nodeMorph = false;
            for (size_t p = 0; p < nodes[n].primitives.size(); p++) {
                const GridPrimitive& g = nodes[n].primitives[p];
                const int row = g.cells + 1;
                std::vector<float> positions, normals, colors, weights, morph;
                std::vector<uint16_t> joints;
                float lo[3] = {1e9f, 1e9f, 0.0f}, hi[3] = {-1e9f, -1e9f, 0.0f};
                for (int j = 0; j <= g.cells; j++) {
                    for (int i = 0; i <= g.cells; i++) {
                        const float u = (float)i / g.cells, v = (float)j / g.cells;
                        const float px = g.x + (u - 0.5f) * g.size, py = g.y + (v - 0.5f) * g.size;
                        positions.insert(positions.end(), {px, py, 0.0f});
                        lo[0] = std::min(lo[0], px); lo[1] = std::min(lo[1], py);
                        hi[0] = std::max(hi[0], px); hi[1] = std::max(hi[1], py);
                        normals.insert(normals.end(), {0.0f, 0.0f, 1.0f});
                        colors.insert(colors.end(), {u, v, 0.25f, 1.0f});
                        joints.insert(joints.end(), {0, 1, 0, 0});
                        weights.insert(weights.end(), {1.0f - v, v, 0.0f, 0.0f});
                        morph.insert(morph.end(), {0.0f, 0.0f, 0.3f * u});
                    }
                }
                std::vector<uint16_t> indices;
                for (int j = 0; j < g.cells; j++) {
                    for (int i = 0; i < g.cells; i++) {
                        const uint16_t a = (uint16_t)(j * row + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + row), d = (uint16_t)(c + 1);
                        indices.insert(indices.end(), {a, b, c, b, d, c});
                    }
                }
                const size_t vertices = positions.size() / 3;
                std::ostringstream bounds;
                bounds << ",\"min\":[" << lo[0] << "," << lo[1] << ",0],\"max\":[" << hi[0] << "," << hi[1] << ",0]";
                const int pos = addAccessor(addView(positions.data(), positions.size() * 4, 34962), 5126, vertices, "VEC3", bounds.str());
                const int nor = addAccessor(addView(normals.data(), normals.size() * 4, 34962), 5126, vertices, "VEC3", "");
                std::ostringstream attributes;
                attributes << "\"POSITION\":" << pos << ",\"NORMAL\":" << nor;
                if (g.colors)
                    attributes << ",\"COLOR_0\":" << addAccessor(addView(colors.data(), colors.size() * 4, 34962), 5126, vertices, "VEC4", "");
                if (g.skin) {
                    attributes << ",\"JOINTS_0\":" << addAccessor(addView(joints.data(), joints.size() * 2, 34962), 5123, vertices, "VEC4", "");
                    attributes << ",\"WEIGHTS_0\":" << addAccessor(addView(weights.data(), weights.size() * 4, 34962), 5126, vertices, "VEC4", "");
                    nodeSkin = anySkin = true;
                }
                const int idx = addAccessor(addView(indices.data(), indices.size() * 2, 34963), 5123, indices.size(), "SCALAR", "");
                meshes << (p ? "," : "") << "{\"attributes\":{" << attributes.str() << "},\"indices\":" << idx;
                if (g.morph) {
                    const int target = addAccessor(addView(morph.data(), morph.size() * 4, 34962), 5126, vertices, "VEC3",
                                                   ",\"min\":[0,0,0],\"max\":[0,0,0.3]");
                    meshes << ",\"targets\":[{\"POSITION\":" << target << "}]";
                    nodeMorph = true;
                }
                meshes << "}";
            }
            meshes << "]";
            if (nodeMorph) meshes << ",\"weights\":[0.0]";
            meshes << "}";
            nodeList << (n ? "," : "") << "{\"name\":\"" << nodes[n].name << "\",\"mesh\":" << n;
            if (nodes[n].x != 0.0f) nodeList << ",\"translation\":[" << nodes[n].x << ",0,0]";
            if (nodeSkin) nodeList << ",\"skin\":0";
            nodeList << "}";
            sceneNodes << (n ? "," : "") << n;
        }
        std::string skins;
        if (anySkin) {
            // two joints: the root at the origin, the tip one unit up; inverse binds undo them
            const int root = (int)nodes.size(), tip = root + 1;
            nodeList << ",{\"name\":\"Root\",\"children\":[" << tip << "]},{\"name\":\"Tip\",\"translation\":[0,1,0]}";
            sceneNodes << "," << root;
            const float inverseBinds[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                                            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
            const int ibm = addAccessor(addView(inverseBinds, sizeof(inverseBinds), 0), 5126, 2, "MAT4", "");
            skins = ",\"skins\":[{\"joints\":[" + std::to_string(root) + "," + std::to_string(tip) + "],\"inverseBindMatrices\":" +
                    std::to_string(ibm) + ",\"skeleton\":" + std::to_string(root) + "}]";
        }
        {
            std::ofstream out(dir.file(name + ".bin"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bin.data()), (std::streamsize)bin.size());
        }
        std::ofstream(dir.file(name + ".gltf")) << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[" << sceneNodes.str()
            << "]}],\"nodes\":[" << nodeList.str() << "],\"meshes\":[" << meshes.str() << "]" << skins
            << ",\"materials\":[],\"buffers\":[{\"uri\":\"" << name << ".bin\",\"byteLength\":" << bin.size() << "}],\"bufferViews\":["
            << views.str() << "],\"accessors\":[" << accessors.str() << "]}";
        return dir.file(name + ".gltf").generic_string();
    }

}
