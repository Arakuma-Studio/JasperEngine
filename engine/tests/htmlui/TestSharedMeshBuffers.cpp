// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared immutable mesh buffers (MeshBufferPool) and full GPU pools (SokolSystem).
// Every Model of a file draws from one set of vertex and index buffers: thousands of copies of
// the skinned hero (tools/fixtures/pose-animation hero_animated.gltf), of a static glTF and of an
// OBJ all draw (one inked grid cell per copy, every mesh loaded) while the immutable vertex and
// index buffers made stay the same whatever the count (sokol's buffer pool once ran out at
// ~1,170 heroes, 7 buffers each, and the rest drew nothing). A reloaded model file gets new
// buffers and the old ones go with their last user; an edited buffer (a procedural mesh, a
// written copy of a model's bytes) gets one of its own and the others keep theirs; a full pool
// is logged once, its meshes count as failed (settled, not drawn) and load once there is room.
// sokol's per-frame buffers (uniforms, STREAM uploads, Vulkan descriptors) hold thousands of
// shadowed heroes, and what goes past one is skipped, counted and logged once.

#include "TestGoldenCommon.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/MeshBufferPool.h"
#include "render/ObjectRender.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"
#include "render/TextureRender.h"
#include "sokol/SokolSystem.h"
#include "sokol_gfx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::test;
namespace fs = std::filesystem;

namespace {

    const int screenWidth = 1280;
    const int screenHeight = 720;

    fs::path repoRoot(){
        return fs::weakly_canonical(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / "..");
    }

    std::string heroPath(){
        return (repoRoot() / "tools/fixtures/pose-animation/assets/models/hero_animated.gltf").generic_string();
    }

    fs::path tempDir(){
        const fs::path dir = fs::temp_directory_path() / ("jasper-shared-mesh-buffers-" + std::to_string(::getpid()));
        fs::create_directories(dir);
        return dir;
    }

    std::string base64(const std::vector<uint8_t>& bytes){
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        size_t i = 0;
        for (; i + 2 < bytes.size(); i += 3){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += table[v & 63];
        }
        if (i + 1 == bytes.size()){
            const uint32_t v = uint32_t(bytes[i]) << 16;
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += "==";
        }else if (i + 2 == bytes.size()){
            const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
            out += table[(v >> 18) & 63]; out += table[(v >> 12) & 63]; out += table[(v >> 6) & 63]; out += '=';
        }
        return out;
    }

    // a static quad facing +z over x [-halfWidth, halfWidth], y [0, height]: positions,
    // normals and indices in 3 buffer views; written to `path`
    void writeQuadGltf(const std::string& path, float halfWidth, float height){
        const std::vector<float> positions = {-halfWidth, 0, 0, halfWidth, 0, 0, halfWidth, height, 0, -halfWidth, height, 0};
        const std::vector<float> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        const std::vector<uint16_t> indices = {0, 1, 2, 0, 2, 3};
        std::vector<uint8_t> data;
        auto add = [&](const void* bytes, size_t size){
            const size_t offset = (data.size() + 3) / 4 * 4;
            data.resize(offset + size, 0);
            std::memcpy(data.data() + offset, bytes, size);
            return offset;
        };
        const size_t p = add(positions.data(), positions.size() * 4);
        const size_t n = add(normals.data(), normals.size() * 4);
        const size_t i = add(indices.data(), indices.size() * 2);
        std::ostringstream json;
        json << "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":\"quad\"}],"
             << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"material\":0}]}],"
             << "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.9,0.8,0.3,1],\"metallicFactor\":0,\"roughnessFactor\":1}}],"
             << "\"buffers\":[{\"byteLength\":" << data.size() << ",\"uri\":\"data:application/octet-stream;base64," << base64(data) << "\"}],"
             << "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":" << p << ",\"byteLength\":48,\"target\":34962},"
             << "{\"buffer\":0,\"byteOffset\":" << n << ",\"byteLength\":48,\"target\":34962},"
             << "{\"buffer\":0,\"byteOffset\":" << i << ",\"byteLength\":12,\"target\":34963}],"
             << "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[" << -halfWidth << ",0,0],\"max\":["
             << halfWidth << "," << height << ",0]},{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
             << "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}";
        std::ofstream(path, std::ios::trunc) << json.str();
    }

    // the same quad as an OBJ
    void writeQuadObj(const std::string& path, float halfWidth, float height){
        std::ofstream file(path, std::ios::trunc);
        file << "v " << -halfWidth << " 0 0\nv " << halfWidth << " 0 0\nv " << halfWidth << " " << height << " 0\nv " << -halfWidth << " " << height << " 0\n"
             << "vn 0 0 1\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n";
    }

    // immutable vertex and index buffers made at the graphics API while it is alive
    class MeshBufferAllocations {
    public:
        MeshBufferAllocations(){
            sg_trace_hooks hooks = {};
            hooks.user_data = this;
            hooks.make_buffer = [](const sg_buffer_desc* desc, sg_buffer, void* data){
                if (desc->usage.immutable && (desc->usage.vertex_buffer || desc->usage.index_buffer))
                    ++static_cast<MeshBufferAllocations*>(data)->count;
            };
            previous = sg_install_trace_hooks(&hooks);
        }
        ~MeshBufferAllocations(){ sg_install_trace_hooks(&previous); }
        MeshBufferAllocations(const MeshBufferAllocations&) = delete;
        MeshBufferAllocations& operator=(const MeshBufferAllocations&) = delete;

        size_t made() const{ return count; }

    private:
        sg_trace_hooks previous = {};
        size_t count = 0;
    };

    // N copies on a grid in the x-y plane, seen by an orthographic camera down -z: copy i in cell
    // (i % columns, i / columns), each cell `cellWidth` x `cellHeight` world units
    struct GridScene {
        SceneFixture fixture;
        Camera camera;
        int columns = 1;
        int rows = 1;
        float cellWidth = 2.0f;
        float cellHeight = 2.4f;
        float pixelsPerUnit = 1.0f;

