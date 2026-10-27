// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// VatAssetWriter (vertex animation spec 2.1-2.3): encodeVatFiles turns a bake result into the
// canonical .vat text and a glTF 2.0 .vat.glb: the payload and socket tracks read back through
// vat::readVatGlb, the JSON reads back unchanged, tinygltf reads the glb (attributes, _VAT_INDEX,
// index counts per primitive, materials with relative texture URIs, the extension used but not
// required), the engine's MeshSystem::loadGLTF loads it, and every bad input is refused.

#include <gtest/gtest.h>

#include "vat/VatAssetWriter.h"

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace jasperengine;
namespace fs = std::filesystem;
namespace ev = jasperengine::editor::vat;

namespace {

    class FileSystemOnly : public System {
    public:
        int getScreenWidth() override { return 1000; }
        int getScreenHeight() override { return 480; }
    };

    fs::path testDir(){
        const fs::path dir = fs::temp_directory_path() / "jasperengine-editor-tests-vat";
        fs::create_directories(dir / "assets" / "vat");
        return dir;
    }

    // A vertices bake: a quad (4 vertices) as two submeshes, one textured material and one
    // default; 2 samples x 4 vertices x 2 texels = 16 half texels in 8 x 2; one socket
    struct Bake{
        std::string vatPath;
        vat::VatData data;
        ev::VatMeshData mesh;
        vat::VatPayload payload;
        vat::VatSocketTracks sockets;
    };

    Bake makeBake(){
        Bake bake;
        bake.vatPath = (testDir() / "assets" / "vat" / "quad.vat").generic_string();
        vat::VatData& data = bake.data;
        data.guid = "00112233445566778899aabbccddeeff";
        data.mode = vat::VatMode::Vertices;
        data.layout.precision = vat::VatPrecision::Half;
        data.layout.width = 8;
        data.layout.height = 2;
        data.layout.samples = 2;
        data.layout.vertices = 4;
        data.layout.normals = true;
        vat::VatClip clip;
        clip.name = "Flap";
        clip.frames = 2;
        clip.fps = 2.0f;
        data.clips.push_back(clip);
        data.bounds.extend(Vector3(0, 0, 0));
        data.bounds.extend(Vector3(1, 1, 0.5f));
        data.sockets.push_back({"Tip", "Quad/Tip"});
        data.recipe.sockets = {"Quad/Tip"};
        vat::VatRecipeSource source;
        source.kind = "model";
        source.path = "models/quad.gltf";
        source.clips = {"Flap"};
        data.recipe.sources.push_back(source);

        ev::VatMeshData& mesh = bake.mesh;
        mesh.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        mesh.tangents = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
        mesh.uv0 = {0, 0, 1, 0, 1, 1, 0, 1};
        mesh.colors = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
        mesh.vatIndex = {0, 1, 2, 3};
        mesh.submeshes.push_back({{0, 1, 2}, 0});
        mesh.submeshes.push_back({{0, 2, 3}, -1});
        ev::VatMaterialData material;
        material.name = "Skin";
        material.baseColor[0] = 0.5f;
        material.metallic = 0.25f;
        material.roughness = 0.75f;
        material.alphaMode = "MASK";
        material.alphaCutoff = 0.3f;
        material.doubleSided = true;
        material.baseColorTexture = (testDir() / "assets" / "textures" / "skin color.png").generic_string();
        material.normalTexture = (testDir() / "assets" / "vat" / "normal.png").generic_string();
        material.emissiveTexture = material.baseColorTexture;
        mesh.materials.push_back(material);

        bake.payload.precision = vat::VatPrecision::Half;
        bake.payload.width = 8;
        bake.payload.height = 2;
        bake.payload.bytes.resize(8 * 2 * 8);
        for (size_t i = 0; i < bake.payload.bytes.size(); i++) bake.payload.bytes[i] = (uint8_t)(i * 7);

        bake.sockets.sockets = 1;
        bake.sockets.frames = 2;
        bake.sockets.data = {0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 0, 0.7071068f, 0, 0.7071068f};
        return bake;
    }

    bool encode(Bake& bake, ev::VatEncodedFiles& out, std::string* error){
        return ev::encodeVatFiles(bake.vatPath, bake.data, bake.mesh, bake.payload, bake.sockets, out, error);
    }

