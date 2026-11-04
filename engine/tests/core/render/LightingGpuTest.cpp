// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S57's lighting on the headless device (docs/specs/S57-forge-lighting.md AC1-AC5): screen-tile
// light lists, capsule shadows, contact shadows, weighted blended OIT and the Hair shading model,
// against goldens in engine/tests/core/golden/ (JASPER_TEST_UPDATE_GOLDEN=1 rewrites them on
// glcore; Vulkan matches the same images), and the passes each adds only while on.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/CapsuleShadowCaster.h"
#include "object/Light.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/LightingTiers.h"
#include "render/OrderIndependentTransparency.h"
#include "render/SystemRender.h"
#include "shader/RuntimeShaderCompiler.h"
#include "subsystem/RenderSystem.h"
#include "texture/MaterialPbr.h"

#include "GpuEnvironment.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class LightingGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};

        void SetUp() override{
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            LightingTiers::resetTier();
            OrderIndependentTransparency::setSupportOverride(nullptr);
            scene.setBackgroundColor(0.12f, 0.12f, 0.16f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 6.0f, 8.0f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            LightingTiers::setLightMode(LightMode::AUTOMATIC);
            LightingTiers::resetTier();
            OrderIndependentTransparency::setSupportOverride(nullptr);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        RenderSystem& renderSystem(){
            return *scene.getSystem<RenderSystem>();
        }

        std::vector<uint8_t> readTarget(){
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba);
            return rgba;
        }

        // the frame's pass labels (the S15 pass list)
        std::vector<std::string> passLabels(){
            GpuEnvironment::drawFrames(2);
            const PassList list = SystemRender::getPassList(SystemRender::FRAME_STATS_FRAME, false);
            std::vector<std::string> labels;
            for (const PassRecord& pass : list.passes)
                labels.push_back(pass.label);
            return labels;
        }

        static bool hasPass(const std::vector<std::string>& labels, const char* label){
            return std::find(labels.begin(), labels.end(), std::string(label)) != labels.end();
        }
    };

    // the largest channel difference, the mean of it, and the share of pixels over 24
    struct Difference{
        int maxDiff = 0;
        double meanDiff = 0.0;
        double badFraction = 0.0;
    };

    Difference differenceOf(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        Difference d;
        if (a.size() != b.size() || a.empty()){
            d.maxDiff = 255;
            d.meanDiff = 255.0;
            d.badFraction = 1.0;
            return d;
        }
        size_t bad = 0;
        double sum = 0.0;
        for (size_t i = 0; i + 3 < a.size(); i += 4){
            int largest = 0;
            for (int c = 0; c < 3; c++)
                largest = std::max(largest, std::abs((int)a[i + c] - (int)b[i + c]));
            d.maxDiff = std::max(d.maxDiff, largest);
            sum += largest;
            bad += largest > 24 ? 1 : 0;
        }
        const double pixels = (double)(a.size() / 4);
        d.meanDiff = sum / pixels;
        d.badFraction = (double)bad / pixels;
        return d;
    }

    // a grid of point lights low over a large grey plane, each its own colour
    void sixtyFourLights(Scene& scene, Primitive& plane, std::vector<std::unique_ptr<Light>>& lights, int count = 64){
        plane.createPlane(16.0f, 16.0f);
        plane.setColor(0.8f, 0.8f, 0.8f);
        const int side = (int)std::ceil(std::sqrt((float)count));
        for (int i = 0; i < count; i++){
            auto light = std::make_unique<Light>(&scene);
            light->setType(LightType::POINT);
            const float x = -7.0f + 14.0f * (float)(i % side) / (float)(side - 1);
            const float z = -7.0f + 14.0f * (float)(i / side) / (float)(side - 1);
            light->setPosition(x, 0.6f, z);
            light->setRange(2.2f);
            light->setIntensity(2.5f);
            light->setColor(0.3f + 0.7f * (float)((i * 37) % 11) / 10.0f, 0.3f + 0.7f * (float)((i * 53) % 7) / 6.0f,
                            0.3f + 0.7f * (float)((i * 71) % 5) / 4.0f);
            light->setShadows(false);
            lights.push_back(std::move(light));
        }
    }

}

