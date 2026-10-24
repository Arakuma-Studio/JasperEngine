// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A skinned glTF loaded into a fresh scene: building its skeleton adds a Transform per bone,
// which grows the scene's Transform array, so MeshSystem::loadGLTF must not keep writing to the
// root Transform through a reference taken before (it wrote one byte into the freed array: the
// heap corruption crashed the shader variant probe's scene teardown on play).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "System.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "object/Model.h"
#include "subsystem/MeshSystem.h"

#include <filesystem>
#include <string>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    // File (the glTF loader's reads) goes through System::instance(), which the editor's
    // Platform provides: a bare one, set once for the test binary
    class FileSystemOnly : public System {
    public:
        int getScreenWidth() override { return 1000; }
        int getScreenHeight() override { return 480; }
    };

    class GltfSkeleton : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            static FileSystemOnly* system = nullptr;
            if (!system) {
                system = new FileSystemOnly();
                Engine::systemInit(0, nullptr, system);
            }
        }
    };

    // the pose animation fixture's hero: 43 bones (Mixamo names), one skinned mesh
    fs::path heroModel() {
        return fs::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / ".." / ".." / "tools" / "fixtures" / "pose-animation" /
               "assets" / "models" / "hero_tpose.gltf";
    }

}

TEST_F(GltfSkeleton, RootTransformFlaggedAfterTheBonesGrowTheArray) {
    const fs::path path = heroModel();
    ASSERT_TRUE(fs::is_regular_file(path)) << path;

    Scene scene;
    Model model(&scene);
    const Entity entity = model.getEntity();
    const size_t transformsBefore = scene.getComponentArray<Transform>()->size();
    // the root's flag is only raised by the skeleton step when the root transform is left alone
    scene.getComponent<Transform>(entity).needUpdate = false;

    ASSERT_TRUE(scene.getSystem<MeshSystem>()->loadGLTF(entity, path.string(), false, false, false));

    const ModelComponent& modelComponent = scene.getComponent<ModelComponent>(entity);
    ASSERT_NE(modelComponent.skeleton, NULL_ENTITY) << "the hero has a skeleton";
    ASSERT_GE(scene.getComponentArray<Transform>()->size(), transformsBefore + 40) << "a Transform per bone";
    EXPECT_TRUE(scene.getComponent<Transform>(entity).needUpdate)
        << "the root Transform is flagged for the skinning matrices (written through a stale reference before)";
}