        GridScene(int count, float cellWidth, float cellHeight): camera(&fixture.getScene()), cellWidth(cellWidth), cellHeight(cellHeight){
            // columns for cells about as wide on screen as the view's aspect asks
            const float aspect = float(screenWidth) / float(screenHeight);
            columns = std::max(1, int(std::ceil(std::sqrt(count * aspect * cellHeight / cellWidth))));
            rows = (count + columns - 1) / columns;
            const float width = columns * cellWidth;
            const float height = rows * cellHeight;
            pixelsPerUnit = std::min(screenWidth / width, screenHeight / height);
            const float halfW = screenWidth * 0.5f / pixelsPerUnit;
            const float halfH = screenHeight * 0.5f / pixelsPerUnit;
            camera.setOrtho(-halfW, halfW, -halfH, halfH, 0.1f, 1000.0f);
            camera.setPosition(width * 0.5f, height * 0.5f, 100.0f);
            camera.setTarget(width * 0.5f, height * 0.5f, 0.0f);
            camera.activate();
            scene().setGlobalIllumination(0.6f);
            scene().setMeshLodEnabled(false);
            light = &fixture.add<Light>();
            light->setType(LightType::DIRECTIONAL);
            light->setDirection(-0.2f, -0.3f, -1.0f);
            light->setIntensity(2.0f);
            light->setShadows(false);
        }

        Light* light = nullptr;

        Scene& scene(){ return fixture.getScene(); }

        // where copy i stands (the bottom middle of its cell)
        Vector3 position(int i) const{
            return Vector3((i % columns + 0.5f) * cellWidth, (i / columns) * cellHeight + 0.1f * cellHeight, 0.0f);
        }

        PixelRect cell(int i) const{
            const int column = i % columns;
            const int row = i / columns;
            const float left = screenWidth * 0.5f - (columns * cellWidth * 0.5f - column * cellWidth) * pixelsPerUnit;
            const float bottom = screenHeight * 0.5f + (rows * cellHeight * 0.5f - row * cellHeight) * pixelsPerUnit;
            return {int(std::ceil(left)), int(std::ceil(bottom - cellHeight * pixelsPerUnit)),
                    int(std::floor(left + cellWidth * pixelsPerUnit)), int(std::floor(bottom))};
        }

        // frames until every mesh with submeshes is loaded (or failed), then 2 more
        bool settle(int maxFrames = 120){
            Scene* current = &scene();
            int extra = 0;
            return GpuEnvironment::runFrames([current, &extra](){
                if (!current->getSystem<RenderSystem>()->isAllLoaded())
                    return false;
                auto meshes = current->getComponentArray<MeshComponent>();
                for (size_t i = 0; i < meshes->size(); i++){
                    const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                    if (mesh.numSubmeshes > 0 && !mesh.loadFailed && (!mesh.loaded || mesh.needReload || mesh.needVariantReload))
                        return false;
                }
                return ++extra > 2;
            }, maxFrames);
        }

        // every skinned mesh uploads its bone matrices this frame, as an animated crowd does
        // each frame (a still one uploads them once); returns how many
        int dirtyBones(){
            int skinned = 0;
            auto meshes = scene().getComponentArray<MeshComponent>();
            for (size_t i = 0; i < meshes->size(); i++){
                MeshComponent& mesh = meshes->getComponentFromIndex(i);
                if (mesh.loaded && mesh.bonesMatrix.stored() > 0){
                    mesh.needUpdateBones = true;
                    skinned++;
                }
            }
            return skinned;
        }

        // meshes with geometry: loaded, failed
        void countMeshes(size_t& loaded, size_t& failed){
            loaded = failed = 0;
            auto meshes = scene().getComponentArray<MeshComponent>();
            for (size_t i = 0; i < meshes->size(); i++){
                const MeshComponent& mesh = meshes->getComponentFromIndex(i);
                if (mesh.numSubmeshes == 0)
                    continue;
                loaded += mesh.loaded ? 1 : 0;
                failed += mesh.loadFailed ? 1 : 0;
            }
        }

        // copies [first, last) whose cell has no ink
        std::vector<int> blankCells(const Screen& screen, int first, int last) const{
            std::vector<int> blank;
            for (int i = first; i < last; i++){
                if (screen.inkOnBackground(cell(i), 16).count < 2)
                    blank.push_back(i);
            }
            return blank;
        }
    };

    std::string errorLines(){
        std::string text;
        for (const auto& line : TestSystem::get().getLogs()){
            if (line.type == D_LOG_ERROR)
                text += line.message + "\n";
        }
        return text;
    }

    std::string firstOf(const std::vector<int>& values){
        std::ostringstream out;
        for (size_t i = 0; i < values.size() && i < 10; i++)
            out << (i ? ", " : "") << values[i];
        if (values.size() > 10)
            out << ", ...";
        return out.str();
    }

    class SharedMeshBuffersTest : public ::testing::Test {
    protected:
        void SetUp() override{
            JASPER_REQUIRE_GPU();
            GpuEnvironment::setView(screenWidth, screenHeight, screenWidth, screenHeight, Scaling::FITWIDTH);
            TestSystem::get().clearLogs();
        }
    };

    // Loads copies [first, last) of `path`, placed in their cells. A `models` declared before the
    // scene outlives it (the scene's teardown destroys their entities with the others).
    void addCopies(GridScene& s, const std::string& path, int first, int last, std::vector<std::unique_ptr<Model>>& models){
        for (int i = first; i < last; i++){
            models.emplace_back(new Model(&s.scene()));
            Model& model = *models.back();
            ASSERT_TRUE(model.loadModel(path)) << path;
            model.setPosition(s.position(i));
        }
    }

