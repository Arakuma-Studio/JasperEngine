// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A model loading off the main thread (the editor's thumbnail strand, a game's
// Engine.startAsyncThread) asks ShaderPool for its variants and has SokolCmdQueue allocate its GPU
// objects, under the frames' semaphore (Engine::executeSceneOnce). What the main thread does
// outside a frame is not under it: an entity or a scene going away lets its variants go
// (ShaderPool::remove) and frees its GPU objects (sg_destroy_*). As in TexturePoolThreadsTest.cpp,
// these runs expect little beyond finishing: under SANITIZE=thread (docs/testing.md, Sanitizers) a
// race among them stops the run with its report, and without a sanitizer two threads writing one
// std::map, or one of sokol's free lists, take the process down sooner or later.

#include "Engine.h"
#include "Scene.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/ShaderPool.h"
#include "render/BufferRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"

#include "GpuEnvironment.h"
#include "LogCapture.h"

#include "sokol_gfx.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // (a variant no build has: its properties are far past any real shader's)
    constexpr uint64_t BF18_PROPERTIES = 0xBF18ull << 40;

    // (six images, embedded, and few enough submeshes for a preview's one mesh)
    const char* const PREVIEW_MODEL = JASPER_TEST_REPO_DIR "/engine/tests/render/assets/khronos/AnisotropyRotationTest/glTF-Binary/AnisotropyRotationTest.glb";

    // the builder, for the run: every variant still building, but one whose build fails, its
    // every failEvery-th ask (its error is logged: few enough that the log's valve holds none back
    // for a later test to find)
    struct BuilderScope {
        std::atomic<int> failEvery{1};
        std::atomic<int> asks{0};
        explicit BuilderScope(ShaderKey failing){
            ShaderPool::setShaderBuilder([this, failing](ShaderKey key){
                ShaderBuildResult result;
                const bool fails = key == failing && asks.fetch_add(1) % failEvery.load() == 0;
                result.state = fails ? ResourceLoadState::Failed : ResourceLoadState::Loading;
                return result;
            });
        }
        ~BuilderScope(){
            ShaderPool::setShaderBuilder(nullptr);
        }
    };

}

// A thread asks for variants, and holds some, while another lets the same ones go and looks at
// them: what's held stays in the pool, what's not is gone, and a build failure is forgotten by remove()
TEST(ShaderPoolThreads, OneThreadAsksForVariantsWhileAnotherLetsThemGo){
    constexpr int variants = 4;
    constexpr int rounds = 300;
    const ShaderKey failing = ShaderPool::getShaderKey(ShaderType::BLIT, BF18_PROPERTIES + variants - 1);
    BuilderScope builder(failing);
    builder.failEvery = 32;

    std::atomic<bool> done{false};
    std::atomic<int> nulls{0};
    std::thread strand([&done, &nulls](){
        std::vector<std::shared_ptr<ShaderRender>> held;
        for (int round = 0; round < rounds; round++){
            for (int i = 0; i < variants; i++){
                std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::BLIT, BF18_PROPERTIES + i);
                if (!shader)
                    nulls.fetch_add(1);
                if (round % 8 == 0)
                    held.push_back(std::move(shader));
            }
            if (round % 8 == 7)
                held.clear();
        }
        done.store(true);
    });
    while (!done.load()){
        for (int i = 0; i < variants; i++){
            (void)ShaderPool::isShaderReady(ShaderType::BLIT, BF18_PROPERTIES + i);
            (void)ShaderPool::getShaderState(ShaderPool::getShaderKey(ShaderType::BLIT, BF18_PROPERTIES + i));
            (void)ShaderPool::isShaderBuildFailed(ShaderType::BLIT, BF18_PROPERTIES + i);
            ShaderPool::remove(ShaderType::BLIT, BF18_PROPERTIES + i);
        }
        ShaderKey found;
        (void)ShaderPool::findKeyOfHandle(0xBF18, found);
    }
    strand.join();
    EXPECT_EQ(nulls.load(), 0);

    // settled, one thread: an entry someone holds stays, and it goes with its last holder
    {
        std::shared_ptr<ShaderRender> shader = ShaderPool::get(ShaderType::BLIT, BF18_PROPERTIES);
        ShaderPool::remove(ShaderType::BLIT, BF18_PROPERTIES);
        EXPECT_EQ(ShaderPool::get(ShaderType::BLIT, BF18_PROPERTIES), shader);
    }
    EXPECT_EQ(ShaderPool::getShaderState(ShaderPool::getShaderKey(ShaderType::BLIT, BF18_PROPERTIES)), ResourceLoadState::Loading);
    ShaderPool::remove(ShaderType::BLIT, BF18_PROPERTIES);
    EXPECT_EQ(ShaderPool::getShaderState(ShaderPool::getShaderKey(ShaderType::BLIT, BF18_PROPERTIES)), ResourceLoadState::NotStarted);

    // the one that failed, until remove() forgets it
    builder.failEvery = 1;
    (void)ShaderPool::get(ShaderType::BLIT, BF18_PROPERTIES + variants - 1);
    EXPECT_TRUE(ShaderPool::isShaderBuildFailed(ShaderType::BLIT, BF18_PROPERTIES + variants - 1));
    ShaderPool::remove(ShaderType::BLIT, BF18_PROPERTIES + variants - 1);
    EXPECT_FALSE(ShaderPool::isShaderBuildFailed(ShaderType::BLIT, BF18_PROPERTIES + variants - 1));
    for (int i = 0; i < variants; i++)
        ShaderPool::remove(ShaderType::BLIT, BF18_PROPERTIES + i);
}

