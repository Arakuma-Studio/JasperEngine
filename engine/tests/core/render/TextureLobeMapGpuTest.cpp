// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An extended lobe's map (MaterialPbr's clear coat, sheen, specular, anisotropy, iridescence,
// transmission) is sampled by the mesh's UVs, whatever else the material has. The variant asked
// for UVs for the five core maps alone (RenderSystem::checkPBRTextures): a material whose only
// image is a lobe's drew without UVs, and includes/pbr_ext_slots.glsl declares no map slot without
// them, so the map was never bound and the lobe's factors drew alone (BF17: SpecularTest's
// M2_SpecTex and M4_whiteTex, ClearCoatTest's three coated rows). Each case draws a plane whose map
// has a half that changes the lobe and one that does not: with a white base colour image, which
// gives the variant its UVs and changes nothing (the reference), and with the lobe's map alone,
// which must draw the same. glcore and Vulkan.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "object/environment/Skybox.h"
#include "pool/TextureDataPool.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"

#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    enum class Lobe { Clearcoat, ClearcoatNormal, Sheen, SpecularFactor, SpecularColor, Anisotropy, Iridescence, Transmission, Thickness };

    struct LobeCase {
        const char* name;
        Lobe lobe;
        // the map's left half (what changes the lobe) and right half (where it stays as its factors)
        uint8_t changed[4];
        uint8_t plain[4];
    };

    std::ostream& operator << (std::ostream& os, const LobeCase& c){
        return os << c.name;
    }

    std::vector<uint8_t> readTarget(){
        Framebuffer& target = GpuEnvironment::target();
        std::vector<uint8_t> rgba;
        readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
        return rgba;
    }

    // mean over the pixels of the largest channel difference (0..255)
    double meanDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size() || a.empty())
            return 255.0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += largest;
        }
        return sum / (double)pixels;
    }

    // an 8x8 image of two halves, made by a generator as a glTF image's pixels are made
    void registerSplitImage(const std::string& id, const uint8_t left[4], const uint8_t right[4]){
        const std::array<uint8_t, 4> l = {left[0], left[1], left[2], left[3]};
        const std::array<uint8_t, 4> r = {right[0], right[1], right[2], right[3]};
        TextureDataPool::setGenerator(id, [l, r]() -> TextureData {
            const int size = 8 * 8 * 4;
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
            if (!pixels)
                return TextureData();
            for (int y = 0; y < 8; y++){
                for (int x = 0; x < 8; x++){
                    const std::array<uint8_t, 4>& texel = x < 4 ? l : r;
                    std::copy(texel.begin(), texel.end(), pixels + (y * 8 + x) * 4);
                }
            }
            // not owned: the pool's copy owns the pixels
            return TextureData(8, 8, (unsigned int)size, ColorFormat::RGBA, 4, pixels);
        });
    }

    Texture namedTexture(const std::string& id){
        Texture texture;
        texture.setId(id);
        return texture;
    }

    // the lobe on, its map `id` (none when empty)
    Material lobeMaterial(Lobe lobe, const std::string& id){
        Material material;
        material.baseColorFactor = Vector4(0.5f, 0.5f, 0.55f, 1.0f);
        material.metallicFactor = 0.0f;
        material.roughnessFactor = 0.5f;
        const Texture map = id.empty() ? Texture() : namedTexture(id);
        MaterialPbrExtensions& pbr = material.pbr.edit();
        switch (lobe){
        case Lobe::Clearcoat:
            pbr.clearcoat.enabled = true;
            pbr.clearcoat.factor = 1.0f;
            pbr.clearcoat.roughness = 0.4f;
            pbr.clearcoat.map = map;
            break;
        case Lobe::ClearcoatNormal:
            pbr.clearcoat.enabled = true;
            pbr.clearcoat.factor = 1.0f;
            pbr.clearcoat.roughness = 0.1f;
            pbr.clearcoat.normalMap = map;
            break;
        case Lobe::Sheen:
            material.baseColorFactor = Vector4(0.1f, 0.1f, 0.1f, 1.0f);
            pbr.sheen.enabled = true;
            pbr.sheen.color = Vector3(1.0f, 1.0f, 1.0f);
            pbr.sheen.roughness = 0.5f;
            pbr.sheen.map = map;
            break;
        case Lobe::SpecularFactor:
        case Lobe::SpecularColor:
            material.baseColorFactor = Vector4(0.1f, 0.1f, 0.1f, 1.0f);
            material.roughnessFactor = 0.25f;
            pbr.specular.enabled = true;
            pbr.specular.factor = 1.0f;
            pbr.specular.color = Vector3(1.0f, 1.0f, 1.0f);
            pbr.specular.map = map;
            break;
        case Lobe::Anisotropy:
            material.baseColorFactor = Vector4(0.1f, 0.1f, 0.1f, 1.0f);
            material.roughnessFactor = 0.3f;
            pbr.anisotropy.enabled = true;
            pbr.anisotropy.strength = 1.0f;
            pbr.anisotropy.map = map;
            break;
        case Lobe::Iridescence:
            material.baseColorFactor = Vector4(0.1f, 0.1f, 0.1f, 1.0f);
            material.roughnessFactor = 0.25f;
            pbr.iridescence.enabled = true;
            pbr.iridescence.factor = 1.0f;
            pbr.iridescence.ior = 1.8f;
            pbr.iridescence.thicknessNm = Vector2(300.0f, 300.0f);
            pbr.iridescence.map = map;
            break;
        case Lobe::Thickness:
            pbr.volume.enabled = true;
            pbr.volume.thickness = 1.0f;
            pbr.volume.attenuationDistance = 0.2f;
            pbr.volume.attenuationColor = Vector3(1.0f, 0.1f, 0.1f);
            [[fallthrough]];
        case Lobe::Transmission:
            pbr.transmission.enabled = true;
            pbr.transmission.factor = 1.0f;
            pbr.transmission.map = map;
            break;
        }
        return material;
    }

    // the white image that gives the reference its UVs and changes nothing of the base colour
    void addWhiteBaseColor(Material& material){
        static const uint8_t white[4] = {255, 255, 255, 255};
        registerSplitImage("bf17-white", white, white);
        material.baseColorTexture = namedTexture("bf17-white");
    }

    class TextureLobeMapGpu : public ::testing::TestWithParam<LobeCase> {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            // a bright background: what transmission shows
            scene.setBackgroundColor(0.75f, 0.8f, 0.3f, 1.0f);
            scene.setGlobalIllumination(0.5f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 2.2f, 2.6f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            // the sun's mirror image toward the camera: the highlight the lobes change
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(0.0f, -2.2f, 2.6f);
            sun.setIntensity(3.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
        }

        std::vector<uint8_t> draw(const Material& material){
            Primitive plane(&scene);
            plane.createPlane(3.0f, 3.0f);
            plane.setMaterial(material);
            Scene* current = &scene;
            EXPECT_TRUE(GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            })) << "the scene never finished loading";
            GpuEnvironment::drawFrames(3);
            return readTarget();
        }
    };

}