    // A grid scene of thousands of copies, all destroyed when the test ends: each Model with its
    // nodes, one entity after another, then the scene (O(1) an entity: a crowd of 150,000 once
    // took hours to go, each one erased from every component array in turn)
    class Crowd {
    public:
        Crowd(int count, float cellWidth, float cellHeight): grid(new GridScene(count, cellWidth, cellHeight)){}
        ~Crowd(){
            using Clock = std::chrono::steady_clock;
            const size_t entities = grid->scene().getEntityList().size();
            const Clock::time_point start = Clock::now();
            copies.clear();
            const Clock::time_point modelsGone = Clock::now();
            grid.reset();
            const Clock::time_point sceneGone = Clock::now();
            std::printf("[crowd] %zu entities destroyed: the Models with their nodes in %.3f s, then the scene in %.3f s\n", entities,
                        std::chrono::duration<double>(modelsGone - start).count(), std::chrono::duration<double>(sceneGone - modelsGone).count());
        }
        Crowd(const Crowd&) = delete;
        Crowd& operator=(const Crowd&) = delete;

        std::vector<std::unique_ptr<Model>>& models(){ return copies; }
        GridScene& scene(){ return *grid; }

    private:
        std::unique_ptr<GridScene> grid;
        std::vector<std::unique_ptr<Model>> copies;
    };

    // the sokol id of every GPU buffer a mesh draws from (vertex, index, external)
    std::set<uint32_t> meshBufferIds(MeshComponent& mesh){
        std::set<uint32_t> ids;
        if (mesh.buffer.getSize() > 0) ids.insert(mesh.buffer.getRender()->backend.get().id);
        if (mesh.indices.getSize() > 0) ids.insert(mesh.indices.getRender()->backend.get().id);
        for (unsigned int b = 0; b < mesh.numExternalBuffers; b++)
            ids.insert(mesh.eBuffers[b].getRender()->backend.get().id);
        return ids;
    }

}

// 3,300 skinned heroes: every one draws, and the copies after the first 300 make no vertex or
// index buffer (the first ones made the hero's 7)
TEST_F(SharedMeshBuffersTest, ThousandsOfSkinnedModelCopiesAllDrawFromOneSetOfBuffers){
    const int firstBatch = 300;
    const int count = 3300;
    Crowd crowd(count, 2.0f, 2.4f);
    std::vector<std::unique_ptr<Model>>& models = crowd.models();
    GridScene& s = crowd.scene();
    const size_t poolBefore = MeshBufferPool::getBufferCount();
    const uint64_t exhaustionsBefore = SystemRender::getResourcePoolExhaustions();

    MeshBufferAllocations first;
    addCopies(s, heroPath(), 0, firstBatch, models);
    ASSERT_TRUE(s.settle());
    const size_t madeByFirst = first.made();
    EXPECT_GE(madeByFirst, 7u) << "the hero's 7 buffer views";
    EXPECT_LE(madeByFirst, 12u) << "one set for every copy, not one per copy";
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 7);
    const int buffersInUse = SystemRender::getResourcesInUse(GpuResourceKind::BUFFER);

    size_t madeByRest = 0;
    {
        MeshBufferAllocations rest;
        addCopies(s, heroPath(), firstBatch, count, models);
        ASSERT_TRUE(s.settle(200));
        madeByRest = rest.made();
    }
    EXPECT_EQ(madeByRest, 0u) << "3,000 more copies made no vertex or index buffer";
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 7);
    // at most a buffer of its own per copy: the bones storage buffer where the backend has one
    EXPECT_LE(SystemRender::getResourcesInUse(GpuResourceKind::BUFFER) - buffersInUse, count - firstBatch);

    size_t loaded = 0, failed = 0;
    s.countMeshes(loaded, failed);
    EXPECT_EQ(loaded, size_t(count)) << "every hero's skinned mesh loaded";
    EXPECT_EQ(failed, 0u);
    EXPECT_EQ(SystemRender::getResourcePoolExhaustions(), exhaustionsBefore) << "no GPU pool ran out";
    // 3 materials: 3 draws a hero
    EXPECT_GE(Engine::getFrameStats().drawCalls, uint32_t(count * 3));
    // their uniform blocks fit the frame's uniform buffer (Vulkan, Metal, WebGPU drop the draws
    // past it): counted on every backend
    EXPECT_GT(SystemRender::getUniformBytesLastFrame(), 0u);
    EXPECT_LE(SystemRender::getUniformBytesLastFrame(), SystemRender::getUniformBytesLimit());
    EXPECT_EQ(SystemRender::getUniformSkippedDraws(), 0u);
    // and their bone matrices, all uploaded in one frame as an animated crowd does, the frame's
    // stream staging buffer (Vulkan skips the uploads past it: sokol's default 16 MB held 2,348
    // of these heroes, and the 952 others, still, never got their bones and drew nothing), the
    // draws' descriptor sets the descriptor buffer (Vulkan)
    EXPECT_GE(s.dirtyBones(), count) << "a skinned mesh or more a hero";
    GpuEnvironment::drawFrames(1);
    EXPECT_GT(SystemRender::getStreamBytesLastFrame(), 0u);
    EXPECT_LE(SystemRender::getStreamBytesLastFrame(), SystemRender::getStreamBytesLimit());
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), 0u);
    EXPECT_LE(SystemRender::getDescriptorBytesLastFrame(), SystemRender::getDescriptorBytesLimit());
    EXPECT_EQ(SystemRender::getDescriptorSkippedDraws(), 0u);

    Screen screen;
    ASSERT_TRUE(screen.read());
    const std::vector<int> blank = s.blankCells(screen, 0, count);
    EXPECT_TRUE(blank.empty()) << blank.size() << " of " << count << " heroes not drawn: " << firstOf(blank);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}

