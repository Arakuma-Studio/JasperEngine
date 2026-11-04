// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The glTF normalise step (docs/specs/S53-model-import-cook.md R1, R2; AC1, AC2) on real files:
// fixtures/gltf/s41 holds one source model and what gltfpack 1.3 (-c, -cc, -kn, -vpf, -noq, -cz
// -ce khr, -cf, a .gltf, quantization alone) and gltf-transform (quantize + meshopt) make of it
// (README.md there says how). Each parses to the triangles, normals, UVs and colours of the
// source, within the quantization step, and loads as a mesh the renderer binds without an
// error. Corrupt and truncated meshopt streams are errors that name the file and the bufferView,
// never a crash (the sanitizer builds run these too). CPU only.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include "GltfTriangleSoup.h"
#include "LogCapture.h"
#include "TempDir.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    bool parse(const std::string& path, tinygltf::Model& model, std::string& error){
        model = tinygltf::Model();
        return MeshSystem::parseGLTFFile(path, model, &error);
    }

    // the vertex formats MeshSystem's attributes bind (SokolObject::getVertexFormat), and that
    // positions are floats (the CPU reads them: bounds, picking, LODs)
    std::string unbindable(const MeshComponent& mesh){
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            for (const auto& [type, attribute] : mesh.submeshes[s].attributes){
                const unsigned int elements = attribute.getElements();
                const AttributeDataType data = attribute.getDataType();
                bool ok = false;
                if (type == AttributeType::INDEX)
                    ok = data == AttributeDataType::UNSIGNED_SHORT || data == AttributeDataType::UNSIGNED_INT;
                else if (type == AttributeType::POSITION)
                    ok = data == AttributeDataType::FLOAT && elements == 3;
                else if (data == AttributeDataType::FLOAT)
                    ok = elements >= 1 && elements <= 4;
                else if (data == AttributeDataType::BYTE || data == AttributeDataType::UNSIGNED_BYTE)
                    ok = elements == 4;
                else if (data == AttributeDataType::SHORT)
                    ok = (elements == 2 || elements == 4);
                else if (data == AttributeDataType::UNSIGNED_SHORT)
                    ok = (elements == 2 || elements == 4) && (attribute.getNormalized() || type == AttributeType::BONEIDS || type == AttributeType::BONEWEIGHTS);
                if (!ok)
                    return "submesh " + std::to_string(s) + " attribute " + std::to_string((int)type) + ": " +
                           std::to_string(elements) + " elements of type " + std::to_string((int)data);
            }
        }
        return std::string();
    }

    struct Variant {
        const char* file;
        double position;  // metres: the quantization step of the variant
        double uv;
    };

    std::ostream& operator<<(std::ostream& out, const Variant& variant){
        return out << variant.file;
    }

    // gltfpack's defaults: 14-bit positions over the scene (~3.5 m: 0.2 mm), 12-bit UVs over their
    // range (2: 0.5e-3), 8-bit normals
    const Variant variants[] = {
        {"meshopt_c.glb", 2e-3, 2e-3},
        {"meshopt_cc.glb", 2e-3, 2e-3},
        {"meshopt_kn.glb", 2e-3, 2e-3},
        {"meshopt_vpf.glb", 2e-3, 2e-3},
        {"meshopt_noq.glb", 1e-5, 1e-5},
        {"meshopt_khr.glb", 2e-3, 2e-3},
        {"meshopt_cf.glb", 2e-3, 2e-3},
        {"meshopt_c.gltf", 2e-3, 2e-3},
        {"quantized.glb", 2e-3, 2e-3},
        {"gt_meshopt.glb", 2e-3, 2e-3},
    };

    class GltfNormalizeTest : public ::testing::TestWithParam<Variant> {};

}

TEST_P(GltfNormalizeTest, SameTrianglesAsTheSource){
    tinygltf::Model source, packed;
    std::string error;
    ASSERT_TRUE(parse(s41Fixture("source.glb"), source, error)) << error;
    ASSERT_TRUE(parse(s41Fixture(GetParam().file), packed, error)) << GetParam().file << ": " << error;
    SoupTolerance tolerance;
    tolerance.position = GetParam().position;
    tolerance.uv = GetParam().uv;
    const std::vector<SoupTriangle> expected = triangleSoup(source);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(compareSoups(expected, triangleSoup(packed), tolerance), "") << GetParam().file;
}