    // one edit of the sample bake must be refused with `message` in the error
    void expectRefused(const std::function<void(Bake&)>& edit, const std::string& message){
        Bake bake = makeBake();
        edit(bake);
        ev::VatEncodedFiles out;
        std::string error;
        EXPECT_FALSE(encode(bake, out, &error)) << message;
        EXPECT_NE(error.find(message), std::string::npos) << "got: " << error;
        EXPECT_TRUE(out.glbBytes.empty());
    }

    // accessor `index` as floats (FLOAT or UNSIGNED_SHORT/INT components)
    std::vector<double> accessorValues(const tinygltf::Model& model, int index){
        const tinygltf::Accessor& accessor = model.accessors[index];
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const unsigned char* bytes = model.buffers[view.buffer].data.data() + view.byteOffset + accessor.byteOffset;
        size_t count = accessor.count * (size_t)tinygltf::GetNumComponentsInType(accessor.type);
        std::vector<double> out;
        for (size_t i = 0; i < count; i++){
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT){
                float v;
                std::memcpy(&v, bytes + 4 * i, 4);
                out.push_back(v);
            }else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT){
                uint32_t v;
                std::memcpy(&v, bytes + 4 * i, 4);
                out.push_back(v);
            }else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT){
                uint16_t v;
                std::memcpy(&v, bytes + 2 * i, 2);
                out.push_back(v);
            }
        }
        return out;
    }

    class VatAssetWriter : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            static FileSystemOnly* system = nullptr;
            if (!system) {
                system = new FileSystemOnly();
                Engine::systemInit(0, nullptr, system);
            }
        }
    };

}

TEST_F(VatAssetWriter, MeshPathFor){
    EXPECT_EQ(ev::meshPathFor("assets/vat/hero.vat"), "assets/vat/hero.vat.glb");
    EXPECT_EQ(ev::meshPathFor("/p/hero 2.vat"), "/p/hero 2.vat.glb");
    EXPECT_EQ(ev::meshPathFor("assets/hero.json"), "assets/hero.vat.glb");
}

TEST_F(VatAssetWriter, EncodesBothFiles){
    Bake bake = makeBake();
    ev::VatEncodedFiles out;
    std::string error;
    ASSERT_TRUE(encode(bake, out, &error)) << error;
    EXPECT_EQ(out.vatPath, bake.vatPath);
    EXPECT_EQ(out.meshPath, bake.vatPath + ".glb");
    // the generated fields were filled in
    EXPECT_EQ(bake.data.mesh, "quad.vat.glb");
    EXPECT_EQ(bake.data.layout.layoutHash, vat::computeLayoutHash(bake.data));
    EXPECT_EQ(bake.data.stats.bytes, 128u);
    EXPECT_EQ(out.vatText, vat::writeVat(bake.data));

    // the JSON reads back clean and unchanged
    vat::VatParseResult parsed = vat::parseVat(out.vatText);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(vat::writeVat(*parsed.data), out.vatText);

    // the payload and sockets read back through the engine's reader
    vat::VatGlbContents contents;
    ASSERT_TRUE(vat::readVatGlb(out.glbBytes, contents, &error)) << error;
    EXPECT_EQ(contents.layoutHash, bake.data.layout.layoutHash);
    EXPECT_EQ(contents.payload.precision, vat::VatPrecision::Half);
    EXPECT_EQ(contents.payload.width, 8);
    EXPECT_EQ(contents.payload.height, 2);
    EXPECT_EQ(contents.payload.bytes, bake.payload.bytes);
    EXPECT_EQ(contents.sockets.sockets, 1);
    EXPECT_EQ(contents.sockets.frames, 2);
    EXPECT_EQ(contents.sockets.data, bake.sockets.data);
    // and build the runtime asset
    EXPECT_TRUE(vat::VertexAnimation::create(*parsed.data, contents.payload, contents.sockets, &error)) << error;

    // deterministic
    Bake again = makeBake();
    ev::VatEncodedFiles second;
    ASSERT_TRUE(encode(again, second, &error)) << error;
    EXPECT_EQ(second.vatText, out.vatText);
    EXPECT_EQ(second.glbBytes, out.glbBytes);
}