// sokol's per-frame buffers (Engine::GpuResourceLimits), measured with skinned heroes in
// shadow-casting light: what one hero costs a frame in each, and that the desktop defaults hold
// 4,000 of them. The uniform blocks (each rounded up to 256 bytes, as the uniform buffer of
// Vulkan, Metal and WebGPU takes them; 3,300 shadowed heroes overflowed sokol's default 4 MB on
// Vulkan and dropped draws), the STREAM uploads (each hero's bone matrices, once a frame,
// staged on Vulkan: 3,300 heroes overflowed sokol's default 16 MB stream staging buffer and 952
// of them drew without bones) and, on Vulkan, the descriptor sets of every draw.
TEST_F(SharedMeshBuffersTest, PerFrameBuffersHoldFourThousandShadowedSkinnedHeroes){
    const int count = 500;
    Crowd crowd(count, 2.0f, 2.4f);
    GridScene& s = crowd.scene();
    s.light->setShadows(true);
    const uint64_t uniformSkippedBefore = SystemRender::getUniformSkippedDraws();
    const uint64_t streamSkippedBefore = SystemRender::getStreamSkippedUploads();
    const uint64_t descriptorSkippedBefore = SystemRender::getDescriptorSkippedDraws();
    ASSERT_TRUE(s.settle());
    GpuEnvironment::drawFrames(1);
    const uint64_t emptyUniforms = SystemRender::getUniformBytesLastFrame();
    const uint64_t emptyStream = SystemRender::getStreamBytesLastFrame();
    const uint64_t emptyDescriptors = SystemRender::getDescriptorBytesLastFrame();
    addCopies(s, heroPath(), 0, count, crowd.models());
    ASSERT_TRUE(s.settle());
    GpuEnvironment::drawFrames(1);
    // an animated crowd: every hero uploads its bones every frame
    EXPECT_GE(s.dirtyBones(), count) << "a skinned mesh or more a hero";
    GpuEnvironment::drawFrames(1);
    const Engine::GpuResourceLimits defaults;

    const uint64_t uniforms = SystemRender::getUniformBytesLastFrame();
    ASSERT_GT(uniforms, emptyUniforms);
    const double uniformsPerHero = double(uniforms - emptyUniforms) / count;
    std::printf("[uniforms] %d shadowed skinned heroes: %llu bytes a frame (%.0f a hero, %llu without them), limit %llu\n", count,
                (unsigned long long)uniforms, uniformsPerHero, (unsigned long long)emptyUniforms, (unsigned long long)SystemRender::getUniformBytesLimit());
    // a hero draws 3 submeshes in the colour pass and the shadow cascades
    EXPECT_GT(uniformsPerHero, 3 * 256.0);
    EXPECT_LE(emptyUniforms + uniformsPerHero * 4000.0, double(SystemRender::getUniformBytesLimit())) << "4,000 shadowed heroes fit";
    EXPECT_EQ(SystemRender::getUniformBytesLimit(), uint64_t(defaults.uniformBytes));
    EXPECT_EQ(SystemRender::getUniformSkippedDraws(), uniformSkippedBefore);

    const uint64_t stream = SystemRender::getStreamBytesLastFrame();
    ASSERT_GT(stream, emptyStream);
    const double streamPerHero = double(stream - emptyStream) / count;
    std::printf("[stream] %d shadowed skinned heroes: %llu bytes a frame (%.0f a hero, %llu without them), limit %llu\n", count,
                (unsigned long long)stream, streamPerHero, (unsigned long long)emptyStream, (unsigned long long)SystemRender::getStreamBytesLimit());
    // the hero's bone matrices (64 bytes each), uploaded once a frame whatever the passes
    EXPECT_GE(streamPerHero, 64.0);
    EXPECT_EQ(std::fmod(streamPerHero, 16.0), 0.0) << "each upload rounded up to 16 bytes";
    EXPECT_LE(emptyStream + streamPerHero * 4000.0, double(SystemRender::getStreamBytesLimit())) << "4,000 skinned heroes fit";
    EXPECT_EQ(SystemRender::getStreamBytesLimit(), uint64_t(defaults.streamStagingBytes));
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), streamSkippedBefore);

    const uint64_t descriptors = SystemRender::getDescriptorBytesLastFrame();
    std::printf("[descriptors] %d shadowed skinned heroes: %llu bytes a frame (%.0f a hero, %llu without them), limit %llu\n", count,
                (unsigned long long)descriptors, double(descriptors - std::min(descriptors, emptyDescriptors)) / count,
                (unsigned long long)emptyDescriptors, (unsigned long long)SystemRender::getDescriptorBytesLimit());
    EXPECT_EQ(SystemRender::getDescriptorBytesLimit(), uint64_t(defaults.descriptorBytes));
    if (SystemRender::hasDescriptorBufferLimit()){
        ASSERT_GT(descriptors, emptyDescriptors);
        const double descriptorsPerHero = double(descriptors - emptyDescriptors) / count;
        EXPECT_LE(emptyDescriptors + descriptorsPerHero * 4000.0, double(SystemRender::getDescriptorBytesLimit())) << "4,000 shadowed heroes fit";
    }else{
        EXPECT_EQ(descriptors, 0u) << "no descriptor buffer on this backend";
    }
    EXPECT_EQ(SystemRender::getDescriptorSkippedDraws(), descriptorSkippedBefore);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}