TEST_P(TextureLobeMapGpu, AMaterialWithOnlyALobesMapBindsIt){
    JASPER_REQUIRE_GPU();
    const LobeCase& c = GetParam();
    const std::string id = std::string("bf17-map|") + c.name;
    registerSplitImage(id, c.changed, c.plain);

    Material withWhite = lobeMaterial(c.lobe, id);
    addWhiteBaseColor(withWhite);
    const std::vector<uint8_t> reference = draw(withWhite);
    const std::vector<uint8_t> noMap = draw(lobeMaterial(c.lobe, ""));
    const std::vector<uint8_t> mapAlone = draw(lobeMaterial(c.lobe, id));

    // (a transmission on its own shows the background the map's half takes away)
    ASSERT_GT(meanDifference(reference, noMap), 0.5) << c.name << ": the map changes nothing (the case shows nothing)";
    EXPECT_LT(meanDifference(mapAlone, reference), 0.25) << c.name << ": the lobe's map alone does not draw as with a core map beside it";
    EXPECT_NO_ENGINE_ERRORS();
}

// An editor or a script gives a loaded mesh's material a lobe's map, then takes it away: the variant
// that takes it is built while the loaded one draws (RenderSystem::loadedVariantMatches), with its UVs and without
TEST_P(TextureLobeMapGpu, AMapGivenToALoadedMaterialAndTakenAwayIsDrawnThenGone){
    JASPER_REQUIRE_GPU();
    const LobeCase& c = GetParam();
    const std::string id = std::string("bf17-map-later|") + c.name;
    registerSplitImage(id, c.changed, c.plain);
    const std::vector<uint8_t> withMap = draw(lobeMaterial(c.lobe, id));
    const std::vector<uint8_t> noMap = draw(lobeMaterial(c.lobe, ""));
    ASSERT_GT(meanDifference(withMap, noMap), 0.5) << c.name << ": the map changes nothing (the case shows nothing)";

    Primitive plane(&scene);
    plane.createPlane(3.0f, 3.0f);
    plane.setMaterial(lobeMaterial(c.lobe, ""));
    Scene* current = &scene;
    const auto settledFrame = [current](){
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        GpuEnvironment::drawFrames(3);
        return readTarget();
    };
    settledFrame();

    Submesh& submesh = scene.getComponent<MeshComponent>(plane.getEntity()).submeshes[0];
    const auto setMap = [&](const Texture& map){
        MaterialPbrExtensions& pbr = submesh.material.pbr.edit();
        switch (c.lobe){
        case Lobe::Clearcoat: pbr.clearcoat.map = map; break;
        case Lobe::ClearcoatNormal: pbr.clearcoat.normalMap = map; break;
        case Lobe::Sheen: pbr.sheen.map = map; break;
        case Lobe::SpecularFactor:
        case Lobe::SpecularColor: pbr.specular.map = map; break;
        case Lobe::Anisotropy: pbr.anisotropy.map = map; break;
        case Lobe::Iridescence: pbr.iridescence.map = map; break;
        case Lobe::Thickness:
        case Lobe::Transmission: pbr.transmission.map = map; break;
        }
        submesh.needUpdateTexture = true;
    };
    setMap(namedTexture(id));
    EXPECT_LT(meanDifference(settledFrame(), withMap), 0.25) << c.name << ": the map given to the loaded material is not drawn";
    setMap(Texture());
    EXPECT_LT(meanDifference(settledFrame(), noMap), 0.25) << c.name << ": the map taken away still draws";
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(Lobes, TextureLobeMapGpu, ::testing::Values(
    // clear coat: R the factor, G the roughness
    LobeCase{"Clearcoat", Lobe::Clearcoat, {0, 255, 0, 255}, {255, 255, 0, 255}},
    // its own normal map: tilted, flat
    LobeCase{"ClearcoatNormal", Lobe::ClearcoatNormal, {230, 128, 180, 255}, {128, 128, 255, 255}},
    // sheen: RGB the colour, A the roughness
    LobeCase{"Sheen", Lobe::Sheen, {0, 0, 0, 255}, {255, 255, 255, 255}},
    // specular: RGB the colour, A the factor
    LobeCase{"SpecularFactor", Lobe::SpecularFactor, {255, 255, 255, 0}, {255, 255, 255, 255}},
    LobeCase{"SpecularColor", Lobe::SpecularColor, {255, 0, 0, 255}, {255, 255, 255, 255}},
    // anisotropy: RG the direction, B the strength
    LobeCase{"Anisotropy", Lobe::Anisotropy, {255, 128, 0, 255}, {255, 128, 255, 255}},
    // iridescence: R the factor, G the thickness between the lobe's minimum and maximum
    LobeCase{"Iridescence", Lobe::Iridescence, {0, 255, 0, 255}, {255, 255, 0, 255}},
    // transmission: R the factor, G the volume's thickness
    LobeCase{"Transmission", Lobe::Transmission, {0, 0, 0, 255}, {255, 0, 0, 255}},
    LobeCase{"Thickness", Lobe::Thickness, {255, 0, 0, 255}, {255, 255, 0, 255}}),
    [](const ::testing::TestParamInfo<LobeCase>& info){ return std::string(info.param.name); });

// With image based lighting alone (no light of the scene) the lobes draw too, and the variant their
// map asks UVs for samples every core map's slot, which must be bound
TEST(TextureLobeMapEnvironmentGpu, ALobesMapAloneIsBoundUnderImageBasedLightingAlone){
    JASPER_REQUIRE_GPU();
    const uint8_t noCoat[4] = {0, 255, 0, 255};
    const uint8_t coat[4] = {255, 255, 0, 255};
    registerSplitImage("bf17-map|ibl", noCoat, coat);

    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 2.2f, 2.6f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    // a cube map that lights the scene: grey faces, a bright one above
    TempDir dir;
    const auto face = [&dir](const char* name, uint8_t value){
        std::vector<uint8_t> pixels(16 * 16 * 4, value);
        for (size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = dir.file(std::string(name) + ".png").generic_string();
        EXPECT_TRUE(stbi_write_png(path.c_str(), 16, 16, 4, pixels.data(), 16 * 4));
        return path;
    };
    Skybox sky(&scene);
    sky.setTextures(face("px", 90), face("nx", 90), face("py", 255), face("ny", 40), face("pz", 90), face("nz", 90));
    Engine::setScene(&scene);

    Scene* current = &scene;
    const auto draw = [&](const Material& material){
        Primitive plane(&scene);
        plane.createPlane(3.0f, 3.0f);
        plane.setMaterial(material);
        scene.getComponent<MeshComponent>(plane.getEntity()).receiveIBL = true;
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        // (ShaderPool::getMeshProperties: 'Ibl')
        EXPECT_NE(scene.getComponent<MeshComponent>(plane.getEntity()).submeshes[0].shaderProperties & (1u << 19), 0u) << "the sky does not light the plane";
        GpuEnvironment::drawFrames(3);
        return readTarget();
    };

    Material withWhite = lobeMaterial(Lobe::Clearcoat, "bf17-map|ibl");
    addWhiteBaseColor(withWhite);
    const std::vector<uint8_t> reference = draw(withWhite);
    const std::vector<uint8_t> noMap = draw(lobeMaterial(Lobe::Clearcoat, ""));
    const std::vector<uint8_t> mapAlone = draw(lobeMaterial(Lobe::Clearcoat, "bf17-map|ibl"));

    // the white image changes nothing here either: the core maps' slots are bound under image
    // based lighting alone too (they were under a scene light only)
    Material plain;
    plain.baseColorFactor = Vector4(0.5f, 0.5f, 0.55f, 1.0f);
    plain.metallicFactor = 0.0f;
    plain.roughnessFactor = 0.5f;
    const std::vector<uint8_t> factors = draw(plain);
    addWhiteBaseColor(plain);
    const std::vector<uint8_t> withCoreMap = draw(plain);
    Engine::removeScene(&scene);
    EXPECT_LT(meanDifference(withCoreMap, factors), 0.25) << "a white base colour image changes the surface under image based lighting alone";

    ASSERT_GT(meanDifference(reference, noMap), 0.5) << "the map changes nothing (the case shows nothing)";
    EXPECT_LT(meanDifference(mapAlone, reference), 0.25) << "the lobe's map alone does not draw as with a core map beside it";
    EXPECT_NO_ENGINE_ERRORS();
}

// A core map as a material's only image, under image based lighting alone. The variant asked for UVs
// for the other core maps under a scene light only (RenderSystem::checkPBRTextures), and without UVs
// no map is sampled. Each case draws with a white base colour image beside the map (the reference)
// and with the map alone, which must draw the same.
namespace {
    struct CoreMapCase {
        const char* name;
        Texture Material::*map;
        // the map's left half (what changes the surface) and right half (where its factors stay)
        uint8_t changed[4];
        uint8_t plain[4];
    };

    std::ostream& operator << (std::ostream& os, const CoreMapCase& c){
        return os << c.name;
    }

    class CoreMapEnvironmentGpu : public ::testing::TestWithParam<CoreMapCase> {};
}

TEST_P(CoreMapEnvironmentGpu, ACoreMapAloneIsBoundUnderImageBasedLightingAlone){
    JASPER_REQUIRE_GPU();
    const CoreMapCase& c = GetParam();
    const std::string id = std::string("bf19-core|") + c.name;
    registerSplitImage(id, c.changed, c.plain);

    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 2.2f, 2.6f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();

    TempDir dir;
    const auto face = [&dir](const char* name, uint8_t value){
        std::vector<uint8_t> pixels(16 * 16 * 4, value);
        for (size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = dir.file(std::string(name) + ".png").generic_string();
        EXPECT_TRUE(stbi_write_png(path.c_str(), 16, 16, 4, pixels.data(), 16 * 4));
        return path;
    };
    Skybox sky(&scene);
    sky.setTextures(face("px", 90), face("nx", 90), face("py", 255), face("ny", 40), face("pz", 90), face("nz", 90));
    Engine::setScene(&scene);

    Scene* current = &scene;
    const auto draw = [&](const Material& material){
        Primitive plane(&scene);
        plane.createPlane(3.0f, 3.0f);
        plane.setMaterial(material);
        scene.getComponent<MeshComponent>(plane.getEntity()).receiveIBL = true;
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        // (ShaderPool::getMeshProperties: 'Ibl')
        EXPECT_NE(scene.getComponent<MeshComponent>(plane.getEntity()).submeshes[0].shaderProperties & (1u << 19), 0u) << "the sky does not light the plane";
        GpuEnvironment::drawFrames(3);
        return readTarget();
    };

    Material plain;
    plain.baseColorFactor = Vector4(0.6f, 0.6f, 0.65f, 1.0f);
    plain.metallicFactor = 1.0f;
    plain.roughnessFactor = 1.0f;
    if (c.map == &Material::emissiveTexture)
        plain.emissiveFactor = Vector3(0.5f, 0.5f, 0.5f);
    Material mapAlone = plain;
    mapAlone.*(c.map) = namedTexture(id);
    Material withWhite = mapAlone;
    addWhiteBaseColor(withWhite);

    const std::vector<uint8_t> noMap = draw(plain);
    const std::vector<uint8_t> reference = draw(withWhite);
    const std::vector<uint8_t> alone = draw(mapAlone);
    Engine::removeScene(&scene);

    ASSERT_GT(meanDifference(reference, noMap), 0.5) << "the map changes nothing (the case shows nothing)";
    EXPECT_LT(meanDifference(alone, reference), 0.25) << "the map alone does not draw as with a base colour image beside it";
    EXPECT_NO_ENGINE_ERRORS();
}

INSTANTIATE_TEST_SUITE_P(CoreMaps, CoreMapEnvironmentGpu, ::testing::Values(
    // (roughness in green, metallic in blue: the left half a smooth dielectric)
    CoreMapCase{"MetallicRoughness", &Material::metallicRoughnessTexture, {0, 30, 0, 255}, {255, 255, 255, 255}},
    CoreMapCase{"Occlusion", &Material::occlusionTexture, {0, 0, 0, 255}, {255, 255, 255, 255}},
    CoreMapCase{"Emissive", &Material::emissiveTexture, {0, 0, 0, 255}, {255, 255, 255, 255}},
    CoreMapCase{"Normal", &Material::normalTexture, {230, 128, 180, 255}, {128, 128, 255, 255}}),
    [](const ::testing::TestParamInfo<CoreMapCase>& info){ return std::string(info.param.name); });

namespace {
    // a quad facing up with two UV sets, the second one mirrored: a glTF of its own, its lobe map
    // an external image read by UV set `texCoord`
    std::string writeTwoSetQuad(TempDir& dir, const std::string& name, const std::string& materialJson){
        struct Vertex { float position[3]; float normal[3]; float uv0[2]; float uv1[2]; };
        const std::array<Vertex, 4> vertices = {{
            {{-1.5f, 0.0f, -1.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}},
            {{1.5f, 0.0f, -1.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}},
            {{1.5f, 0.0f, 1.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}},
            {{-1.5f, 0.0f, 1.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}},
        }};
        const std::array<uint16_t, 6> indices = {0, 2, 1, 0, 3, 2};
        const std::string bin = name + ".bin";
        {
            std::ofstream out(dir.file(bin), std::ios::binary);
            out.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices));
            out.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
        }
        const size_t stride = sizeof(Vertex);
        const std::string gltf = name + ".gltf";
        std::ofstream(dir.file(gltf)) << R"({
"asset": {"version": "2.0"},
"extensionsUsed": ["KHR_materials_clearcoat"],
"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "TEXCOORD_1": 3}, "indices": 4, "material": 0}]}],
"buffers": [{"uri": ")" << bin << R"(", "byteLength": )" << sizeof(vertices) + sizeof(indices) << R"(}],
"bufferViews": [
  {"buffer": 0, "byteOffset": 0, "byteLength": )" << sizeof(vertices) << R"(, "byteStride": )" << stride << R"(, "target": 34962},
  {"buffer": 0, "byteOffset": )" << sizeof(vertices) << R"(, "byteLength": )" << sizeof(indices) << R"(, "target": 34963}],
"accessors": [
  {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1.5, 0, -1.5], "max": [1.5, 0, 1.5]},
  {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 4, "type": "VEC3"},
  {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 4, "type": "VEC2"},
  {"bufferView": 0, "byteOffset": 32, "componentType": 5126, "count": 4, "type": "VEC2"},
  {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"}],
"images": [{"uri": "white.png"}, {"uri": "coat.png"}],
"textures": [{"source": 0}, {"source": 1}],
"materials": [)" << materialJson << R"(]
})";
        return dir.file(gltf).generic_string();
    }
}