// AC1: 64 lights on one plane, every one in view drawn by its tile; Per Object (8 a mesh) drops most
// into the plane's SH (the red control: its image is far from Tiled's)
TEST_F(LightingGpu, TiledDrawsSixtyFourLightsOnOnePlane){
    JASPER_REQUIRE_GPU();
    Primitive plane(&scene);
    std::vector<std::unique_ptr<Light>> lights;
    sixtyFourLights(scene, plane, lights);

    LightingTiers::setLightMode(LightMode::TILED);
    ASSERT_TRUE(render()) << "the scene never finished loading";
    EXPECT_GOLDEN("lighting_tiled_64_lights");
    const RenderSystem::LightingStats stats = renderSystem().getLightingStats();
    EXPECT_TRUE(stats.tiledLights);
    ASSERT_TRUE(stats.tileStatsValid);
    // the default camera (a vertical field of 0.75 rad) sees 60 of the spheres: the
    // four corner lights of the two rows nearest it lie outside its frustum
    EXPECT_EQ(stats.tiles.lights, 60);
    EXPECT_GT(stats.tiles.maxPerTile, 0);
    EXPECT_EQ(stats.tiles.droppedFromTiles, 0);
    const std::vector<uint8_t> tiled = readTarget();

    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderSystem().getLightingStats().tiledLights);
    const Difference perObject = differenceOf(tiled, readTarget());
    EXPECT_GT(perObject.badFraction, 0.05) << "Per Object drew the 64 lights as Tiled does (mean " << perObject.meanDiff << ")";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC1 and S57 6: a scene within the object cap lights the same in both modes (the tiles add the
// same lights in the same order)
TEST_F(LightingGpu, TiledMatchesPerObjectWithinTheCap){
    JASPER_REQUIRE_GPU();
    Primitive plane(&scene);
    std::vector<std::unique_ptr<Light>> lights;
    sixtyFourLights(scene, plane, lights, 4);
    Primitive cube(&scene);
    cube.createBox(1.0f, 1.0f, 1.0f);
    cube.setPosition(0.0f, 0.5f, 0.0f);
    cube.setColor(0.2f, 0.6f, 0.9f);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -1.0f, -0.6f);
    sun.setIntensity(0.6f);
    sun.setShadows(false);

    LightingTiers::setLightMode(LightMode::PER_OBJECT);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> perObject = readTarget();
    LightingTiers::setLightMode(LightMode::TILED);
    ASSERT_TRUE(render());
    ASSERT_TRUE(renderSystem().getLightingStats().tiledLights);
    const Difference difference = differenceOf(perObject, readTarget());
    EXPECT_LE(difference.maxDiff, 2) << "mean " << difference.meanDiff;
    EXPECT_NO_ENGINE_ERRORS();
}

// a scene of suns keeps the object lists: no texture, no job (the default scene draws as before)
TEST_F(LightingGpu, SunOnlySceneStaysPerObject){
    JASPER_REQUIRE_GPU();
    Primitive cube(&scene);
    cube.createBox(1.5f, 1.5f, 1.5f);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.4f, -1.0f, -0.6f);
    LightingTiers::setLightMode(LightMode::TILED);
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderSystem().getLightingStats().tiledLights);
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    // a ground plane casting into the cache, a sun with shadows, and a "character": a tall box
    // whose Capsule Shadow Caster has a torso and two legs on the GameObject itself
    struct CapsuleScene{
        Primitive ground;
        Primitive body;
        Light sun;

        explicit CapsuleScene(Scene& scene): ground(&scene), body(&scene), sun(&scene){
            ground.createPlane(12.0f, 12.0f);
            ground.setColor(0.85f, 0.85f, 0.85f);
            ground.setStaticShadowCaster(true);
            body.createBox(0.6f, 1.8f, 0.4f);
            body.setPosition(0.0f, 0.9f, 0.0f);
            body.setColor(0.8f, 0.4f, 0.2f);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.3f);
            sun.setIntensity(2.0f);
            sun.setShadows(true);

            scene.addComponent<CapsuleShadowCasterComponent>(body.getEntity(), CapsuleShadowCasterComponent());
            CapsuleShadowCaster caster(&scene, body.getEntity());
            caster.setCastShadows(CapsuleCastShadows::CAPSULES_ONLY);
            const int torso = caster.addCapsule("", 0.3f, 0.6f);
            caster.setCapsuleOffset(torso, Vector3(0.0f, 0.35f, 0.0f));
            for (float side : {-0.15f, 0.15f}){
                const int leg = caster.addCapsule("", 0.12f, 0.7f);
                caster.setCapsuleOffset(leg, Vector3(side, -0.45f, 0.0f));
            }
        }
    };
}

