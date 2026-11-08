// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 AC5 (R6, R30, D5): the CPU posing of picking against the GPU's. A skinned and morphed glTF
// strip, its tip bent and its morph target half on, drawn by a debug variant of the skinned mesh
// vertex shader (fixtures/picking/world_position.vert: skinning.glsl and morphtarget.glsl, as the
// library's mesh shaders call them) as one point per vertex, in a cell of its own; one frame per
// value writes each point's world x, y, z and its vertex index as 24-bit fixed point
// (world_position_<x|y|z|index>.frag). Every vertex the GPU drew equals the vertex
// picking::poseGeometry poses on the CPU, in world space, within 1e-4. Runs on glcore and Vulkan
// (lavapipe).

#include "Engine.h"
#include "component/Transform.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Model.h"
#include "picking/MeshPose.h"
#include "picking/PickGeometry.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr float RANGE_MIN = -8.0f;
    constexpr float RANGE = 16.0f;

    // the world coordinate a pixel of the debug shader holds
    double decodeInteger(const uint8_t* rgba){
        return (double)rgba[0] * 65536.0 + (double)rgba[1] * 256.0 + (double)rgba[2];
    }
    float decode(const uint8_t* rgba){
        return (float)(decodeInteger(rgba) / 16777215.0 * RANGE + RANGE_MIN);
    }

    Entity skinnedMeshEntity(Scene& scene){
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            const MeshComponent& mesh = meshes->getComponentFromIndex(i);
            if (mesh.numSubmeshes > 0 && mesh.submeshes[0].hasSkinning){
                return meshes->getEntity(i);
            }
        }
        return NULL_ENTITY;
    }

}

TEST(MeshPose, MatchesGpuSkinning){
    JASPER_REQUIRE_GPU();
    Scene scene;
    scene.setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(-0.3f, 1.1f, 3.2f);
    camera.setTarget(-0.3f, 1.0f, 0.0f);
    camera.activate();
    Engine::setScene(&scene);

    Model model(&scene);
    ASSERT_TRUE(model.loadModel("picking/skinned_morph.gltf"));
    auto loaded = [&scene](){ return scene.getSystem<RenderSystem>()->isAllLoaded() && skinnedMeshEntity(scene) != NULL_ENTITY; };
    ASSERT_TRUE(GpuEnvironment::runFrames(loaded)) << "the model never loaded";

    const Entity strip = skinnedMeshEntity(scene);
    ASSERT_NE(strip, NULL_ENTITY);
    MeshComponent& mesh = scene.getComponent<MeshComponent>(strip);

    // the pose: the tip joint bent 70 degrees about z, the morph target half on
    Entity tip = NULL_ENTITY;
    auto transforms = scene.getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        if (scene.getEntityName(transforms->getEntity(i)) == "Tip"){
            tip = transforms->getEntity(i);
        }
    }
    ASSERT_NE(tip, NULL_ENTITY) << "the fixture's Tip joint";
    Transform& tipTransform = scene.getComponent<Transform>(tip);
    tipTransform.rotation = Quaternion(Angle::degToDefault(70.0f), Vector3(0, 0, 1));
    tipTransform.needUpdate = true;
    mesh.morphWeights[0] = 0.5f;

    Framebuffer& target = GpuEnvironment::target();
    const int width = (int)target.getWidth();
    const int height = (int)target.getHeight();
    ASSERT_GT(width, 0);

    // the strip's vertices as picking gathers them (before it is drawn as points)
    PickGeometry geometry;
    PickGeometryLayout layout;
    ASSERT_TRUE(picking::gather(mesh, geometry, &layout));
    ASSERT_EQ(layout.submeshes.size(), 1u);
    const uint32_t vertexCount = layout.submeshes[0].vertexCount;
    ASSERT_GT(vertexCount, 0u);
    ASSERT_LE(vertexCount, 256u) << "one 16 x 16 cell per vertex";
    mesh.submeshes[0].primitiveType = PrimitiveType::POINTS;

    std::vector<uint8_t> images[4];
    const char* shaders[4] = {"picking/world_position.vert|picking/world_position_x.frag", "picking/world_position.vert|picking/world_position_y.frag",
                              "picking/world_position.vert|picking/world_position_z.frag", "picking/world_position.vert|picking/world_position_index.frag"};
    for (int pass = 0; pass < 4; pass++){
        mesh.customShader = shaders[pass];
        mesh.needReload = true;
        ASSERT_TRUE(GpuEnvironment::runFrames([&scene](){ return scene.getSystem<RenderSystem>()->isAllLoaded(); }))
            << shaders[pass] << " never compiled";
        GpuEnvironment::drawFrames(2);
        ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, images[pass]));
    }
    ASSERT_TRUE(picking::isPosed(mesh)) << "the strip is skinned and morphed";

    // the CPU pose of the bone matrices and morph weights the frames drew with
    std::vector<Vector3> posed;
    ASSERT_TRUE(picking::poseGeometry(mesh, layout, geometry.positions, posed));
    const Matrix4& modelMatrix = scene.getComponent<Transform>(strip).modelMatrix;

    std::vector<bool> seen(vertexCount, false);
    float worst = 0.0f;
    float moved = 0.0f;
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            const size_t i = ((size_t)y * width + x) * 4;
            if (images[3][i + 3] != 255){
                continue;
            }
            const double index = decodeInteger(&images[3][i]);
            ASSERT_LT(index, (double)vertexCount) << "pixel (" << x << ", " << y << ") names no vertex";
            const uint32_t vertex = (uint32_t)index;
            seen[vertex] = true;
            const Vector3 gpu(decode(&images[0][i]), decode(&images[1][i]), decode(&images[2][i]));
            const Vector3 cpu = modelMatrix * posed[layout.submeshes[0].base + vertex];
            const float error = (gpu - cpu).length();
            // (a NaN is the worst there is)
            worst = std::isnan(error) ? INFINITY : std::max(worst, error);
            moved = std::max(moved, (cpu - modelMatrix * geometry.positions[layout.submeshes[0].base + vertex]).length());
            if (!(error <= 1e-4f)){
                // what the CPU posed the vertex from
                picking::VertexPoseInput input;
                std::string detail;
                if (picking::readPoseInput(mesh, layout, 0, vertex, input)){
                    detail = " skinned " + std::to_string(input.skinned) + " morphs " + std::to_string(input.morphCount);
                    for (int b = 0; b < 4; b++){
                        detail += " joint " + std::to_string(input.boneIds[b]) + " weight " + std::to_string(input.boneWeights[b]);
                    }
                }
                ADD_FAILURE() << "vertex " << vertex << ": GPU " << gpu.toString() << " CPU " << cpu.toString() << detail;
            }
        }
    }
    if (worst > 1e-4f){
        const picking::PoseParameters parameters = picking::poseParameters(mesh, 0);
        std::string bones = "normAdjust " + std::to_string(parameters.normAdjustJoint) + " " + std::to_string(parameters.normAdjustWeight);
        for (size_t b = 0; b < parameters.bones.size(); b++){
            bones += "\nbone " + std::to_string(b) + " " + parameters.bones[b].toString();
        }
        ADD_FAILURE() << bones;
    }
    for (uint32_t v = 0; v < vertexCount; v++){
        EXPECT_TRUE(seen[v]) << "the GPU drew no point for vertex " << v;
    }
    EXPECT_LE(worst, 1e-4f);
    // the pose moves the strip (a bind-pose CPU copy would not match)
    EXPECT_GT(moved, 0.05f);
    EXPECT_NO_ENGINE_ERRORS();
    Engine::removeScene(&scene);
}