// What the pool's lock costs (a draw's path asks for a post-processing variant, a load for every
// submesh's): nanoseconds per ShaderPool::get among 2000 variants, printed as [bench] lines: the
// map lookup without a lock, the same with an uncontended lock, and the pool's own (a variant
// still building, so its builder's call is in it). Run it on a quiet machine, in a RelWithDebInfo build: under a sanitizer or
// next to a build the numbers say nothing.
TEST(ShaderPoolBench, Get){
    constexpr int variants = 2000;
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr int lookups = 2000;
#else
    constexpr int lookups = 2000000;
#endif
    std::map<ShaderKey, std::shared_ptr<ShaderRender>> plain;
    std::vector<uint64_t> properties;
    for (int i = 0; i < variants; i++){
        properties.push_back(BF18_PROPERTIES + i);
        plain[ShaderPool::getShaderKey(ShaderType::BLIT, properties.back())] = std::make_shared<ShaderRender>();
    }
    BuilderScope builder(ShaderKey{});
    const auto measure = [&](const char* what, const std::function<bool(uint64_t)>& lookup){
        const auto start = std::chrono::steady_clock::now();
        int found = 0;
        for (int i = 0; i < lookups; i++)
            found += lookup(properties[(size_t)(i * 7) % properties.size()]) ? 1 : 0;
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / lookups;
        printf("[bench] %s: %.1f ns per lookup (%d found)\n", what, ns, found);
    };
    std::mutex mutex;
    for (int run = 0; run < 3; run++){
        measure("map, no lock", [&](uint64_t value){
            auto it = plain.find(ShaderPool::getShaderKey(ShaderType::BLIT, value));
            std::shared_ptr<ShaderRender> copy = it != plain.end() ? it->second : nullptr;
            return copy != nullptr;
        });
        measure("map, uncontended lock", [&](uint64_t value){
            std::lock_guard<std::mutex> lock(mutex);
            auto it = plain.find(ShaderPool::getShaderKey(ShaderType::BLIT, value));
            std::shared_ptr<ShaderRender> copy = it != plain.end() ? it->second : nullptr;
            return copy != nullptr;
        });
        measure("ShaderPool::get", [](uint64_t value){ return ShaderPool::get(ShaderType::BLIT, value) != nullptr; });
    }
    for (uint64_t value : properties)
        ShaderPool::remove(ShaderType::BLIT, value);
    EXPECT_NO_ENGINE_ERRORS();
}