// A frame's STREAM uploads past the stream staging buffer (a limit made small for the test,
// enforced on any backend as on Vulkan): the uploads that do not fit are skipped (each frame the
// same ones), counted, and the first such frame is logged once as an error with the bytes
// needed, the limit and the setting to raise; sokol never sees an upload it would drop. Back
// to the configured limit, nothing is skipped.
TEST_F(SharedMeshBuffersTest, StreamUploadsPastTheStagingBufferAreSkippedCountedAndLoggedOnce){
    const int count = 40;
    Crowd crowd(count, 2.0f, 2.4f);
    GridScene& s = crowd.scene();
    ASSERT_TRUE(s.settle());
    GpuEnvironment::drawFrames(1);
    const uint64_t empty = SystemRender::getStreamBytesLastFrame();
    addCopies(s, heroPath(), 0, count, crowd.models());
    ASSERT_TRUE(s.settle());
    // an animated crowd: every hero uploads its bones every frame
    auto animatedFrame = [&s](){
        s.dirtyBones();
        GpuEnvironment::drawFrames(1);
    };
    animatedFrame();
    const uint64_t full = SystemRender::getStreamBytesLastFrame();
    ASSERT_GT(full, empty);
    const uint64_t perHero = (full - empty) / count;
    ASSERT_GT(perHero, 0u);

    // room for 10 heroes' bones (and what the frame streams without them)
    const uint64_t limit = empty + perHero * 10 + perHero / 2;
    TestSystem::get().clearLogs();
    SokolSystem::overrideBudget(SokolSystem::STREAM_BUDGET, limit, true);
    const uint64_t before = SystemRender::getStreamSkippedUploads();
    animatedFrame();
    const uint64_t firstFrame = SystemRender::getStreamSkippedUploads() - before;
    animatedFrame();
    const uint64_t secondFrame = SystemRender::getStreamSkippedUploads() - before - firstFrame;
    animatedFrame();
    const uint64_t needed = SystemRender::getStreamBytesLastFrame();
    SokolSystem::restoreBudgets();

    EXPECT_GE(firstFrame, uint64_t(count - 11));
    EXPECT_LE(firstFrame, uint64_t(count - 10));
    EXPECT_EQ(secondFrame, firstFrame) << "the same uploads skipped every frame";
    EXPECT_EQ(needed, full) << "what the frame needed counts the skipped uploads";
    const std::string message = "GPU stream staging buffer full: a frame needed " + std::to_string(full) +
                                " bytes of STREAM buffer and texture uploads, the per-frame buffer holds " + std::to_string(limit);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, message), 1u) << errorLines();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "Engine::GpuResourceLimits::streamStagingBytes"), 1u) << "names the setting";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 1u) << "logged once, not every frame or upload: " << errorLines();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "STAGING_STREAM_BUFFER_OVERFLOW"), 0u) << "sokol never overflows";

    const uint64_t restored = SystemRender::getStreamSkippedUploads();
    animatedFrame();
    animatedFrame();
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), restored) << "nothing skipped at the configured limit";
    EXPECT_EQ(SystemRender::getStreamBytesLastFrame(), full);

    // a still crowd uploads its bones once: one refused is uploaded the next frame, not never
    SokolSystem::overrideBudget(SokolSystem::STREAM_BUDGET, limit, true);
    s.dirtyBones();
    GpuEnvironment::drawFrames(1);
    const uint64_t refused = SystemRender::getStreamSkippedUploads() - restored;
    EXPECT_GE(refused, uint64_t(count - 11));
    GpuEnvironment::drawFrames(1);
    EXPECT_GT(SystemRender::getStreamBytesLastFrame(), empty) << "the refused heroes upload their bones the next frame";
    SokolSystem::restoreBudgets();
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(SystemRender::getStreamBytesLastFrame(), empty) << "every hero has its bones, nothing left to upload";
    int pending = 0;
    auto meshes = s.scene().getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++)
        pending += meshes->getComponentFromIndex(i).needUpdateBones ? 1 : 0;
    EXPECT_EQ(pending, 0);
}

// A STREAM texture update past the stream staging buffer keeps the texture's content (the
// update is dropped whole, not half-staged) and is counted; one that fits goes through.
TEST_F(SharedMeshBuffersTest, StreamTextureUpdatePastTheStagingBufferKeepsTheTexture){
    const int size = 16;
    TextureRender texture;
    ASSERT_TRUE(texture.createDynamicTexture("stream-budget-texture", size, size, ColorFormat::RGBA, TextureFilter::NEAREST));
    std::vector<uint8_t> first(size * size * 4), second(size * size * 4);
    for (size_t i = 0; i < first.size(); i++){
        first[i] = uint8_t(i * 7);
        second[i] = uint8_t(255 - i * 3);
    }
    texture.updateTexture(first.data(), first.size());
    GpuEnvironment::drawFrames(1);
    EXPECT_GE(SystemRender::getStreamBytesLastFrame(), uint64_t(first.size()));

    TestSystem::get().clearLogs();
    const uint64_t before = SystemRender::getStreamSkippedUploads();
    SokolSystem::overrideBudget(SokolSystem::STREAM_BUDGET, first.size() - 1, true);
    texture.updateTexture(second.data(), second.size());
    GpuEnvironment::drawFrames(1);
    SokolSystem::restoreBudgets();
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), before + 1);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "GPU stream staging buffer full"), 1u) << errorLines();
    std::vector<uint8_t> pixels;
    ASSERT_TRUE(readTexturePixels(texture, size, size, 4, pixels));
    EXPECT_EQ(pixels, first) << "the skipped update left the texture as it was";

    texture.updateTexture(second.data(), second.size());
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(SystemRender::getStreamSkippedUploads(), before + 1);
    ASSERT_TRUE(readTexturePixels(texture, size, size, 4, pixels));
    EXPECT_EQ(pixels, second);
    texture.destroyTexture();
}