TEST_P(GltfNormalizeTest, LoadsAsAMeshTheRendererBinds){
    Scene scene;
    LogCapture log;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(s41Fixture(GetParam().file))) << GetParam().file;
    EXPECT_TRUE(log.errors().empty()) << GetParam().file << ":\n" << log.describeErrors();
    const ModelComponent& component = scene.getComponent<ModelComponent>(model.getEntity());
    std::vector<Entity> meshes = {model.getEntity()};
    for (const auto& [node, entity] : component.meshNodesMapping)
        meshes.push_back(entity);
    unsigned int submeshes = 0;
    for (Entity entity : meshes){
        const MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        submeshes += mesh.numSubmeshes;
        EXPECT_EQ(unbindable(mesh), "") << GetParam().file;
    }
    EXPECT_EQ(submeshes, 2u) << GetParam().file;
}

INSTANTIATE_TEST_SUITE_P(Fixtures, GltfNormalizeTest, ::testing::ValuesIn(variants));

namespace {

    // a GLB's chunks, to write broken copies of a fixture
    struct Glb {
        nlohmann::json json;
        std::string bin;
    };

    Glb readGlb(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Glb glb;
        uint32_t jsonLength = 0;
        std::memcpy(&jsonLength, bytes.data() + 12, 4);
        glb.json = nlohmann::json::parse(bytes.substr(20, jsonLength));
        const size_t binHeader = 20 + jsonLength;
        uint32_t binLength = 0;
        std::memcpy(&binLength, bytes.data() + binHeader, 4);
        glb.bin = bytes.substr(binHeader + 8, binLength);
        return glb;
    }

    std::string writeGlb(const TempDir& dir, const std::string& name, const Glb& glb){
        std::string json = glb.json.dump();
        while (json.size() % 4)
            json += ' ';
        std::string bin = glb.bin;
        while (bin.size() % 4)
            bin += '\0';
        auto u32 = [](uint32_t value){ return std::string(reinterpret_cast<const char*>(&value), 4); };
        const uint32_t total = 12 + 8 + (uint32_t)json.size() + 8 + (uint32_t)bin.size();
        std::string out = u32(0x46546C67) + u32(2) + u32(total) + u32((uint32_t)json.size()) + u32(0x4E4F534A) + json +
                          u32((uint32_t)bin.size()) + u32(0x004E4942) + bin;
        return dir.write(name, out).generic_string();
    }

    // the index of the first bufferView compressed in `mode`
    int compressedView(const Glb& glb, const std::string& mode){
        const nlohmann::json& views = glb.json["bufferViews"];
        for (size_t v = 0; v < views.size(); v++){
            if (views[v].contains("extensions") && views[v]["extensions"]["EXT_meshopt_compression"]["mode"] == mode)
                return (int)v;
        }
        return -1;
    }

    nlohmann::json& meshopt(Glb& glb, int view){
        return glb.json["bufferViews"][view]["extensions"]["EXT_meshopt_compression"];
    }

    // a broken copy must fail to parse, naming the file and `part`
    void expectRefused(const TempDir& dir, const std::string& name, const Glb& glb, const std::string& part){
        const std::string path = writeGlb(dir, name, glb);
        tinygltf::Model model;
        std::string error;
        EXPECT_FALSE(parse(path, model, error)) << name;
        EXPECT_NE(error.find(name), std::string::npos) << name << ": " << error;
        EXPECT_NE(error.find(part), std::string::npos) << name << ": " << error;
    }

}

TEST(GltfNormalizeTest, MalformedTruncatedStream){
    TempDir dir;
    Glb glb = readGlb(s41Fixture("meshopt_c.glb"));
    const int view = compressedView(glb, "ATTRIBUTES");
    ASSERT_GE(view, 0);
    // the stream ends early: its tail block is gone
    meshopt(glb, view)["byteLength"] = meshopt(glb, view)["byteLength"].get<int>() / 2;
    expectRefused(dir, "truncated.glb", glb, "bufferView " + std::to_string(view));
}