// What the pool's lock costs on the path a draw takes: a variant that is made, asked for again.
// Printed as a [bench] line next to the one above; a build before the lock prints what it was.
TEST(ShaderPoolBenchGpu, GetOfAMadeVariant){
    JASPER_REQUIRE_GPU();
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr int lookups = 2000;
#else
    constexpr int lookups = 2000000;
#endif
    std::shared_ptr<ShaderRender> held = ShaderPool::get(ShaderType::BLIT, 0);
    ASSERT_TRUE(held);
    ASSERT_TRUE(GpuEnvironment::runFrames([&held]{ return held->isCreated(); }, 600));
    for (int run = 0; run < 3; run++){
        const auto start = std::chrono::steady_clock::now();
        int found = 0;
        for (int i = 0; i < lookups; i++)
            found += ShaderPool::get(ShaderType::BLIT, 0) == held ? 1 : 0;
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / lookups;
        printf("[bench] ShaderPool::get of a made variant: %.1f ns per lookup (%d found)\n", ns, found);
    }
    held.reset();
    ShaderPool::remove(ShaderType::BLIT, 0);
}

// What the lock on sokol's free lists costs (taken once for a GPU object made, once for one
// destroyed, never by a draw): a buffer's slot allocated and let go, nanoseconds per pair, printed
// as a [bench] line; a build before the lock prints what it was.
TEST(SokolPoolBenchGpu, AllocAndFree){
    JASPER_REQUIRE_GPU();
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr int pairs = 2000;
#else
    constexpr int pairs = 2000000;
#endif
    for (int run = 0; run < 3; run++){
        const auto start = std::chrono::steady_clock::now();
        int freed = 0;
        for (int i = 0; i < pairs; i++){
            const sg_buffer buffer = sg_alloc_buffer();
            if (buffer.id != SG_INVALID_ID){
                sg_dealloc_buffer(buffer);
                freed++;
            }
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / pairs;
        printf("[bench] a GPU pool's slot allocated and freed: %.1f ns per pair (%d freed)\n", ns, freed);
        EXPECT_EQ(freed, pairs);
    }
}

// A preview's load on its strand, every step as ProjectWindow takes it, while the main thread
// draws frames and, between them, frees: its own objects made for the purpose, a scene's meshes
// and textures and variants, and the previews the strand is done with while it loads the next.
TEST(ShaderPoolThreadsGpu, APreviewLoadsWhileTheMainThreadFreesGpuObjectsOutsideAFrame){
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
    std::mutex finishedMutex;
    std::deque<Preview*> finished;
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
            // the main thread frees it when it has drawn it: the next load does not wait for that
            std::lock_guard<std::mutex> lock(finishedMutex);
            finished.push_back(preview);
        }
        strandDone.store(true);
    });

    const auto freeFinishedPreview = [&](bool all){
        std::lock_guard<std::mutex> lock(finishedMutex);
        for (auto it = finished.begin(); it != finished.end();){
            if (all || !Engine::isSceneRunning(&(*it)->scene)){
                Engine::removeScene(&(*it)->scene);
                delete *it;
                it = finished.erase(it);
            }else{
                ++it;
            }
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (!strandDone.load()){
        if (std::chrono::steady_clock::now() > deadline){
            ADD_FAILURE() << "the strand's previews never finished";
            stop.store(true);
        }
        GpuEnvironment::drawFrames(1);

        // outside a frame from here on
        {
            float vertices[12] = {};
            BufferRender buffer;
            buffer.createBuffer(sizeof(vertices), vertices, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE);
            buffer.destroyBuffer();
        }
        {
            // a scene of its own, loaded by a frame and gone right after
            Scene other(EntityPool::System);
            Camera otherCamera(&other);
            other.setCamera(&otherCamera);
            otherCamera.setType(CameraType::CAMERA_PERSPECTIVE);
            otherCamera.setFramebufferSize(16, 16);
            otherCamera.setRenderToTexture(true);
            Primitive otherPlane(&other);
            otherPlane.createPlane(1.0f, 1.0f);
            // (the strand's model, for the same variants the strand asks for)
            Model otherModel(&other);
            EXPECT_TRUE(otherModel.loadModel(PREVIEW_MODEL));
            Engine::executeSceneOnce(&other);
            GpuEnvironment::drawFrames(1);
            Engine::removeScene(&other);
        }
        freeFinishedPreview(false);
    }
    strand.join();
    GpuEnvironment::drawFrames(2);
    freeFinishedPreview(true);
    GpuEnvironment::drawFrames(2);
    Engine::removeScene(&scene);
    // (no look at the engine's errors: TexturePoolThreadsTest.cpp says why)
}