// A glTF material whose only image is a clear coat map on the second UV set: it reads that set
TEST(TextureLobeMapGltfGpu, ALobesMapAloneOfAModelReadsItsUvSet){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    const auto png = [&dir](const char* name, const uint8_t left[4], const uint8_t right[4]){
        std::vector<uint8_t> pixels(8 * 8 * 4);
        for (int y = 0; y < 8; y++){
            for (int x = 0; x < 8; x++){
                const uint8_t* texel = x < 4 ? left : right;
                std::copy(texel, texel + 4, pixels.begin() + (y * 8 + x) * 4);
            }
        }
        EXPECT_TRUE(stbi_write_png(dir.file(name).generic_string().c_str(), 8, 8, 4, pixels.data(), 8 * 4));
    };
    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t coated[4] = {255, 255, 0, 255};
    const uint8_t plain[4] = {0, 255, 0, 255};
    png("white.png", white, white);
    png("coat.png", plain, coated);

    const std::string lobe = R"("extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.1, "clearcoatTexture": {"index": 1, "texCoord": 1}}})";
    const std::string factors = R"("baseColorFactor": [0.5, 0.5, 0.55, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.5)";
    const std::string alone = writeTwoSetQuad(dir, "alone", R"({"pbrMetallicRoughness": {)" + factors + "}, " + lobe + "}");
    const std::string withWhite = writeTwoSetQuad(dir, "with-white",
        R"({"pbrMetallicRoughness": {)" + factors + R"(, "baseColorTexture": {"index": 0}}, )" + lobe + "}");
    const std::string first = writeTwoSetQuad(dir, "first-set",
        R"({"pbrMetallicRoughness": {)" + factors + R"(, "baseColorTexture": {"index": 0}}, "extensions": {"KHR_materials_clearcoat": {"clearcoatFactor": 1.0, "clearcoatRoughnessFactor": 0.1, "clearcoatTexture": {"index": 1, "texCoord": 0}}}})");

    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 2.2f, 2.6f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(0.0f, -2.2f, 2.6f);
    sun.setIntensity(3.0f);
    sun.setShadows(false);
    scene.setGlobalIllumination(0.5f);
    Engine::setScene(&scene);

    Scene* current = &scene;
    const auto draw = [&](const std::string& file){
        Model model(&scene);
        EXPECT_TRUE(model.loadModel(file)) << file;
        EXPECT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << "the scene never finished loading";
        GpuEnvironment::drawFrames(3);
        return readTarget();
    };
    const std::vector<uint8_t> reference = draw(withWhite);
    const std::vector<uint8_t> firstSet = draw(first);
    const std::vector<uint8_t> mapAlone = draw(alone);
    Engine::removeScene(&scene);

    ASSERT_GT(meanDifference(reference, firstSet), 0.5) << "the UV sets draw alike (the case shows nothing)";
    EXPECT_LT(meanDifference(mapAlone, reference), 0.25) << "the lobe's map alone does not read its UV set";
    EXPECT_NO_ENGINE_ERRORS();
}

