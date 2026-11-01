// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The pixels of a glTF model's embedded image go once a GPU texture is made of them
// (setReleaseDataAfterLoad). A Texture of that image that makes its GPU texture later (its pool
// entry gone with its last holder, another sampler) had no pixels to make it of and drew white
// for good. It makes them again from the model's decoded image.

#include "Engine.h"
#include "Scene.h"
#include "object/Model.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace jasperengine;

TEST(GltfTexturePixels, AnEmbeddedImageMakesItsPixelsAgainOnceReleased){
    Scene scene;
    Model model(&scene);
    const Entity entity = model.getEntity();
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    ASSERT_TRUE(meshSystem->loadGLTF(entity, JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/SpecularTest/glTF-Binary/SpecularTest.glb",
                                     false, false, false));

    // the label plates' base colour: the model's one base colour texture (embedded)
    Texture* found = nullptr;
    auto meshes = scene.getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size() && !found; i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);
        for (unsigned int s = 0; s < mesh.numSubmeshes && !found; s++){
            Texture& texture = mesh.submeshes[s].material.baseColorTexture;
            if (!texture.getId().empty()) found = &texture;
        }
    }
    ASSERT_NE(found, nullptr) << "a submesh with the embedded base colour texture";
    Texture texture = *found;
    ASSERT_EQ(texture.load().state, ResourceLoadState::Finished);
    const std::string id = texture.getId();
    const unsigned int width = texture.getWidth();

    // what the cleanup after a GPU upload does to the pooled copy
    std::shared_ptr<std::array<TextureData, 6>> pooled = TextureDataPool::get(id);
    ASSERT_TRUE(pooled && pooled->at(0).getData());
    pooled->at(0).releaseImageData();

    // a Texture of the image making its own GPU texture now: its pixels again, from the model
    Texture later = texture;
    TextureLoadResult result = later.load();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (result.state == ResourceLoadState::Loading && std::chrono::steady_clock::now() < until){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        result = later.load();
    }
    ASSERT_EQ(result.state, ResourceLoadState::Finished) << "state " << (int)result.state << ": no pixels to make a GPU texture of";
    ASSERT_TRUE(later.hasData());
    EXPECT_NE(later.getData().getData(), nullptr);
    EXPECT_EQ(later.getWidth(), width);
}
