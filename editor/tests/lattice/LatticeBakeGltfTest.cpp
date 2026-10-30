// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The bake's glTF writer (LatticeBakeGltf): a shared accessor that two nodes give different data.

#include "lattice/LatticeBakeGltf.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;

namespace {

    std::string base64(const std::vector<unsigned char>& bytes){
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3){
            const unsigned v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i + 1 == bytes.size()){
            const unsigned v = bytes[i] << 16;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += "==";
        }else if (i + 2 == bytes.size()){
            const unsigned v = (bytes[i] << 16) | (bytes[i + 1] << 8);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    // one triangle (accessor "pos"), one mesh, two nodes using it
    std::string sharedMeshGltf(){
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        std::vector<unsigned char> bytes(sizeof(positions));
        std::memcpy(bytes.data(), positions, sizeof(positions));
        return std::string(R"({"asset": {"version": "2.0"},
            "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,)") + base64(bytes) + R"("}],
            "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962}],
            "accessors": [{"name": "pos", "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                           "min": [0, 0, 0], "max": [1, 1, 0]}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "nodes": [{"mesh": 0, "name": "A"}, {"mesh": 0, "name": "B"}],
            "scenes": [{"nodes": [0, 1]}], "scene": 0})";
    }

}

TEST(LatticeBakeGltf, NodesOfASharedMeshGetTheirOwnDataInNewAccessors){
    TempDir dir;
    const auto source = dir.write("shared.gltf", sharedMeshGltf());
    LatticeBakeGltf gltf;
    std::string error;
    ASSERT_TRUE(gltf.load(source, error)) << error;
    const LatticeBakeGltf::Slot position{0, -1, "POSITION"};
    // different data for each node: the mesh is copied, and the shared accessor gets one new
    // accessor per node (the second one used to read the first's freed list entry)
    gltf.setAttribute(0, position, {0, 0, 0, 2, 0, 0, 0, 2, 0}, 3);
    gltf.setAttribute(1, position, {0, 0, 0, 3, 0, 0, 0, 3, 0}, 3);
    const auto baked = dir.file("baked.glb");
    ASSERT_TRUE(gltf.saveBinary(baked, error)) << error;

    LatticeBakeGltf out;
    ASSERT_TRUE(out.load(baked, error)) << error;
    const auto& json = out.getJson();
    ASSERT_EQ(json["meshes"].size(), 2u);
    const size_t meshA = json["nodes"][0]["mesh"].get<size_t>();
    const size_t meshB = json["nodes"][1]["mesh"].get<size_t>();
    ASSERT_NE(meshA, meshB);
    for (const auto& [mesh, scale] : {std::pair<size_t, float>{meshA, 2.0f}, {meshB, 3.0f}}){
        const int accessor = json["meshes"][mesh]["primitives"][0]["attributes"]["POSITION"].get<int>();
        EXPECT_NE(accessor, 0) << "a new accessor, the old one left as it was";
        EXPECT_EQ(json["accessors"][(size_t)accessor].value("name", std::string()), "pos") << "the new accessor keeps the name";
        std::vector<float> values;
        int components = 0;
        ASSERT_TRUE(out.readAccessor(accessor, values, components, error)) << error;
        ASSERT_EQ(components, 3);
        EXPECT_EQ(values, (std::vector<float>{0, 0, 0, scale, 0, 0, 0, scale, 0}));
    }
}
