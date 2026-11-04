// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// glTF files made by the tests themselves (spec S53's cook tests): a builder for buffers, views
// and accessors, and a model that has what the cook must keep: a node tree, two meshes, a
// blended material, a morph target, a skin and an animation. Every file is a pure function of
// its arguments.

#pragma once

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    struct GltfBuild {
        nlohmann::ordered_json json;
        std::vector<uint8_t> bin;

        GltfBuild();
        size_t addView(const void* data, size_t size, int target = 0, size_t stride = 0);
        size_t addAccessor(size_t view, int componentType, size_t count, const char* type, bool normalized = false);
        // an accessor of floats in a view of its own, with POSITION's bounds when asked
        size_t addFloats(const std::vector<float>& values, int components, int target, bool bounds = false);
        size_t addIndices(const std::vector<uint32_t>& indices, int componentType = 5125);
        std::vector<uint8_t> glb() const;
        void write(const std::filesystem::path& file) const;
    };

    struct GridOptions {
        int quads = 24;             // per side
        bool welded = true;         // false: every triangle has vertices of its own
        bool shuffled = true;       // the triangles in a seeded random order
        bool normals = true;
        bool uvs = true;
        bool colors = false;
        float bump = 0.15f;         // the height of the surface's waves
        float offset = 0.0f;        // moves the grid along x
        int seed = 7;
    };

    // a primitive of a bumpy grid: its JSON (attributes, indices) on `build`'s accessors
    nlohmann::ordered_json addGridPrimitive(GltfBuild& build, const GridOptions& options);

    // The cook tests' model: Root > (Body: mesh "Body" of two grid primitives, the second
    // drawn BLEND; Prop: mesh "Prop" with a morph target, a skin of one joint, and a
    // translation animation)
    std::vector<uint8_t> makeTestModel(int quads = 24, bool welded = false);

}