// AC2: the capsules' soft shadow on the ground, the body casting no shadow map
TEST_F(LightingGpu, CapsuleShadowsOfACapsulesOnlyCharacter){
    JASPER_REQUIRE_GPU();
    CapsuleScene setup(scene);
    scene.setCapsuleShadowsEnabled(true);
    scene.setCapsuleShadowSoftness(0.25f);
    ASSERT_TRUE(render());
    const RenderSystem::LightingStats stats = renderSystem().getLightingStats();
    EXPECT_TRUE(stats.capsuleShadows);
    EXPECT_EQ(stats.capsuleCasters, 1);
    EXPECT_EQ(stats.capsules, 3);
    EXPECT_GOLDEN("lighting_capsule_shadows");
    const std::vector<uint8_t> withCapsules = readTarget();

    // Intensity 0 keeps the variant and draws the ground lit, as with the feature off
    scene.setCapsuleShadowIntensity(0.0f);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> faded = readTarget();

    // off: the character has no shadow at all (Capsules Only), the ground is lit there. The
    // torso's soft shadow covers about 0.3% of this frame, its core much darker than the ground
    scene.setCapsuleShadowsEnabled(false);
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderSystem().getLightingStats().capsuleShadows);
    const std::vector<uint8_t> off = readTarget();
    const Difference shadow = differenceOf(withCapsules, off);
    EXPECT_GT(shadow.badFraction, 0.001);
    EXPECT_GT(shadow.maxDiff, 60);
    EXPECT_LE(differenceOf(faded, off).maxDiff, 2) << "Intensity 0 still darkened the ground";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC2: the static shadow cache stays valid while a Capsules Only character walks (no shadow
// pass); the same character casting its shadow map redraws every frame it moves
TEST_F(LightingGpu, CapsulesOnlyCharacterKeepsTheShadowCache){
    JASPER_REQUIRE_GPU();
    CapsuleScene setup(scene);
    scene.setCapsuleShadowsEnabled(true);
    ASSERT_TRUE(render());

    uint32_t passes = 0;
    for (int frame = 0; frame < 6; frame++){
        setup.body.setPosition(0.1f * (float)frame, 0.9f, 0.0f);
        GpuEnvironment::drawFrames(1);
        passes += renderSystem().getShadowStats().passes;
    }
    EXPECT_EQ(passes, 0u) << "a Capsules Only character's moves redrew the shadow maps";

    CapsuleShadowCaster(&scene, setup.body.getEntity()).setCastShadows(CapsuleCastShadows::ON);
    GpuEnvironment::drawFrames(2);
    passes = 0;
    for (int frame = 0; frame < 6; frame++){
        setup.body.setPosition(0.6f - 0.1f * (float)frame, 0.9f, 0.0f);
        GpuEnvironment::drawFrames(1);
        passes += renderSystem().getShadowStats().passes;
    }
    EXPECT_GT(passes, 0u) << "a shadow-casting character moved without a shadow pass";
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    // a box and a thin post on a plane, a low sun with shadows and contact shadows
    struct ContactScene{
        Primitive ground;
        Primitive box;
        Primitive post;
        Light sun;

        explicit ContactScene(Scene& scene): ground(&scene), box(&scene), post(&scene), sun(&scene){
            ground.createPlane(10.0f, 10.0f);
            ground.setColor(0.85f, 0.85f, 0.85f);
            box.createBox(1.0f, 0.4f, 1.0f);
            box.setPosition(-0.8f, 0.2f, 0.5f);
            box.setColor(0.3f, 0.5f, 0.8f);
            post.createBox(0.08f, 1.2f, 0.08f);
            post.setPosition(1.0f, 0.6f, 0.0f);
            post.setColor(0.8f, 0.7f, 0.3f);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.8f, -0.45f, -0.4f);
            sun.setIntensity(2.0f);
            sun.setShadows(true);
            LightComponent& light = scene.getComponent<LightComponent>(sun.getEntity());
            light.contactShadows = true;
            light.contactShadowLength = 0.4f;
            light.contactShadowThickness = 0.1f;
            light.contactShadowSampleCount = 16;
        }
    };
}