// Draws past the uniform buffer (a limit of 0 for the test, enforced on any backend as on
// Vulkan, Metal and WebGPU) and, on Vulkan, past the descriptor buffer: skipped (not drawn with
// another draw's data, not handed to sokol to drop), counted, and logged once each.
TEST_F(SharedMeshBuffersTest, DrawsPastTheUniformOrDescriptorBufferAreSkippedCountedAndLoggedOnce){
    const int count = 12;
    Crowd crowd(count, 2.0f, 2.4f);
    GridScene& s = crowd.scene();
    addCopies(s, heroPath(), 0, count, crowd.models());
    ASSERT_TRUE(s.settle());

    TestSystem::get().clearLogs();
    const uint64_t uniformBefore = SystemRender::getUniformSkippedDraws();
    SokolSystem::overrideBudget(SokolSystem::UNIFORM_BUDGET, 0, true);
    GpuEnvironment::drawFrames(2);
    Screen none;
    ASSERT_TRUE(none.read());
    SokolSystem::restoreBudgets();
    EXPECT_GE(SystemRender::getUniformSkippedDraws() - uniformBefore, uint64_t(count * 3 * 2)) << "3 draws a hero, 2 frames";
    EXPECT_EQ(s.blankCells(none, 0, count).size(), size_t(count)) << "no hero drawn";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "GPU uniform buffer full"), 1u) << errorLines();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "Engine::GpuResourceLimits::uniformBytes"), 1u);

    GpuEnvironment::drawFrames(1);
    Screen all;
    ASSERT_TRUE(all.read());
    EXPECT_TRUE(s.blankCells(all, 0, count).empty()) << "drawn again at the configured limit";

    // Vulkan: the descriptor sets of a few draws only
    TestSystem::get().clearLogs();
    const uint64_t descriptorBefore = SystemRender::getDescriptorSkippedDraws();
    SokolSystem::overrideBudget(SokolSystem::DESCRIPTOR_BUDGET, 1024, true);
    GpuEnvironment::drawFrames(2);
    SokolSystem::restoreBudgets();
    if (SystemRender::hasDescriptorBufferLimit()){
        EXPECT_GT(SystemRender::getDescriptorSkippedDraws(), descriptorBefore);
        EXPECT_GT(SystemRender::getDescriptorBytesLastFrame(), 1024u) << "what the frame needed";
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "GPU descriptor buffer full"), 1u) << errorLines();
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "DESCRIPTOR_BUFFER_OVERFLOW"), 0u) << "sokol never overflows";
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 1u) << errorLines();
    }else{
        EXPECT_EQ(SystemRender::getDescriptorSkippedDraws(), descriptorBefore) << "no descriptor buffer on this backend";
        EXPECT_EQ(SystemRender::getDescriptorBytesLastFrame(), 0u);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
    }
    const uint64_t restored = SystemRender::getDescriptorSkippedDraws();
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(SystemRender::getDescriptorSkippedDraws(), restored);
}

// 3,000 copies of a static glTF and 3,000 of an OBJ: one vertex (or 2 for the glTF's positions
// and normals) and one index buffer each, every copy drawn
TEST_F(SharedMeshBuffersTest, ThousandsOfStaticGltfAndObjCopiesAllDrawFromOneSetOfBuffers){
    const int perFile = 3000;
    const std::string gltf = (tempDir() / "static_quad.gltf").string();
    const std::string obj = (tempDir() / "static_quad.obj").string();
    writeQuadGltf(gltf, 0.6f, 1.6f);
    writeQuadObj(obj, 0.6f, 1.6f);

    Crowd crowd(perFile * 2, 1.6f, 2.0f);
    std::vector<std::unique_ptr<Model>>& models = crowd.models();
    GridScene& s = crowd.scene();
    const size_t poolBefore = MeshBufferPool::getBufferCount();
    MeshBufferAllocations allocations;
    addCopies(s, gltf, 0, 10, models);
    addCopies(s, obj, perFile, perFile + 10, models);
    ASSERT_TRUE(s.settle());
    const size_t madeByFirst = allocations.made();
    EXPECT_EQ(madeByFirst, 5u) << "the glTF's 3 buffer views, the OBJ's vertices and indices";
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 5);

    addCopies(s, gltf, 10, perFile, models);
    addCopies(s, obj, perFile + 10, perFile * 2, models);
    ASSERT_TRUE(s.settle(200));
    EXPECT_EQ(allocations.made(), madeByFirst) << "5,980 more copies made no vertex or index buffer";
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 5);

    size_t loaded = 0, failed = 0;
    s.countMeshes(loaded, failed);
    EXPECT_EQ(loaded, size_t(perFile * 2));
    EXPECT_EQ(failed, 0u);
    EXPECT_GE(Engine::getFrameStats().drawCalls, uint32_t(perFile * 2));

    Screen screen;
    ASSERT_TRUE(screen.read());
    const std::vector<int> blank = s.blankCells(screen, 0, perFile * 2);
    EXPECT_TRUE(blank.empty()) << blank.size() << " copies not drawn: " << firstOf(blank);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}

// A model file changed on disk (MeshSystem::invalidateModelFile, as the editor does): the copies
// reloaded draw the new geometry from new shared buffers; the old ones stay while a copy still
// shows the old parse and are destroyed with the last of them
TEST_F(SharedMeshBuffersTest, ReloadedModelFileReleasesAndRecreatesTheSharedBuffers){
    const std::string path = (tempDir() / "reloaded_quad.gltf").string();
    writeQuadGltf(path, 0.2f, 1.6f); // narrow
    const int count = 40;
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene)
    GridScene s(count, 2.0f, 2.0f);
    const size_t poolBefore = MeshBufferPool::getBufferCount();
    addCopies(s, path, 0, count, models);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 3);
    const std::set<uint32_t> oldIds = meshBufferIds(models[0]->getComponent<MeshComponent>());
    ASSERT_EQ(oldIds.size(), 3u);
    for (auto& model : models)
        EXPECT_EQ(meshBufferIds(model->getComponent<MeshComponent>()), oldIds) << "every copy draws the same buffers";

    Screen before;
    ASSERT_TRUE(before.read());
    const int narrow = before.inkOnBackground(s.cell(0), 16).width();
    ASSERT_GT(narrow, 0);

    writeQuadGltf(path, 0.8f, 1.6f); // wide
    MeshSystem::invalidateModelFile(path);
    for (int i = 0; i < count / 2; i++)
        ASSERT_TRUE(models[i]->loadModel(path));
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 6) << "the old parse's buffers stay while copies show it";
    const std::set<uint32_t> newIds = meshBufferIds(models[0]->getComponent<MeshComponent>());
    ASSERT_EQ(newIds.size(), 3u);
    for (uint32_t id : newIds)
        EXPECT_EQ(oldIds.count(id), 0u) << "the reloaded file draws from new buffers";
    EXPECT_EQ(meshBufferIds(models[count - 1]->getComponent<MeshComponent>()), oldIds);

    for (int i = count / 2; i < count; i++)
        ASSERT_TRUE(models[i]->loadModel(path));
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 3) << "the old buffers went with their last user";
    for (uint32_t id : oldIds)
        EXPECT_NE(sg_query_buffer_state(sg_buffer{id}), SG_RESOURCESTATE_VALID) << "old buffer " << id << " destroyed";
    for (auto& model : models)
        EXPECT_EQ(meshBufferIds(model->getComponent<MeshComponent>()), newIds);

    Screen after;
    ASSERT_TRUE(after.read());
    for (int i = 0; i < count; i++){
        const int width = after.inkOnBackground(s.cell(i), 16).width();
        EXPECT_GT(width, narrow + 4) << "copy " << i << " draws the new, wider quad";
    }

    // all copies gone: so are the buffers
    for (auto& model : models)
        GameObject::destroyImmediate(*model);
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore);
    for (uint32_t id : newIds)
        EXPECT_NE(sg_query_buffer_state(sg_buffer{id}), SG_RESOURCESTATE_VALID);
}

