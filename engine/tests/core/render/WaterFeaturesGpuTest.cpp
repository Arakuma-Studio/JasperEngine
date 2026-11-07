// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// library:water's spline features on the headless device (docs/specs/S18-splines.md D9, AC15).
// The shader clock is pinned (Engine::setPinnedShaderTime), so the ripples drift to the same
// place every run.
//
// water_features_off: a flat plane of a typical material (BUILTIN_TEXTURES, lit per pixel, the
// default values, no scene-depth feature) over a ground plane. Its golden was captured on glcore
// from the shader before FLOW and MESH_DEPTH, and glcore must still match it exactly: with both
// features off their code is preprocessed away and the uniform block only grows at its end, so
// the variant computes the same values in the same order. Another backend is held to the
// cross-backend bound against the same glcore image.
//
// water_flow_mesh_depth: FLOW and MESH_DEPTH on a synthetic river ribbon carrying what the spline
// water mesher writes (uv0 = (arc, depth), the flow in the vertex colour): the ripples and foam
// carried down the arc, foam and clear water along the banks, deeper colour mid-stream.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Primitive.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the shader time of every golden here (seconds)
    constexpr double PINNED_TIME = 7.25;

    bool isGoldenBackend(){
        return std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
    }

    // glcore against its own goldens: exact; another backend: the cross-backend bound
    CompareOptions exactOnGolden(){
        CompareOptions options;
        if (isGoldenBackend()){
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    // a library:water material as a scene has it: the built-in ripples and foam noise, lit per
    // pixel, every value at its default
    Material waterMaterial(const std::vector<std::string>& features){
        Material material;
        material.name = "Water";
        material.shader = "library:water";
        material.shaderFeatures = features;
        material.lighting = MaterialLighting::PIXEL;
        return material;
    }

    class WaterFeaturesGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            // the view the goldens and pixel positions were made at
            GpuEnvironment::resetView();
            Engine::setPinnedShaderTime(PINNED_TIME);
            scene.setBackgroundColor(0.12f, 0.14f, 0.18f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.4f, -1.0f, -0.5f);
            sun.setIntensity(1.5f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            Engine::removeScene(&scene);
            Engine::setPinnedShaderTime(-1.0);
        }

        bool render(){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current](){
                return current->getSystem<RenderSystem>()->isAllLoaded();
            });
        }

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, exactOnGolden());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }
    };

}

TEST_F(WaterFeaturesGpu, FeaturesOffMatchesTheShaderBeforeThem){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 3.0f, 7.0f);
    camera.setTarget(0.0f, 0.0f, -2.0f);
    camera.activate();

    // a sandy bed under part of the water, so its alpha shows over both it and the background
    Primitive ground(&scene);
    ground.createPlane(8.0f, 8.0f);
    ground.setPosition(-2.0f, -0.4f, -1.0f);
    ground.setColor(0.55f, 0.45f, 0.3f);

    Primitive water(&scene);
    water.createPlane(24.0f, 24.0f);
    water.setMaterial(waterMaterial({"BUILTIN_TEXTURES"}));

    ASSERT_TRUE(render()) << "the scene never finished loading";
    expectGolden("water_features_off");
    EXPECT_NO_ENGINE_ERRORS();
}

namespace {

    // a river ribbon as the spline water mesher writes one (docs/specs/S18-splines.md D9): a
    // 20 m arc of radius 10 m, 6 m wide, uv0 = (metres along it, depth in metres: 0 at the banks,
    // 2 mid-stream), colour rg = 0.5 + 0.5 * the flow direction over XZ (down the arc), b = the
    // share of flowSpeed (faster mid-stream)
    void buildRiver(Scene& scene, Mesh& river){
        constexpr int ALONG = 40;
        constexpr int ACROSS = 12;
        constexpr float LENGTH = 20.0f;
        constexpr float RADIUS = 10.0f;
        constexpr float HALF_WIDTH = 3.0f;
        constexpr float DEPTH = 2.0f;
        constexpr float START_ANGLE = -0.9f;
        const Vector2 centre(-6.0f, -2.0f);

        MeshComponent& mesh = river.getComponent<MeshComponent>();
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);

        std::vector<Vector3> points;
        for (int i = 0; i <= ALONG; i++){
            const float arc = LENGTH * (float)i / ALONG;
            const float angle = START_ANGLE + arc / RADIUS;
            const Vector2 radial(std::cos(angle), std::sin(angle));
            const Vector2 tangent(-radial.y, radial.x);
            for (int j = 0; j <= ACROSS; j++){
                const float side = 2.0f * (float)j / ACROSS - 1.0f;
                const float middle = 1.0f - side * side;
                const Vector2 xz = centre + radial * (RADIUS + side * HALF_WIDTH);
                points.push_back(Vector3(xz.x, 0.0f, xz.y));
                mesh.buffer.addVector3(position, points.back());
                mesh.buffer.addVector2(texcoord, Vector2(arc, DEPTH * middle));
                mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
                mesh.buffer.addVector4(color, Vector4(0.5f + 0.5f * tangent.x, 0.5f + 0.5f * tangent.y, 0.35f + 0.65f * middle, 1.0f));
            }
        }

        // front faces up: (b - a) x (c - a) along +Y
        std::vector<uint16_t> indices;
        auto triangle = [&](uint16_t a, uint16_t b, uint16_t c){
            const Vector3 up = (points[b] - points[a]).crossProduct(points[c] - points[a]);
            if (up.y >= 0.0f)
                indices.insert(indices.end(), {a, b, c});
            else
                indices.insert(indices.end(), {a, c, b});
        };
        const int row = ACROSS + 1;
        for (int i = 0; i < ALONG; i++){
            for (int j = 0; j < ACROSS; j++){
                const uint16_t a = (uint16_t)(i * row + j);
                const uint16_t b = (uint16_t)((i + 1) * row + j);
                const uint16_t c = (uint16_t)((i + 1) * row + j + 1);
                const uint16_t d = (uint16_t)(i * row + j + 1);
                triangle(a, b, c);
                triangle(a, c, d);
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)&indices[0], sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

}

// FLOW + MESH_DEPTH on a synthetic river: the ripples and foam carried down the arc, foam and
// clear water along the banks, deeper colour mid-stream, all from the mesh
TEST_F(WaterFeaturesGpu, RiverFlowAndMeshDepth){
    JASPER_REQUIRE_GPU();
    camera.setPosition(1.0f, 21.0f, 15.0f);
    camera.setTarget(1.0f, 0.0f, 0.5f);
    camera.activate();

    // the banks' ground, under the river
    Primitive ground(&scene);
    ground.createPlane(40.0f, 40.0f);
    ground.setPosition(1.5f, -0.25f, -1.5f);
    ground.setColor(0.55f, 0.5f, 0.32f);

    Mesh river(&scene);
    buildRiver(scene, river);
    river.setMaterial(waterMaterial({"BUILTIN_TEXTURES", "FLOW", "MESH_DEPTH"}));

    ASSERT_TRUE(render()) << "the scene never finished loading";
    expectGolden("water_flow_mesh_depth");
    EXPECT_NO_ENGINE_ERRORS();
}