// AC3: full resolution on High, half on Medium, with their pass in the frame
TEST_F(LightingGpu, ContactShadowsHighAndMedium){
    JASPER_REQUIRE_GPU();
    ContactScene setup(scene);
    LightingTiers::setTier(LightingTier::HIGH);
    ASSERT_TRUE(render());
    RenderSystem::LightingStats stats = renderSystem().getLightingStats();
    ASSERT_TRUE(stats.contactShadows);
    EXPECT_EQ(stats.contactWidth, (int)GpuEnvironment::target().getWidth());
    EXPECT_TRUE(hasPass(passLabels(), "contact shadows"));
    EXPECT_GOLDEN("lighting_contact_shadows_high");

    LightingTiers::setTier(LightingTier::MEDIUM);
    ASSERT_TRUE(render());
    stats = renderSystem().getLightingStats();
    ASSERT_TRUE(stats.contactShadows);
    EXPECT_EQ(stats.contactWidth, ((int)GpuEnvironment::target().getWidth() + 1) / 2);
    EXPECT_GOLDEN("lighting_contact_shadows_medium");

    // the mask darkens what the shadow map misses: the light without them draws another frame
    LightingTiers::setTier(LightingTier::HIGH);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> withContact = readTarget();
    scene.getComponent<LightComponent>(setup.sun.getEntity()).contactShadows = false;
    ASSERT_TRUE(render());
    EXPECT_FALSE(renderSystem().getLightingStats().contactShadows);
    EXPECT_GT(differenceOf(withContact, readTarget()).maxDiff, 8) << "contact shadows changed nothing";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3: zero passes when off: the Low tier, or no light asking, draws no contact pass (and a
// scene without an Order Independent camera no OIT pass)
TEST_F(LightingGpu, OffFeaturesAddNoPass){
    JASPER_REQUIRE_GPU();
    ContactScene setup(scene);
    LightingTiers::setTier(LightingTier::LOW);
    ASSERT_TRUE(render());
    std::vector<std::string> labels = passLabels();
    EXPECT_FALSE(renderSystem().getLightingStats().contactShadows);
    EXPECT_FALSE(hasPass(labels, "contact shadows"));
    EXPECT_FALSE(hasPass(labels, "oit accumulation"));

    LightingTiers::setTier(LightingTier::HIGH);
    scene.getComponent<LightComponent>(setup.sun.getEntity()).contactShadows = false;
    ASSERT_TRUE(render());
    labels = passLabels();
    EXPECT_FALSE(hasPass(labels, "contact shadows"));
    EXPECT_FALSE(hasPass(labels, "oit accumulation"));
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {
    // three half-transparent coloured walls crossing in front of the camera, created in the
    // order given
    void threePlanes(Scene& scene, std::vector<std::unique_ptr<Primitive>>& planes, bool reversed){
        struct Wall{ float r, g, b, x, z, turn; };
        const Wall walls[3] = {
            {0.9f, 0.15f, 0.1f, -0.6f, 0.4f, 20.0f},
            {0.1f, 0.85f, 0.2f, 0.0f, 0.0f, -15.0f},
            {0.15f, 0.3f, 0.95f, 0.6f, -0.4f, 35.0f},
        };
        for (int k = 0; k < 3; k++){
            const Wall& wall = walls[reversed ? 2 - k : k];
            auto plane = std::make_unique<Primitive>(&scene);
            plane->createWall(2.4f, 2.0f);
            plane->setPosition(wall.x, 1.0f, wall.z);
            plane->setRotation(0.0f, wall.turn, 0.0f);
            Material material = plane->getMaterial();
            material.baseColorFactor = Vector4(wall.r, wall.g, wall.b, 0.55f);
            material.blendMode = MaterialBlendMode::ALPHA;
            material.cullMode = MaterialCullMode::OFF;
            material.lighting = MaterialLighting::NONE;
            plane->setMaterial(material);
            plane->setTransparent(true);
            planes.push_back(std::move(plane));
        }
    }
}

// AC4: the three planes give the same image in either order, through Order Independent
TEST_F(LightingGpu, OrderIndependentThreePlanesInBothOrders){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 1.5f, 5.0f);
    camera.setTarget(0.0f, 1.0f, 0.0f);
    scene.getComponent<CameraComponent>(camera.getEntity()).transparencyMode = TransparencyMode::ORDER_INDEPENDENT;
    Primitive ground(&scene);
    ground.createPlane(10.0f, 10.0f);
    ground.setColor(0.7f, 0.7f, 0.7f);
    ground.setReceiveLights(false);

    std::vector<std::unique_ptr<Primitive>> planes;
    threePlanes(scene, planes, false);
    ASSERT_TRUE(render());
    const RenderSystem::LightingStats stats = renderSystem().getLightingStats();
    EXPECT_TRUE(stats.oit);
    EXPECT_FALSE(stats.oitFallback);
    EXPECT_TRUE(hasPass(passLabels(), "oit accumulation"));
    EXPECT_GOLDEN("lighting_oit_three_planes");
    const std::vector<uint8_t> forward = readTarget();

    planes.clear();
    threePlanes(scene, planes, true);
    ASSERT_TRUE(render());
    const Difference difference = differenceOf(forward, readTarget());
    EXPECT_LE(difference.maxDiff, 2) << "the order of the planes changed the image (mean " << difference.meanDiff << ")";
    EXPECT_NO_ENGINE_ERRORS();
}

// S32 x S57: while the Order Independent programs (the resolve and the walls' OIT variant) are
// still building, the camera draws Sorted and the scene's load waits for them; once they are
// built Order Independent turns on. The builder holds them "building" for as long as the test
// says (the ShaderPool hook AsyncCreateTest uses), the other variants compile as usual.
TEST_F(LightingGpu, OrderIndependentWaitsForItsPrograms){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 1.5f, 5.0f);
    camera.setTarget(0.0f, 1.0f, 0.0f);
    std::vector<std::unique_ptr<Primitive>> planes;
    threePlanes(scene, planes, false);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> sorted = readTarget();

    std::atomic<bool> held{true};
    std::atomic<int> heldAsks{0};
    const uint64_t oitBit = ShaderPool::getLightingProperty(ShaderPool::LightingFeature::OIT);
    ShaderPool::setShaderBuilder([&held, &heldAsks, oitBit](ShaderKey key){
        const ShaderType type = ShaderPool::getShaderTypeFromKey(key);
        const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
        const bool oitProgram = (type == ShaderType::LIGHTING_PASS && (properties & 1u)) ||
                                (type == ShaderType::MESH && (properties & oitBit));
        if (oitProgram && held.load()){
            heldAsks.fetch_add(1);
            return ShaderBuildResult({}, ResourceLoadState::Loading);
        }
        return RuntimeShaderCompiler::request(key);
    });
    struct BuilderReset{ ~BuilderReset(){ ShaderPool::setShaderBuilder(nullptr); } } reset;

    scene.getComponent<CameraComponent>(camera.getEntity()).transparencyMode = TransparencyMode::ORDER_INDEPENDENT;
    GpuEnvironment::drawFrames(30);
    EXPECT_GT(heldAsks.load(), 0) << "the Order Independent programs were never asked for";
    EXPECT_TRUE(renderSystem().getLightingStats().oit);
    EXPECT_FALSE(renderSystem().isAllLoaded()) << "the scene counted as loaded with its OIT programs building";
    EXPECT_FALSE(hasPass(passLabels(), "oit accumulation"));
    const Difference whileBuilding = differenceOf(sorted, readTarget());
    EXPECT_LE(whileBuilding.maxDiff, 2) << "while its programs build the camera does not draw as Sorted does (mean "
                                        << whileBuilding.meanDiff << ")";

    held.store(false);
    ASSERT_TRUE(render()) << "the scene never finished loading once the OIT programs could build";
    EXPECT_TRUE(hasPass(passLabels(), "oit accumulation")) << "Order Independent never turned on once its programs were built";
    EXPECT_GT(differenceOf(sorted, readTarget()).maxDiff, 2) << "built, the camera still draws as Sorted does";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC4: a device without renderable half floats draws the camera Sorted, warning once
TEST_F(LightingGpu, OrderIndependentFallsBackToSorted){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 1.5f, 5.0f);
    camera.setTarget(0.0f, 1.0f, 0.0f);
    std::vector<std::unique_ptr<Primitive>> planes;
    threePlanes(scene, planes, false);
    ASSERT_TRUE(render());
    const std::vector<uint8_t> sorted = readTarget();

    const bool unsupported = false;
    OrderIndependentTransparency::setSupportOverride(&unsupported);
    scene.getComponent<CameraComponent>(camera.getEntity()).transparencyMode = TransparencyMode::ORDER_INDEPENDENT;
    ASSERT_TRUE(render());
    const RenderSystem::LightingStats stats = renderSystem().getLightingStats();
    EXPECT_FALSE(stats.oit);
    EXPECT_TRUE(stats.oitFallback);
    EXPECT_FALSE(hasPass(passLabels(), "oit accumulation"));
    EXPECT_LE(differenceOf(sorted, readTarget()).maxDiff, 1);
    EXPECT_NO_ENGINE_ERRORS();
}

// AC5: a hair card sphere: Kajiya-Kay's two highlights along the strands
TEST_F(LightingGpu, HairShadingModelSphere){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.5f, 3.0f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    Primitive sphere(&scene);
    sphere.createSphere(1.0f, 48, 32);
    Material material = sphere.getMaterial();
    material.baseColorFactor = Vector4(0.45f, 0.28f, 0.12f, 1.0f);
    material.roughnessFactor = 0.6f;
    material.metallicFactor = 0.0f;
    MaterialPbrExtensions& pbr = material.pbr.edit();
    MaterialPbr::setShadingModel(pbr, ShadingModel::HAIR);
    pbr.hair.primaryColor = Vector3(0.9f, 0.85f, 0.8f);
    pbr.hair.secondaryColor = Vector3(0.6f, 0.6f, 0.6f);
    pbr.hair.rotation = 1.5707963f;
    sphere.setMaterial(material);
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.3f, -0.6f, -1.0f);
    sun.setIntensity(2.0f);
    sun.setShadows(false);

    ASSERT_TRUE(render());
    EXPECT_GOLDEN("lighting_hair_sphere");
    const std::vector<uint8_t> hair = readTarget();

    MaterialPbr::setShadingModel(material.pbr.edit(), ShadingModel::STANDARD);
    sphere.setMaterial(material);
    ASSERT_TRUE(render());
    EXPECT_GT(differenceOf(hair, readTarget()).badFraction, 0.01) << "Hair shaded as Standard does";
    EXPECT_NO_ENGINE_ERRORS();
}