// Edits: a procedural mesh edited leaves an identical one alone, and a copy of a model whose
// bytes are written gets a buffer of its own (copy on write) while the other copies keep the
// shared one and draw unchanged
TEST_F(SharedMeshBuffersTest, EditingOneMeshDoesNotAffectAnother){
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene)
    GridScene s(4, 3.0f, 3.0f);

    Primitive& a = s.fixture.add<Primitive>();
    Primitive& b = s.fixture.add<Primitive>();
    a.createWall(1.0f, 1.0f);
    b.createWall(1.0f, 1.0f);
    a.setPosition(s.position(0) + Vector3(0, 1.0f, 0));
    b.setPosition(s.position(1) + Vector3(0, 1.0f, 0));
    ASSERT_TRUE(s.settle());
    Screen before;
    ASSERT_TRUE(before.read());
    const InkBox bBefore = before.inkOnBackground(s.cell(1), 16);
    ASSERT_FALSE(bBefore.empty());
    EXPECT_NE(a.getComponent<MeshComponent>().buffer.getRender()->backend.get().id,
              b.getComponent<MeshComponent>().buffer.getRender()->backend.get().id) << "procedural meshes are not shared";

    a.createWall(2.5f, 2.5f); // edited
    const std::string path = (tempDir() / "edited_quad.gltf").string();
    writeQuadGltf(path, 0.6f, 1.6f);
    addCopies(s, path, 2, 4, models);
    ASSERT_TRUE(s.settle());
    Screen edited;
    ASSERT_TRUE(edited.read());
    EXPECT_GT(edited.inkOnBackground(s.cell(0), 16).width(), before.inkOnBackground(s.cell(0), 16).width() + 4) << "the edited wall grew";
    const InkBox bAfter = edited.inkOnBackground(s.cell(1), 16);
    EXPECT_EQ(bAfter.x0, bBefore.x0);
    EXPECT_EQ(bAfter.x1, bBefore.x1);
    EXPECT_EQ(bAfter.y0, bBefore.y0);
    EXPECT_EQ(bAfter.y1, bBefore.y1);

    MeshComponent& first = models[0]->getComponent<MeshComponent>();
    MeshComponent& second = models[1]->getComponent<MeshComponent>();
    ASSERT_EQ(meshBufferIds(first), meshBufferIds(second));
    const InkBox secondBefore = edited.inkOnBackground(s.cell(3), 16);
    const int firstWidth = edited.inkOnBackground(s.cell(2), 16).width();

    // the first copy's positions: its own copy of the bytes (still the same bytes: still shared),
    // then written (an editor edit): x doubled
    int positions = -1;
    ASSERT_EQ(first.submeshes[0].attributes.count(AttributeType::POSITION), 1u);
    const std::string positionBuffer = first.submeshes[0].attributes[AttributeType::POSITION].getBufferName();
    for (unsigned int b = 0; b < first.numExternalBuffers; b++){
        if (first.eBuffers[b].getName() == positionBuffer){
            positions = (int)b;
            break;
        }
    }
    ASSERT_GE(positions, 0);
    ExternalBuffer& written = first.eBuffers[positions];
    written.copyToOwnData();
    EXPECT_TRUE(MeshBufferPool::holdsSource(*written.getRender(), written)) << "a copy of the same bytes still shares";
    Attribute position(AttributeDataType::FLOAT, "", 3, 0, 0, true, false);
    written.setStride(12);
    for (unsigned int v = 0; v < 4; v++){
        Vector3 p = written.getVector3(&position, v);
        written.setVector3(v, &position, Vector3(p.x * 2.0f, p.y, p.z));
    }
    EXPECT_FALSE(written.hasShareSource()) << "a write drops the share source";
    first.needReload = true;
    ASSERT_TRUE(s.settle());

    EXPECT_FALSE(written.getRender()->isShared()) << "the written buffer has a GPU buffer of its own";
    EXPECT_TRUE(second.eBuffers[positions].getRender()->isShared());
    EXPECT_NE(written.getRender()->backend.get().id, second.eBuffers[positions].getRender()->backend.get().id);
    Screen after;
    ASSERT_TRUE(after.read());
    EXPECT_GT(after.inkOnBackground(s.cell(2), 16).width(), firstWidth + 4) << "the written copy draws its edit";
    const InkBox secondAfter = after.inkOnBackground(s.cell(3), 16);
    EXPECT_EQ(secondAfter.x0, secondBefore.x0) << "the other copy draws the file's quad";
    EXPECT_EQ(secondAfter.x1, secondBefore.x1);
}

