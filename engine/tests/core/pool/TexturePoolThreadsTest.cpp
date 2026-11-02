// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The editor's thumbnail strand (ProjectWindow::thumbnailStep, a Background SerialQueue), and a
// game's loading thread (Engine.startAsyncThread), load models next to the main thread's frames:
// MeshSystem::loadGLTFTexture looks images up in TextureDataPool and registers new ones there, a
// Texture naming one looks its GPU texture up in TexturePool, and the pixels they let go and the
// GPU uploads they ask for go through SokolCmdQueue's cleanups and commands. The main thread does
// the same every frame. These runs have no expectation of their own beyond finishing: under
// SANITIZE=thread (docs/testing.md, Sanitizers) a race among them stops the run with its report.
// Without a sanitizer two threads writing one std::map take the process down sooner or later.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"
#include "render/SystemRender.h"
#include "render/TextureRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "texture/Material.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // (six images, embedded, and few enough submeshes for a preview's one mesh)
    const char* const PREVIEW_MODEL = JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/AnisotropyRotationTest/glTF-Binary/AnisotropyRotationTest.glb";

    // a one-face image whose pixels the pool's copy owns
    std::shared_ptr<std::array<TextureData, 6>> onePixelImage(uint8_t value){
        unsigned char* pixels = static_cast<unsigned char*>(std::malloc(4));
        pixels[0] = pixels[1] = pixels[2] = value;
        pixels[3] = 255;
        auto faces = std::make_shared<std::array<TextureData, 6>>();
        faces->at(0) = TextureData(1, 1, 4, ColorFormat::RGBA, 4, pixels);
        faces->at(0).setDataOwned(true);
        return faces;
    }

    void registerOnePixel(const std::string& id, uint8_t value){
        TextureDataPool::setGenerator(id, [value]() -> TextureData {
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(4));
            if (!pixels)
                return TextureData();
            pixels[0] = pixels[1] = pixels[2] = value;
            pixels[3] = 255;
            // not owned: the pool's copy owns the pixels
            return TextureData(1, 1, 4, ColorFormat::RGBA, 4, pixels);
        });
    }

    // every call of the pool a model's load makes off the main thread, on ids of its own
    void poolLoadCalls(const std::string& prefix, int iterations){
        for (int i = 0; i < iterations; i++){
            const std::string id = prefix + std::to_string(i % 16);
            // as MeshSystem::loadGLTFTexture: the pool's copy takes the pixels
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(4));
            pixels[0] = pixels[1] = pixels[2] = (uint8_t)i;
            pixels[3] = 255;
            Texture texture;
            texture.setData(id, TextureData(1, 1, 4, ColorFormat::RGBA, 4, pixels));
            TextureData& pooled = texture.getData();
            if (pooled.getData() == pixels)
                pooled.setDataOwned(true);
            else
                std::free(pixels);
            EXPECT_NE(TextureDataPool::get(id), nullptr);
            (void)TextureDataPool::generation(id);
            TextureSource source;
            (void)TextureDataPool::getSource(id, source);
            Texture named;
            named.setId(id);
            pooled.releaseImageData();
            TextureDataPool::replace(id, onePixelImage((uint8_t)i));
            TextureDataPool::invalidate(id);
            TextureDataPool::remove(id);
        }
    }

}

// what the pool is asked by two threads at once, on ids of each one's own: a map shared by both
TEST(TextureDataPoolThreads, TwoThreadsLoadImagesAtOnce){
    std::thread other([](){ poolLoadCalls("bf17-other-", 2000); });
    poolLoadCalls("bf17-main-", 2000);
    other.join();
    EXPECT_NO_ENGINE_ERRORS();
}

// the GPU textures' pool, on ids of each thread's own (no GPU texture is made: only the entries)
TEST(TextureDataPoolThreads, TwoThreadsAskForGpuTexturesAtOnce){
    const auto calls = [](const std::string& prefix){
        for (int i = 0; i < 4000; i++){
            const std::string id = prefix + std::to_string(i % 16);
            TexturePool::add(id, std::make_shared<TextureRender>());
            (void)TexturePool::get(id);
            (void)TexturePool::getKept(id);
            (void)TexturePool::holds(id, nullptr);
            TexturePool::invalidate(id);
            TexturePool::remove(id);
        }
    };
    std::thread other(calls, "bf17-other-gpu-");
    calls("bf17-main-gpu-");
    other.join();
    EXPECT_NO_ENGINE_ERRORS();
}