// The Khronos samples' materials with a lobe's map as their only image (a golden compares their
// pixels, test_materials_pbr): each draws with UVs, so it binds the map
TEST(TextureLobeMapKhronosGpu, TheSamplesWithOnlyALobesMapSampleIt){
    JASPER_REQUIRE_GPU();
    // the sample, and its materials with a lobe's map and no core map
    const std::map<std::string, int> samples = {{"SpecularTest", 3}, {"ClearCoatTest", 3}, {"TransmissionTest", 1}, {"AttenuationTest", 1}};
    for (const auto& [sample, lobeOnly] : samples){
        Scene scene;
        Camera camera(&scene);
        camera.activate();
        Light sun(&scene);
        sun.setType(LightType::DIRECTIONAL);
        sun.setShadows(false);
        Engine::setScene(&scene);
        Model model(&scene);
        const std::string file = std::string(JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/") + sample + "/glTF-Binary/" + sample + ".glb";
        ASSERT_TRUE(model.loadModel(file)) << file;
        Scene* current = &scene;
        ASSERT_TRUE(GpuEnvironment::runFrames([current](){
            return current->getSystem<RenderSystem>()->isAllLoaded();
        })) << sample;

        int found = 0, withoutUvs = 0;
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                const Submesh& submesh = mesh.submeshes[s];
                const Material& material = submesh.material;
                if (submesh.pbrMaps[0] == PbrMap::COUNT || !material.baseColorTexture.empty() || !material.metallicRoughnessTexture.empty() ||
                        !material.normalTexture.empty() || !material.occlusionTexture.empty() || !material.emissiveTexture.empty())
                    continue;
                found++;
                // 'Uv1' or 'Uv2' (ShaderPool::getMeshProperties): the slot its map binds to is there
                if (!(submesh.shaderProperties & ((1u << 1) | (1u << 2))))
                    withoutUvs++;
            }
        }
        EXPECT_GE(found, lobeOnly) << sample << ": its submeshes with a lobe's map alone";
        EXPECT_EQ(withoutUvs, 0) << sample << ": submeshes with a lobe's map alone draw without UVs (the map is not bound)";
        Engine::removeScene(&scene);
    }
    EXPECT_NO_ENGINE_ERRORS();
}