TEST_F(VatAssetWriter, GlbIsValidGltf){
    Bake bake = makeBake();
    ev::VatEncodedFiles out;
    std::string error;
    ASSERT_TRUE(encode(bake, out, &error)) << error;

    // 4-byte aligned chunks
    ASSERT_GE(out.glbBytes.size(), 20u);
    uint32_t jsonLength;
    std::memcpy(&jsonLength, out.glbBytes.data() + 12, 4);
    EXPECT_EQ(jsonLength % 4, 0u);
    EXPECT_EQ(out.glbBytes.size() % 4, 0u);

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    ASSERT_TRUE(loader.LoadBinaryFromMemory(&model, &err, &warn, out.glbBytes.data(), (unsigned int)out.glbBytes.size(),
                                            fs::path(out.meshPath).parent_path().generic_string())) << err;
    EXPECT_TRUE(err.empty()) << err;

    // the extension: used, not required
    EXPECT_EQ(model.extensionsUsed, std::vector<std::string>{vat::VAT_GLTF_EXTENSION});
    EXPECT_TRUE(model.extensionsRequired.empty());
    ASSERT_EQ(model.extensions.count(vat::VAT_GLTF_EXTENSION), 1u);
    const tinygltf::Value& extension = model.extensions.at(vat::VAT_GLTF_EXTENSION);
    EXPECT_EQ(extension.Get("version").GetNumberAsInt(), 1);
    EXPECT_EQ(extension.Get("layoutHash").Get<std::string>(), bake.data.layout.layoutHash);
    EXPECT_EQ(extension.Get("payload").Get("format").Get<std::string>(), "half");
    EXPECT_EQ(extension.Get("payload").Get("width").GetNumberAsInt(), 8);
    EXPECT_EQ(extension.Get("sockets").Get("count").GetNumberAsInt(), 1);
    int payloadView = extension.Get("payload").Get("bufferView").GetNumberAsInt();
    ASSERT_GE(payloadView, 0);
    EXPECT_EQ(model.bufferViews[payloadView].target, 0);
    EXPECT_EQ(model.bufferViews[payloadView].byteLength, 128u);

    // one node with an identity transform, one mesh, no skin/animation/morph
    ASSERT_EQ(model.nodes.size(), 1u);
    EXPECT_EQ(model.nodes[0].mesh, 0);
    EXPECT_TRUE(model.nodes[0].matrix.empty());
    EXPECT_TRUE(model.nodes[0].translation.empty());
    EXPECT_TRUE(model.skins.empty());
    EXPECT_TRUE(model.animations.empty());
    ASSERT_EQ(model.scenes.size(), 1u);
    EXPECT_EQ(model.defaultScene, 0);
    ASSERT_EQ(model.meshes.size(), 1u);
    ASSERT_EQ(model.meshes[0].primitives.size(), 2u);

    for (size_t p = 0; p < 2; p++){
        const tinygltf::Primitive& primitive = model.meshes[0].primitives[p];
        EXPECT_TRUE(primitive.targets.empty());
        EXPECT_EQ(primitive.mode, TINYGLTF_MODE_TRIANGLES);
        std::vector<std::string> names;
        for (const auto& attribute : primitive.attributes) names.push_back(attribute.first);
        EXPECT_EQ(names, (std::vector<std::string>{"COLOR_0", "NORMAL", "POSITION", "TANGENT", "TEXCOORD_0", "_VAT_INDEX"}));
        // indices: 32-bit, one triangle each
        const tinygltf::Accessor& indices = model.accessors[primitive.indices];
        EXPECT_EQ(indices.componentType, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
        EXPECT_EQ(indices.count, 3u);
        EXPECT_EQ(model.bufferViews[indices.bufferView].target, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        std::vector<double> expected = p == 0 ? std::vector<double>{0, 1, 2} : std::vector<double>{0, 2, 3};
        EXPECT_EQ(accessorValues(model, primitive.indices), expected);
        for (const auto& attribute : primitive.attributes){
            const tinygltf::Accessor& accessor = model.accessors[attribute.second];
            EXPECT_EQ(accessor.count, 4u) << attribute.first;
            EXPECT_EQ(model.bufferViews[accessor.bufferView].target, TINYGLTF_TARGET_ARRAY_BUFFER) << attribute.first;
            EXPECT_EQ(model.bufferViews[accessor.bufferView].byteOffset % 4, 0u) << attribute.first;
        }
    }
    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    const tinygltf::Accessor& position = model.accessors[primitive.attributes.at("POSITION")];
    EXPECT_EQ(position.minValues, (std::vector<double>{0, 0, 0}));
    EXPECT_EQ(position.maxValues, (std::vector<double>{1, 1, 0}));
    EXPECT_EQ(accessorValues(model, primitive.attributes.at("POSITION")), (std::vector<double>{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}));
    const tinygltf::Accessor& vatIndex = model.accessors[primitive.attributes.at(vat::VAT_INDEX_ATTRIBUTE)];
    EXPECT_EQ(vatIndex.componentType, TINYGLTF_COMPONENT_TYPE_FLOAT);
    EXPECT_EQ(vatIndex.type, TINYGLTF_TYPE_SCALAR);
    EXPECT_EQ(accessorValues(model, primitive.attributes.at(vat::VAT_INDEX_ATTRIBUTE)), (std::vector<double>{0, 1, 2, 3}));
    EXPECT_EQ(model.accessors[primitive.attributes.at("TANGENT")].type, TINYGLTF_TYPE_VEC4);

    // materials: the given factors, the second primitive on the default material
    EXPECT_EQ(model.meshes[0].primitives[0].material, 0);
    EXPECT_EQ(model.meshes[0].primitives[1].material, -1);
    ASSERT_EQ(model.materials.size(), 1u);
    const tinygltf::Material& material = model.materials[0];
    EXPECT_EQ(material.name, "Skin");
    EXPECT_FLOAT_EQ((float)material.pbrMetallicRoughness.baseColorFactor[0], 0.5f);
    EXPECT_FLOAT_EQ((float)material.pbrMetallicRoughness.metallicFactor, 0.25f);
    EXPECT_FLOAT_EQ((float)material.pbrMetallicRoughness.roughnessFactor, 0.75f);
    EXPECT_EQ(material.alphaMode, "MASK");
    EXPECT_FLOAT_EQ((float)material.alphaCutoff, 0.3f);
    EXPECT_TRUE(material.doubleSided);
    // textures by URI relative to the glb, one image per file
    ASSERT_EQ(model.images.size(), 2u);
    ASSERT_GE(material.pbrMetallicRoughness.baseColorTexture.index, 0);
    const tinygltf::Image& color = model.images[model.textures[material.pbrMetallicRoughness.baseColorTexture.index].source];
    EXPECT_EQ(color.uri, "../textures/skin%20color.png");
    const tinygltf::Image& normal = model.images[model.textures[material.normalTexture.index].source];
    EXPECT_EQ(normal.uri, "normal.png");
    EXPECT_EQ(material.emissiveTexture.index, material.pbrMetallicRoughness.baseColorTexture.index);
    EXPECT_EQ(material.occlusionTexture.index, -1);
    EXPECT_EQ(material.pbrMetallicRoughness.metallicRoughnessTexture.index, -1);
}

TEST_F(VatAssetWriter, BonesModeKeepsTheSkinAttributes){
    Bake bake = makeBake();
    bake.data.mode = vat::VatMode::Bones;
    bake.data.layout.precision = vat::VatPrecision::Float;
    bake.data.layout.vertices = 0;
    bake.data.layout.bones = 2;
    bake.data.layout.normals = false;
    bake.payload.precision = vat::VatPrecision::Float;
    bake.payload.bytes.assign(8 * 2 * 16, 0);
    bake.mesh.joints = {0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    bake.mesh.weights = {0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0.25f, 0.75f, 0, 0};
    ev::VatEncodedFiles out;
    std::string error;
    ASSERT_TRUE(encode(bake, out, &error)) << error;

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    ASSERT_TRUE(loader.LoadBinaryFromMemory(&model, &err, &warn, out.glbBytes.data(), (unsigned int)out.glbBytes.size(), ""))
        << err;
    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    ASSERT_EQ(primitive.attributes.count("JOINTS_0"), 1u);
    ASSERT_EQ(primitive.attributes.count("WEIGHTS_0"), 1u);
    const tinygltf::Accessor& joints = model.accessors[primitive.attributes.at("JOINTS_0")];
    EXPECT_EQ(joints.componentType, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    EXPECT_EQ(joints.type, TINYGLTF_TYPE_VEC4);
    EXPECT_EQ(accessorValues(model, primitive.attributes.at("JOINTS_0"))[4], 1.0);
    EXPECT_EQ(accessorValues(model, primitive.attributes.at("WEIGHTS_0"))[13], 0.75);
    EXPECT_TRUE(model.skins.empty());
    vat::VatParseResult parsed = vat::parseVat(out.vatText);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    EXPECT_EQ(parsed.data->mode, vat::VatMode::Bones);
}

TEST_F(VatAssetWriter, TheEngineLoadsTheGlb){
    Bake bake = makeBake();
    bake.mesh.materials[0].baseColorTexture.clear();
    bake.mesh.materials[0].normalTexture.clear();
    bake.mesh.materials[0].emissiveTexture.clear();
    ev::VatEncodedFiles out;
    std::string error;
    ASSERT_TRUE(encode(bake, out, &error)) << error;
    {
        std::ofstream file(out.meshPath, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.glbBytes.data()), (std::streamsize)out.glbBytes.size());
    }
    Scene scene;
    Model model(&scene);
    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), out.meshPath));
    const MeshComponent& mesh = scene.getComponent<MeshComponent>(model.getEntity());
    EXPECT_EQ(mesh.numSubmeshes, 2u);
    EXPECT_NE(mesh.submeshes[0].attributes.find(AttributeType::POSITION), mesh.submeshes[0].attributes.end());
    EXPECT_NE(mesh.submeshes[0].attributes.find(AttributeType::INDEX), mesh.submeshes[0].attributes.end());
    EXPECT_NEAR(mesh.verticesAABB.getMaximum().x, 1.0f, 1e-5f);
    EXPECT_NEAR(mesh.verticesAABB.getMaximum().y, 1.0f, 1e-5f);
    std::error_code ignored;
    fs::remove(out.meshPath, ignored);
}