// A full buffer pool: one clear error naming the pool and its limit, the mesh that could not get
// a buffer counted as failed (the scene settles, the mesh is not drawn, not tried every frame),
// then loaded and drawn once buffers are released
TEST_F(SharedMeshBuffersTest, FullPoolIsReportedOnceAndItsMeshFailsUntilThereIsRoom){
    const std::string path = (tempDir() / "pool_full_quad.gltf").string();
    writeQuadGltf(path, 0.6f, 1.6f);
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene)
    GridScene s(1, 2.0f, 2.0f);
    ASSERT_TRUE(s.settle());

    const uint64_t exhaustionsBefore = SystemRender::getResourcePoolExhaustions(GpuResourceKind::BUFFER);
    const int limit = SystemRender::getResourceLimit(GpuResourceKind::BUFFER);
    EXPECT_EQ(limit, Engine::getGpuResourceLimits().buffers);

    // every free buffer slot taken
    std::vector<sg_buffer> fillers;
    for (;;){
        sg_buffer buffer = sg_alloc_buffer();
        if (buffer.id == SG_INVALID_ID)
            break;
        fillers.push_back(buffer);
    }
    EXPECT_EQ(SystemRender::getResourcesInUse(GpuResourceKind::BUFFER), limit);

    addCopies(s, path, 0, 1, models);
    ASSERT_TRUE(s.settle()) << "a mesh that cannot get its buffers does not keep the scene loading";
    MeshComponent& mesh = models[0]->getComponent<MeshComponent>();
    EXPECT_TRUE(mesh.loadFailed);
    EXPECT_FALSE(mesh.loaded);
    EXPECT_TRUE(s.scene().getSystem<RenderSystem>()->isAllLoaded());
    EXPECT_GT(SystemRender::getResourcePoolExhaustions(GpuResourceKind::BUFFER), exhaustionsBefore);

    // not tried every frame: no new full-pool event while nothing is released
    const uint64_t settled = SystemRender::getResourcePoolExhaustions(GpuResourceKind::BUFFER);
    GpuEnvironment::drawFrames(5);
    EXPECT_EQ(SystemRender::getResourcePoolExhaustions(GpuResourceKind::BUFFER), settled);

    // one error naming the pool and its limit (none more for the mesh), whoever ran out first
    const std::string message = "GPU buffer pool full: all " + std::to_string(limit) + " buffers are in use";
    const size_t errors = TestSystem::get().countLogs(D_LOG_ERROR, message);
    if (exhaustionsBefore == 0) {
        EXPECT_EQ(errors, 1u) << errorLines();
    }
    else
        EXPECT_LE(errors, 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "Cannot create GPU buffer"), 0u) << "the mesh's failure is not an error of its own";
    Screen full;
    ASSERT_TRUE(full.read());
    EXPECT_TRUE(full.inkOnBackground(s.cell(0), 16).empty()) << "a failed mesh is not drawn";

    for (sg_buffer buffer : fillers)
        sg_dealloc_buffer(buffer);
    ASSERT_TRUE(s.settle());
    EXPECT_FALSE(mesh.loadFailed);
    EXPECT_TRUE(mesh.loaded) << "loaded once the pool has room";
    Screen roomy;
    ASSERT_TRUE(roomy.read());
    EXPECT_FALSE(roomy.inkOnBackground(s.cell(0), 16).empty());
}

// Copies sharing their buffers and pipelines, some mirrored (a negative scale): a mirrored copy
// still draws with the reversed-winding variant of the shared pipeline (SokolPipelineCache's,
// one for all of them), so its single-sided quad keeps its front face and draws as the others;
// the variant goes with the last mirrored copy
TEST_F(SharedMeshBuffersTest, MirroredCopiesSharingBuffersDrawWithTheReversedWindingVariant){
    const std::string path = (tempDir() / "mirrored_quad.gltf").string();
    writeQuadGltf(path, 0.6f, 1.6f); // symmetric in x
    const int count = 8;
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene)
    GridScene s(count, 2.0f, 2.0f);
    ASSERT_TRUE(s.settle());
    const size_t variantsBefore = ObjectRender::getSharedVariantCount();
    const size_t poolBefore = MeshBufferPool::getBufferCount();
    addCopies(s, path, 0, count, models);
    for (int i = 1; i < count; i += 2)
        models[i]->setScale(Vector3(-1.0f, 1.0f, 1.0f));
    ASSERT_TRUE(s.settle());

    EXPECT_TRUE(models[0]->isFaceCulling()) << "a single-sided glTF material culls back faces";
    EXPECT_EQ(MeshBufferPool::getBufferCount(), poolBefore + 3) << "one set of buffers for every copy";
    const std::set<uint32_t> ids = meshBufferIds(models[0]->getComponent<MeshComponent>());
    for (auto& model : models)
        EXPECT_EQ(meshBufferIds(model->getComponent<MeshComponent>()), ids) << "mirrored or not, the same buffers";
    const size_t variants = ObjectRender::getSharedVariantCount();
    EXPECT_GT(variants, variantsBefore) << "the mirrored copies draw with a variant";
    EXPECT_LT(variants - variantsBefore, size_t(count / 2)) << "one a pass type for all of them, not one a mirrored copy";

    Screen screen;
    ASSERT_TRUE(screen.read());
    const InkBox plain = screen.inkOnBackground(s.cell(0), 16);
    ASSERT_GT(plain.count, 2u);
    for (int i = 0; i < count; i++){
        const InkBox box = screen.inkOnBackground(s.cell(i), 16);
        EXPECT_GT(box.count, plain.count * 9 / 10) << "copy " << i << (i % 2 ? " (mirrored)" : "") << " draws its front face";
        EXPECT_NEAR(box.width(), plain.width(), 2) << "copy " << i;
    }

    // unmirrored and reloaded: the variant goes with its last holder
    for (int i = 1; i < count; i += 2){
        models[i]->setScale(Vector3(1.0f, 1.0f, 1.0f));
        models[i]->getComponent<MeshComponent>().needReload = true;
    }
    ASSERT_TRUE(s.settle());
    EXPECT_EQ(ObjectRender::getSharedVariantCount(), variantsBefore);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}