// What a thumbnail's model load asks the pools off the main thread (MeshSystem::loadGLTFTexture,
// Texture::setData, Texture::setId), next to the main thread's own
TEST(TextureDataPoolThreads, AModelLoadingOnItsThreadSharesThePoolsWithTheMainThread){
    std::atomic<bool> done{false};
    std::mutex uploadedMutex;
    std::vector<std::shared_ptr<std::array<TextureData, 6>>> uploaded;
    std::thread strand([&done, &uploadedMutex, &uploaded](){
        for (int i = 0; i < 3; i++){
            Scene preview(EntityPool::System);
            Model model(&preview);
            MeshSystem::setImageDecodeMaxDimension(256);
            EXPECT_TRUE(preview.getSystem<MeshSystem>()->loadGLTF(model.getEntity(), PREVIEW_MODEL, false, false, false));
            MeshSystem::setImageDecodeMaxDimension(0);
            // this preview's images, as its GPU upload holds them: their cleanups come next to
            // the next preview's load, which finds the same ones in the pool
            auto meshes = preview.getComponentArray<MeshComponent>();
            std::lock_guard<std::mutex> lock(uploadedMutex);
            for (size_t m = 0; m < meshes->size(); m++){
                MeshComponent& mesh = meshes->getComponentFromIndex(m);
                for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
                    const std::string& id = mesh.submeshes[s].material.baseColorTexture.getId();
                    if (std::shared_ptr<std::array<TextureData, 6>> pooled = id.empty() ? nullptr : TextureDataPool::get(id))
                        uploaded.push_back(pooled);
                }
            }
        }
        done.store(true);
    });
    for (int i = 0; !done.load(); i++){
        // what the cleanup after a preview's GPU upload does on the main thread (Texture::getRender)
        {
            std::lock_guard<std::mutex> lock(uploadedMutex);
            for (const std::shared_ptr<std::array<TextureData, 6>>& images : uploaded)
                TextureDataPool::releasePixels(images, 0);
            uploaded.clear();
        }
        const std::string id = "bf17-main-" + std::to_string(i % 16);
        TextureDataPool::replace(id, onePixelImage((uint8_t)i));
        EXPECT_NE(TextureDataPool::get(id), nullptr);
        // a GPU texture made and let go (none is made here: only the pool's entry)
        TexturePool::add(id, std::make_shared<TextureRender>());
        (void)TexturePool::get(id);
        (void)TexturePool::holds(id, nullptr);
        TexturePool::remove(id);
        TextureDataPool::invalidate(id);
    }
    strand.join();
    EXPECT_NO_ENGINE_ERRORS();
}

// A thumbnail's preview on its strand next to the frames, every step as ProjectWindow takes it
// (thumbnailStep off the main thread, processModelThumbnails on it), several times over: the
// strand loads the model into a preview scene of its own and asks the main thread to draw it. The
// main thread draws frames and, between them, makes and replaces images as a hot reload does; it
// frees a preview after it was drawn, while the strand waits for that. (It keeps its GPU objects
// between frames: the strand's Engine::executeSceneOnce takes the frames' semaphore for its GPU
// work, what the main thread frees outside a frame is another matter.)
TEST(TextureDataPoolThreadsGpu, APreviewOnItsStrandSharesThePoolsWithTheFrames){
    JASPER_REQUIRE_GPU();
    // (what the frames leave made goes with them below: the suites after this one make their own)
    struct LeavesNoShaders {
        ~LeavesNoShaders(){
            GpuEnvironment::drawFrames(2);
            ShaderPool::clearUnused();
        }
    } leavesNoShaders;
    Scene scene;
    Camera camera(&scene);
    camera.setType(CameraType::CAMERA_PERSPECTIVE);
    camera.setPosition(0.0f, 2.2f, 2.6f);
    camera.setTarget(0.0f, 0.0f, 0.0f);
    camera.activate();
    Light sun(&scene);
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.3f);
    sun.setShadows(false);
    Primitive plane(&scene);
    plane.createPlane(1.0f, 1.0f);
    Engine::setScene(&scene);

    struct Preview {
        Scene scene{EntityPool::System};
        Camera camera{&scene};
        Light light{&scene};
        Model model{&scene};
    };
    constexpr int previews = 3;
    std::atomic<Preview*> drawn{nullptr};
    std::atomic<bool> strandDone{false};
    std::atomic<bool> stop{false};
    std::thread strand([&](){
        for (int i = 0; i < previews && !stop; i++){
            Preview* preview = nullptr;
            {
                Engine::AsyncThreadScope loadScope;
                MeshSystem::setImageDecodeMaxDimension(256);
                preview = new Preview();
                preview->scene.setCamera(&preview->camera);
                preview->camera.setType(CameraType::CAMERA_PERSPECTIVE);
                preview->camera.setFramebufferSize(64, 64);
                preview->camera.setRenderToTexture(true);
                preview->light.setType(LightType::DIRECTIONAL);
                const bool loaded = preview->model.loadModel(PREVIEW_MODEL);
                MeshSystem::setImageDecodeMaxDimension(0);
                EXPECT_TRUE(loaded);
                {
                    Engine::AsyncThreadScope drawScope;
                    Engine::executeSceneOnce(&preview->scene);
                }
            }
            drawn.store(preview);
            while (drawn.load() && !stop)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        strandDone.store(true);
    });

    // the frames, until the strand is done (a commit of its queue waits for one, so it never
    // finishes without them)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    int frame = 0;
    while (!strandDone.load()){
        if (std::chrono::steady_clock::now() > deadline){
            ADD_FAILURE() << "the strand's previews never finished";
            stop.store(true);
        }
        const std::string id = "bf17-frame-" + std::to_string(frame % 8);
        frame++;
        registerOnePixel(id, (uint8_t)frame);
        Material material;
        Texture named;
        named.setId(id);
        material.baseColorTexture = named;
        plane.setMaterial(material);
        GpuEnvironment::drawFrames(1);
        TextureDataPool::replace(id, onePixelImage((uint8_t)frame));
        TexturePool::invalidate(id);
        GpuEnvironment::drawFrames(1);

        Preview* preview = drawn.load();
        if (preview && !Engine::isSceneRunning(&preview->scene)){
            Engine::removeScene(&preview->scene);
            delete preview;
            drawn.store(nullptr);
        }
    }
    strand.join();
    if (Preview* preview = drawn.exchange(nullptr)){
        Engine::removeScene(&preview->scene);
        delete preview;
    }
    GpuEnvironment::drawFrames(2);
    Engine::removeScene(&scene);
    // (no look at the engine's errors: a preview off the main thread bakes its nodes into one mesh,
    // and this model's nodes are over that mesh's limit of external buffers: the images are what
    // this run is about)
}