TEST_F(VatAssetWriter, RefusesInputsThatDisagree){
    using vat::VatMode;
    using vat::VatPrecision;
    expectRefused([](Bake& b){ b.vatPath = (testDir() / "quad.glb").generic_string(); }, "the output must be a .vat file");
    expectRefused([](Bake& b){ b.vatPath = (testDir() / ".vat").generic_string(); }, "the output must be a .vat file");
    // payload vs layout
    expectRefused([](Bake& b){ b.payload.precision = VatPrecision::Float; }, "the payload is 8 x 2 float, the layout says 8 x 2 half");
    expectRefused([](Bake& b){ b.payload.width = 4; }, "the layout says 8 x 2 half");
    expectRefused([](Bake& b){ b.payload.bytes.pop_back(); }, "the payload has 127 bytes");
    expectRefused([](Bake& b){
        b.data.layout.width = b.payload.width = 0;
    }, "the payload is empty");
    expectRefused([](Bake& b){ b.data.mode = VatMode::Bones; b.data.layout.bones = 2; }, "always float precision");
    expectRefused([](Bake& b){ b.data.layout.height = b.payload.height = 1; b.payload.bytes.resize(64); },
                  "generated field layout.height is invalid");
    // clips
    expectRefused([](Bake& b){ b.data.clips.clear(); }, "no clips");
    expectRefused([](Bake& b){
        for (int i = 0; i < vat::VAT_MAX_CLIPS; i++) b.data.clips.push_back(b.data.clips[0]);
    }, "at most 64");
    expectRefused([](Bake& b){ b.data.clips[0].frames = 3; }, "generated field clips[0].frames is invalid");
    expectRefused([](Bake& b){ b.data.clips[0].fps = 0; }, "generated field clips[0].fps is invalid");
    expectRefused([](Bake& b){ b.data.clips.push_back(b.data.clips[0]); }, "clips[1].name");
    expectRefused([](Bake& b){
        ClipEventData event;
        event.function = "not a function";
        b.data.clips[0].events.push_back(event);
    }, "clips[0].events[0].function");
    // mesh attributes
    expectRefused([](Bake& b){ b.mesh.positions.clear(); }, "mesh positions");
    expectRefused([](Bake& b){ b.mesh.positions.push_back(1); }, "mesh positions");
    expectRefused([](Bake& b){ b.mesh.normals.pop_back(); }, "mesh normals has 11 values");
    expectRefused([](Bake& b){ b.mesh.tangents.resize(12); }, "mesh tangents");
    expectRefused([](Bake& b){ b.mesh.uv0.resize(6); }, "mesh uv0");
    expectRefused([](Bake& b){ b.mesh.uv1.resize(2); }, "mesh uv1");
    expectRefused([](Bake& b){ b.mesh.colors.resize(3); }, "mesh colors");
    expectRefused([](Bake& b){ b.mesh.vatIndex.clear(); }, "mesh vatIndex");
    expectRefused([](Bake& b){ b.mesh.vatIndex[2] = 4; }, "vatIndex of vertex 2 is 4");
    expectRefused([](Bake& b){ b.mesh.vatIndex[1] = 0.5f; }, "vatIndex of vertex 1");
    expectRefused([](Bake& b){ b.mesh.vatIndex[0] = -1; }, "vatIndex of vertex 0");
    expectRefused([](Bake& b){ b.mesh.joints.assign(16, 0); }, "joints/weights are for bones mode");
    expectRefused([](Bake& b){ b.mesh.weights.assign(16, 0); }, "joints/weights are for bones mode");
    expectRefused([](Bake& b){
        b.data.mode = VatMode::Bones;
        b.data.layout.precision = b.payload.precision = VatPrecision::Float;
        b.payload.bytes.resize(256);
        b.data.layout.bones = 2;
    }, "mesh joints has 0 values");
    expectRefused([](Bake& b){
        b.data.mode = VatMode::Bones;
        b.data.layout.precision = b.payload.precision = VatPrecision::Float;
        b.payload.bytes.resize(256);
        b.data.layout.bones = 2;
        b.mesh.joints.assign(16, 0);
        b.mesh.joints[5] = 2;
        b.mesh.weights.assign(16, 0.25f);
    }, "mesh joint 2 (vertex 1) is past the 2 baked bones");
    // submeshes and materials
    expectRefused([](Bake& b){ b.mesh.submeshes.clear(); }, "no submeshes");
    expectRefused([](Bake& b){ b.mesh.submeshes[1].indices.clear(); }, "submesh 1 has 0 indices");
    expectRefused([](Bake& b){ b.mesh.submeshes[1].indices.push_back(0); }, "submesh 1 has 4 indices");
    expectRefused([](Bake& b){ b.mesh.submeshes[0].indices[1] = 4; }, "submesh 0 index 4 is past the 4 vertices");
    expectRefused([](Bake& b){ b.mesh.submeshes[0].material = 1; }, "submesh 0 material 1 doesn't exist");
    expectRefused([](Bake& b){ b.mesh.submeshes[0].material = -2; }, "submesh 0 material -2");
    expectRefused([](Bake& b){ b.mesh.materials[0].alphaMode = "CUTOUT"; }, "alphaMode \"CUTOUT\"");
    // sockets
    expectRefused([](Bake& b){ b.sockets.data.pop_back(); }, "the socket tracks have 13 floats");
    expectRefused([](Bake& b){ b.sockets.frames = 1; b.sockets.data.resize(7); }, "the socket tracks have 1 frames");
    expectRefused([](Bake& b){ b.data.recipe.sockets.push_back("Quad/Root"); }, "recipe.sockets lists 2 sockets");
    expectRefused([](Bake& b){
        b.data.recipe.sockets.clear();
        b.data.sockets.push_back({"Other", "Quad/Other"});
    }, "2 sockets, the tracks have 1");
    expectRefused([](Bake& b){ b.data.sockets[0].path = "Quad/Elsewhere"; }, "sockets[0].path");
    // a null error pointer is fine
    Bake bake = makeBake();
    bake.mesh.positions.clear();
    ev::VatEncodedFiles out;
    EXPECT_FALSE(encode(bake, out, nullptr));
}
