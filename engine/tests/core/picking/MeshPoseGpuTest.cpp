// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 AC5 (R6, D5): the CPU posing of picking against the GPU's. A skinned and morphed glTF strip,
// its tip bent and its morph target half on, drawn by the library's mesh.vert (skinning.glsl,
// morphtarget.glsl) with a debug fragment stage writing the world position (one axis per frame,
// 24-bit fixed point: fixtures/picking/world_position_<axis>.frag). For every pixel inside the
// strip, the position the GPU drew equals the point a MeshRaycast through that pixel's centre hits
// on the CPU-posed triangles, within 1e-4. Runs on glcore and Vulkan (lavapipe).

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Model.h"
#include "picking/MeshPose.h"
#include "picking/MeshRaycast.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"
#include "Readback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr float RANGE_MIN = -8.0f;
    constexpr float RANGE = 16.0f;

    // the world coordinate a pixel of the debug shader holds
    float decode(const uint8_t* rgba){
        const double value = (double)rgba[0] * 65536.0 + (double)rgba[1] * 256.0 + (double)rgba[2];
        return (float)(value / 16777215.0 * RANGE + RANGE_MIN);
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

    std::vector<uint8_t> images[3];
    const char* shaders[3] = {"picking/world_position_x.frag", "picking/world_position_y.frag", "picking/world_position_z.frag"};
    for (int axis = 0; axis < 3; axis++){
        mesh.customShader = shaders[axis];
        mesh.needReload = true;
        ASSERT_TRUE(GpuEnvironment::runFrames([&scene](){ return scene.getSystem<RenderSystem>()->isAllLoaded(); }))
            << shaders[axis] << " never compiled";
        GpuEnvironment::drawFrames(2);
        ASSERT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, images[axis]));
    }
    ASSERT_TRUE(picking::isPosed(mesh)) << "the strip is skinned and morphed";

    // every pixel well inside the strip (its 3x3 neighbourhood drawn: no edge, no MSAA blend)
    auto drawn = [&](int x, int y){
        return x >= 0 && y >= 0 && x < width && y < height && images[0][((size_t)y * width + x) * 4 + 3] == 255;
    };
    int compared = 0;
    float worst = 0.0f;
    for (int y = 1; y < height - 1; y++){
        for (int x = 1; x < width - 1; x++){
            bool inside = true;
            for (int dy = -1; dy <= 1 && inside; dy++){
                for (int dx = -1; dx <= 1 && inside; dx++){
                    inside = drawn(x + dx, y + dy);
                }
            }
            if (!inside){
                continue;
            }
            const Ray ray = camera.screenToRay((float)x + 0.5f, (float)y + 0.5f);
            MeshRaycastOptions options;
            options.hitBackfaces = true;
            const MeshRaycastHit hit = MeshRaycast::raycastEntity(&scene, strip, ray, options);
            if (!hit.hit){
                ADD_FAILURE() << "the GPU drew the strip at (" << x << ", " << y << ") where the CPU pose has nothing";
                continue;
            }
            const size_t i = ((size_t)y * width + x) * 4;
            const Vector3 gpu(decode(&images[0][i]), decode(&images[1][i]), decode(&images[2][i]));
            const float error = (gpu - hit.point).length();
            worst = std::max(worst, error);
            compared++;
            if (error > 1e-4f){
                ADD_FAILURE() << "pixel (" << x << ", " << y << "): GPU " << gpu.toString() << " CPU " << hit.point.toString();
                if (compared > 50){
                    break;
                }
            }
        }
    }
    EXPECT_GT(compared, 200) << "too few pixels of the strip were drawn";
    EXPECT_LE(worst, 1e-4f);
    EXPECT_NO_ENGINE_ERRORS();
    Engine::removeScene(&scene);
}
