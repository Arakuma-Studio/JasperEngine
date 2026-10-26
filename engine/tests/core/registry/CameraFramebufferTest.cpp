// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A CameraComponent owns the Framebuffer its initializer allocates: it goes with the camera's
// entity, whether that entity is destroyed on its own or with its whole scene or registry.

#include "Scene.h"
#include "component/CameraComponent.h"
#include "registry/EntityRegistry.h"

#include <gtest/gtest.h>

#include <memory>

using namespace jasperengine;

namespace {

    int deletedFramebuffers = 0;

    class CountedFramebuffer : public Framebuffer {
    public:
        ~CountedFramebuffer() override{ deletedFramebuffers++; }
    };

    // swaps the framebuffer of every camera of the registry for a counted one; how many
    size_t countCameraFramebuffers(EntityRegistry& registry){
        ComponentArray<CameraComponent>* cameras = registry.getComponentArray<CameraComponent>();
        for (size_t i = 0; i < cameras->size(); i++){
            CameraComponent& camera = cameras->getComponentFromIndex(i);
            delete camera.framebuffer;
            camera.framebuffer = new CountedFramebuffer();
        }
        deletedFramebuffers = 0;
        return cameras->size();
    }

}

TEST(CameraFramebuffer, ClosingASceneDeletesItsCamerasFramebuffers){
    auto scene = std::make_unique<Scene>();
    const Entity entity = scene->createUserEntity();
    scene->addComponent<Transform>(entity, {});
    scene->addComponent<CameraComponent>(entity, {});
    // the one added (and the scene's default camera, once it has one)
    const size_t cameras = countCameraFramebuffers(*scene);
    ASSERT_GE(cameras, 1u);

    scene.reset();
    EXPECT_EQ(deletedFramebuffers, (int)cameras);
}

TEST(CameraFramebuffer, DestroyingTheEntityDeletesItsFramebufferOnce){
    auto scene = std::make_unique<Scene>();
    const Entity entity = scene->createUserEntity();
    scene->addComponent<Transform>(entity, {});
    scene->addComponent<CameraComponent>(entity, {});
    const size_t cameras = countCameraFramebuffers(*scene);

    scene->destroyEntity(entity);
    EXPECT_EQ(deletedFramebuffers, 1);
    scene.reset();
    EXPECT_EQ(deletedFramebuffers, (int)cameras);
}

TEST(CameraFramebuffer, ARegistryWithoutARendererDeletesThemToo){
    auto registry = std::make_unique<EntityRegistry>();
    for (int i = 0; i < 3; i++)
        registry->addComponent<CameraComponent>(registry->createUserEntity(), {});
    ASSERT_EQ(countCameraFramebuffers(*registry), 3u);

    registry.reset();
    EXPECT_EQ(deletedFramebuffers, 3);
}