TEST(GltfNormalizeTest, MalformedIndicesPastTheVertices){
    TempDir dir;
    Glb glb = readGlb(s41Fixture("meshopt_c.glb"));
    const int view = compressedView(glb, "TRIANGLES");
    ASSERT_GE(view, 0);
    // the index codec decodes any bytes to some indices: the ones past the primitive's
    // vertices (here, fewer vertices than the stream names) are refused, not drawn
    const int position = glb.json["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<int>();
    glb.json["accessors"][position]["count"] = 10;
    expectRefused(dir, "indices_past.glb", glb, "bufferView " + std::to_string(view));
}

TEST(GltfNormalizeTest, MalformedStrides){
    TempDir dir;
    const Glb original = readGlb(s41Fixture("meshopt_c.glb"));
    const int attributes = compressedView(original, "ATTRIBUTES");
    const int triangles = compressedView(original, "TRIANGLES");
    ASSERT_GE(attributes, 0);
    ASSERT_GE(triangles, 0);
    const std::string attributePart = "bufferView " + std::to_string(attributes);
    {
        Glb glb = original;
        meshopt(glb, attributes)["byteStride"] = 6; // the decoder asserts a multiple of 4
        expectRefused(dir, "stride6.glb", glb, attributePart);
    }
    {
        Glb glb = original;
        meshopt(glb, attributes)["byteStride"] = 260; // past 256: the decoder's stack buffer
        expectRefused(dir, "stride260.glb", glb, attributePart);
    }
    {
        Glb glb = original;
        meshopt(glb, triangles)["byteStride"] = 3;
        expectRefused(dir, "index_stride3.glb", glb, "bufferView " + std::to_string(triangles));
    }
    {
        Glb glb = original;
        meshopt(glb, triangles)["count"] = meshopt(glb, triangles)["count"].get<int>() - 1; // not whole triangles
        expectRefused(dir, "triangles_count.glb", glb, "bufferView " + std::to_string(triangles));
    }
    {
        Glb glb = original;
        meshopt(glb, attributes)["filter"] = "QUATERNION"; // needs a stride of 8
        meshopt(glb, attributes)["byteStride"] = 4;
        glb.json["bufferViews"][attributes]["byteLength"] = 4 * meshopt(glb, attributes)["count"].get<int>();
        expectRefused(dir, "quaternion4.glb", glb, attributePart);
    }
    {
        Glb glb = original;
        meshopt(glb, attributes)["mode"] = "STRIPS";
        expectRefused(dir, "mode.glb", glb, attributePart);
    }
}

TEST(GltfNormalizeTest, MalformedSizes){
    TempDir dir;
    const Glb original = readGlb(s41Fixture("meshopt_c.glb"));
    const int view = compressedView(original, "ATTRIBUTES");
    ASSERT_GE(view, 0);
    const std::string part = "bufferView " + std::to_string(view);
    {
        // the count disagrees with the view it fills
        Glb glb = original;
        meshopt(glb, view)["count"] = meshopt(glb, view)["count"].get<int>() + 1;
        expectRefused(dir, "count.glb", glb, part);
    }
    {
        // the compressed bytes run past their buffer
        Glb glb = original;
        meshopt(glb, view)["byteOffset"] = glb.bin.size() - 4;
        expectRefused(dir, "past_buffer.glb", glb, part);
    }
    {
        // a count that asks for a gigabyte: refused before any allocation
        Glb glb = original;
        const size_t stride = meshopt(glb, view)["byteStride"].get<size_t>();
        const size_t count = (size_t(1) << 30) / stride;
        meshopt(glb, view)["count"] = count;
        glb.json["bufferViews"][view]["byteLength"] = count * stride;
        glb.json["buffers"][1]["byteLength"] = count * stride * 2;
        expectRefused(dir, "huge.glb", glb, "MB");
    }
    {
        // the fallback buffer claims the BIN chunk: tinygltf used to copy it in
        Glb glb = original;
        glb.json["buffers"][1].erase("extensions");
        const std::string path = writeGlb(dir, "fallback_bin.glb", glb);
        tinygltf::Model model;
        std::string error;
        EXPECT_FALSE(parse(path, model, error));
        EXPECT_NE(error.find("BIN chunk"), std::string::npos) << error;
    }
}

// random byte damage to the compressed streams and random JSON values: every run parses or
// refuses, naming the file, and never reads or writes past a buffer (ASan builds)
TEST(GltfNormalizeTest, MalformedFuzz){
    TempDir dir;
    const Glb original = readGlb(s41Fixture("meshopt_c.glb"));
    std::mt19937 random(41);
    const char* const keys[] = {"byteOffset", "byteLength", "byteStride", "count"};
    int refused = 0;
    for (int run = 0; run < 200; run++){
        Glb glb = original;
        if (run % 2 == 0){
            // flip bytes in the compressed region (after the image: buffer view 0 is the PNG)
            const size_t start = glb.json["bufferViews"][0]["byteLength"].get<size_t>();
            std::uniform_int_distribution<size_t> at(start, glb.bin.size() - 1);
            for (int flips = 0; flips < 1 + run % 7; flips++)
                glb.bin[at(random)] ^= (char)(1 + random() % 255);
        }else{
            const int views = (int)glb.json["bufferViews"].size();
            const int view = 1 + (int)(random() % (views - 1));
            const char* key = keys[random() % 4];
            const int64_t values[] = {0, 1, 3, 4, 7, 255, 256, 257, 65536, (int64_t)1 << 32, -1};
            meshopt(glb, view)[key] = values[random() % 11];
        }
        const std::string name = "fuzz" + std::to_string(run) + ".glb";
        const std::string path = writeGlb(dir, name, glb);
        tinygltf::Model model;
        std::string error;
        if (!parse(path, model, error)){
            refused++;
            EXPECT_FALSE(error.empty()) << name;
        }
    }
    // most damage is caught (a flip in an unused tail can decode to other values)
    EXPECT_GT(refused, 50);
}