// The cleanups of the render queue: a thread working under Engine::AsyncThreadScope queues them (the
// release of an upload's pixels, an entry's last reference on the strand: shareDecoded's deleter)
// while the main thread runs the batch it committed (SokolCmdQueue::process_cleanups)
TEST(TextureDataPoolThreadsGpu, TheRenderQueuesCleanupsComeFromAnotherThreadDuringItsExecution){
    JASPER_REQUIRE_GPU();
    std::atomic<int> ran{0};
    std::atomic<bool> done{false};
    std::thread strand([&ran, &done](){
        Engine::AsyncThreadScope scope;
        for (int batch = 0; batch < 200; batch++){
            for (int i = 0; i < 32; i++)
                SystemRender::scheduleCleanup([](void* counter){ static_cast<std::atomic<int>*>(counter)->fetch_add(1); }, &ran);
            Engine::commitThreadQueue();
        }
        done.store(true);
    });
    while (!done.load())
        SystemRender::executeQueue();
    strand.join();
    for (int frame = 0; frame < 8; frame++)
        SystemRender::executeQueue();
    // (scope's end committed one more batch)
    EXPECT_GT(ran.load(), 0);
    EXPECT_NO_ENGINE_ERRORS();
}

// What the pools' lock costs (it is taken by a lookup a model's load or a binding makes): nanoseconds
// per lookup among 2000 ids, printed as [bench] lines (docs/textures.md, Measurements): the map
// lookup as it was without a lock, the same with an uncontended lock, and the pools' own.
TEST(TextureDataPoolBench, Lookup){
    constexpr int ids = 2000;
    constexpr int lookups = 2000000;
    std::vector<std::string> names;
    std::map<std::string, std::shared_ptr<std::array<TextureData, 6>>> plain;
    for (int i = 0; i < ids; i++){
        names.push_back("models/some-folder/some-model.glb|gltf-image|" + std::to_string(i));
        std::shared_ptr<std::array<TextureData, 6>> image = onePixelImage((uint8_t)i);
        plain[names.back()] = image;
        TextureDataPool::replace(names.back(), image);
    }
    const auto measure = [&](const char* what, const std::function<bool(const std::string&)>& lookup){
        const auto start = std::chrono::steady_clock::now();
        int found = 0;
        for (int i = 0; i < lookups; i++)
            found += lookup(names[(size_t)(i * 7) % names.size()]) ? 1 : 0;
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / lookups;
        printf("[bench] %s: %.1f ns per lookup (%d found)\n", what, ns, found);
    };
    std::mutex mutex;
    for (int run = 0; run < 3; run++){
        measure("map, no lock", [&](const std::string& id){
            auto it = plain.find(id);
            std::shared_ptr<std::array<TextureData, 6>> copy = it != plain.end() ? it->second : nullptr;
            return copy != nullptr;
        });
        measure("map, uncontended lock", [&](const std::string& id){
            std::lock_guard<std::mutex> lock(mutex);
            auto it = plain.find(id);
            std::shared_ptr<std::array<TextureData, 6>> copy = it != plain.end() ? it->second : nullptr;
            return copy != nullptr;
        });
        measure("TextureDataPool::get", [](const std::string& id){ return TextureDataPool::get(id) != nullptr; });
        measure("TexturePool::get (a miss)", [](const std::string& id){ return TexturePool::get(id) != nullptr; });
    }
    for (const std::string& name : names)
        TextureDataPool::invalidate(name);
    EXPECT_NO_ENGINE_ERRORS();
}
