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
#include <memory>
#include <string>
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

// The editor's Project window makes a thumbnail of a model file that appears or changes: a
// preview of the model at 256 px (MeshSystem::setImageDecodeMaxDimension), loaded on the
// thumbnail strand while a scene may be loading the same file (BF15). It keyed its images as the
// scene's model does, so the two shared one pooled image: the preview's GPU upload freed the
// scene's pixels, its generator (bound to the preview's model, gone with the preview) replaced
// the scene's, and a scene mesh that made its GPU texture after that drew white for good.
namespace {
    const char* const SPECULAR_TEST = JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/SpecularTest/glTF-Binary/SpecularTest.glb";

    // the label plates' base colour: the model's one base colour texture (embedded)
    Texture* labelsTexture(Scene& scene){
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            MeshComponent& mesh = meshes->getComponentFromIndex(i);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                Texture& texture = mesh.submeshes[s].material.baseColorTexture;
                if (!texture.getId().empty()) return &texture;
            }
        }
        return nullptr;
    }

    TextureLoadResult loadUntilDone(Texture& texture){
        TextureLoadResult result = texture.load();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (result.state == ResourceLoadState::Loading && std::chrono::steady_clock::now() < until){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            result = texture.load();
        }
        return result;
    }

    // a thumbnail's preview of the model, as ProjectWindow makes it: loaded at 256 px; `upload`
    // does what its GPU upload does to its image (frees the pooled pixels), then it goes
    void previewModel(bool upload, std::string& previewId){
        auto preview = std::make_unique<Scene>();
        Model model(preview.get());
        MeshSystem::setImageDecodeMaxDimension(256);
        const bool loaded = model.loadModel(SPECULAR_TEST);
        MeshSystem::setImageDecodeMaxDimension(0);
        ASSERT_TRUE(loaded);
        Texture* texture = labelsTexture(*preview);
        ASSERT_NE(texture, nullptr);
        previewId = texture->getId();
        if (upload){
            std::shared_ptr<std::array<TextureData, 6>> pooled = TextureDataPool::get(previewId);
            ASSERT_TRUE(pooled && pooled->at(0).getData());
            pooled->at(0).releaseImageData();
        }
        // what RenderSystem::destroyMesh does to the preview's textures when it goes
        texture->destroy();
    }
}

// the preview loads while the scene's mesh has not made its GPU texture yet
TEST(GltfTexturePixels, APreviewOfTheModelLeavesTheSceneItsImage){
    Scene scene;
    Model model(&scene);
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    ASSERT_TRUE(meshSystem->loadGLTF(model.getEntity(), SPECULAR_TEST, false, false, false));
    Texture* labels = labelsTexture(scene);
    ASSERT_NE(labels, nullptr);
    const unsigned int width = labels->getWidth();
    ASSERT_EQ(width, 512u);

    std::string previewId;
    previewModel(true, previewId);
    ASSERT_FALSE(HasFatalFailure());

    // the scene's mesh makes its GPU texture now: the image's pixels, at full size
    TextureLoadResult result = loadUntilDone(*labels);
    ASSERT_EQ(result.state, ResourceLoadState::Finished) << "state " << (int)result.state << ": the scene's image has no pixels after the preview (id " << previewId << ")";
    ASSERT_TRUE(labels->hasData());
    EXPECT_NE(labels->getData().getData(), nullptr);
    EXPECT_EQ(labels->getData().getWidth(), (int)width);
}

// the preview loads after the scene's mesh made its GPU texture (its pixels freed then), and
// the scene's mesh makes it again later (another sampler, a GPU texture remade)
TEST(GltfTexturePixels, APreviewOfTheModelDoesNotReplaceTheScenesImage){
    Scene scene;
    Model model(&scene);
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    ASSERT_TRUE(meshSystem->loadGLTF(model.getEntity(), SPECULAR_TEST, false, false, false));
    Texture* labels = labelsTexture(scene);
    ASSERT_NE(labels, nullptr);
    ASSERT_EQ(loadUntilDone(*labels).state, ResourceLoadState::Finished);
    const unsigned int width = labels->getWidth();
    // what the cleanup after the scene's GPU upload does
    labels->getData().releaseImageData();

    std::string previewId;
    previewModel(false, previewId);
    ASSERT_FALSE(HasFatalFailure());

    Texture again = *labels;
    TextureLoadResult result = loadUntilDone(again);
    ASSERT_EQ(result.state, ResourceLoadState::Finished) << "state " << (int)result.state << ": no pixels for the scene's image after the preview (id " << previewId << ")";
    ASSERT_TRUE(again.hasData());
    EXPECT_NE(again.getData().getData(), nullptr);
    EXPECT_EQ(again.getData().getWidth(), (int)width) << "the scene's image is the preview's";
}
