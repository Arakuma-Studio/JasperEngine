// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICEBAKEGLTF_H
#define LATTICEBAKEGLTF_H

// The file side of the lattice bake (spec §9.4, §12 WP-BAKE): a glTF (.gltf or .glb) read with
// its buffers, new vertex data given per node and primitive, written back as one self-contained
// binary glTF. The JSON is kept as it was read (key order, extensions, extras, materials, skins,
// animations, images); only what the new data needs is added or repointed:
// - every buffer is merged into the single GLB buffer (external .bin files and data URIs embedded),
//   bufferViews moved with them; images keep their URIs, valid next to the original file;
// - a mesh used by several nodes that get different data is copied for them (node.mesh repointed);
// - an accessor is rewritten in place only when it is FLOAT, dense, every primitive using it gets
//   the same new data and no other accessor or image shares its bytes; else the primitives get new
//   FLOAT accessors (a sparse morph target without a base stays sparse with the same indices) and
//   the old one is left as it was for its other users.

#include "json.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class LatticeBakeGltf{
    public:
        // one vertex attribute of a primitive: target -1 is the primitive's own attribute, else
        // the morph target's (a displacement)
        struct Slot{
            int primitive = 0;
            int target = -1;
            std::string semantic;

            bool operator<(const Slot& other) const;
        };

        bool load(const std::filesystem::path& file, std::string& error);

        const nlohmann::ordered_json& getJson() const { return json; }

        // the accessor as floats, `components` per element: normalized integers as the GPU reads
        // them (unsigned c / max, signed max(c / max, -1)), other integers as their value, the
        // sparse substitution applied, zeros without a bufferView
        bool readAccessor(int accessor, std::vector<float>& values, int& components, std::string& error) const;

        // new data for one attribute of one primitive of the mesh `node` uses: 3 components for
        // POSITION, NORMAL and every morph displacement, 4 for TANGENT (w kept by the caller)
        void setAttribute(int node, const Slot& slot, std::vector<float> values, int components);
        bool hasChanges() const { return !outputs.empty(); }

        // writes the document with the new data as a binary glTF at `file`, which must not exist;
        // nothing is left behind on failure. The document is consumed (load again to reuse it).
        bool saveBinary(const std::filesystem::path& file, std::string& error);

    private:
        struct Data{
            int components = 0;
            std::vector<float> values;

            bool sameAs(const Data& other) const;
        };

        nlohmann::ordered_json json;
        std::vector<std::vector<uint8_t>> buffers;
        // node -> its primitives' new data
        std::map<int, std::map<Slot, Data>> outputs;

        bool accessorLayout(int accessor, int& componentType, int& components, size_t& count, std::string& error) const;
    };

}

#endif //LATTICEBAKEGLTF_H
