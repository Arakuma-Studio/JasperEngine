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

#include "TestGoldenCommon.h"

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "pool/MeshBufferPool.h"
#include "render/SystemRender.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "harness/TestSystem.h"
#include "sokol_gfx.h"

#include <algorithm>
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

    // Loads copies [first, last) of `path`, placed in their cells. `models` is declared before the
    // scene, so the scene goes first, all at once: destroying 150,000 entities one by one (the
    // heroes' node hierarchies) erases from every component array in turn and takes hours
    void addCopies(GridScene& s, const std::string& path, int first, int last, std::vector<std::unique_ptr<Model>>& models){
        for (int i = first; i < last; i++){
            models.emplace_back(new Model(&s.scene()));
            Model& model = *models.back();
            ASSERT_TRUE(model.loadModel(path)) << path;
            model.setPosition(s.position(i));
        }
    }

    // A grid scene of thousands of copies, left alive (out of the engine: not drawn nor updated)
    // when the test ends: destroying its entities takes minutes to hours (each one is erased
    // from every component array in turn, by the scene's teardown too: an engine limit of its own)
    class AbandonedCrowd {
    public:
        AbandonedCrowd(int count, float cellWidth, float cellHeight): crowd(new Crowd(count, cellWidth, cellHeight)){}
        ~AbandonedCrowd(){ Engine::removeScene(&crowd->scene.scene()); }
        AbandonedCrowd(const AbandonedCrowd&) = delete;
        AbandonedCrowd& operator=(const AbandonedCrowd&) = delete;

        std::vector<std::unique_ptr<Model>>& models(){ return crowd->models; }
        GridScene& scene(){ return crowd->scene; }

    private:
        struct Crowd {
            std::vector<std::unique_ptr<Model>> models;
            GridScene scene;
            Crowd(int count, float cellWidth, float cellHeight): scene(count, cellWidth, cellHeight){}
        };
        Crowd* crowd;
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
    AbandonedCrowd crowd(count, 2.0f, 2.4f);
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

    Screen screen;
    ASSERT_TRUE(screen.read());
    const std::vector<int> blank = s.blankCells(screen, 0, count);
    EXPECT_TRUE(blank.empty()) << blank.size() << " of " << count << " heroes not drawn: " << firstOf(blank);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}

// The frame's uniform blocks (each rounded up to 256 bytes, as sokol's per-frame uniform buffer
// takes them on Vulkan, Metal and WebGPU) of skinned heroes in shadow-casting light: what one
// hero costs a frame, and that Engine::GpuResourceLimits::uniformBytes' desktop default (64 MB)
// holds 4,000 of them. A frame of 3,300 shadowed heroes overflowed sokol's default 4 MB on
// Vulkan and dropped draws.
TEST_F(SharedMeshBuffersTest, UniformBufferHoldsFourThousandShadowedSkinnedHeroes){
    const int count = 500;
    AbandonedCrowd crowd(count, 2.0f, 2.4f);
    GridScene& s = crowd.scene();
    s.light->setShadows(true);
    const uint64_t skippedBefore = SystemRender::getUniformSkippedDraws();
    ASSERT_TRUE(s.settle());
    const uint64_t empty = SystemRender::getUniformBytesLastFrame();
    addCopies(s, heroPath(), 0, count, crowd.models());
    ASSERT_TRUE(s.settle());
    GpuEnvironment::drawFrames(2);
    const uint64_t frame = SystemRender::getUniformBytesLastFrame();
    ASSERT_GT(frame, empty);
    const double perHero = double(frame - empty) / count;
    std::printf("[uniforms] %d shadowed skinned heroes: %llu bytes a frame (%.0f a hero, %llu without them), limit %llu\n", count,
                (unsigned long long)frame, perHero, (unsigned long long)empty, (unsigned long long)SystemRender::getUniformBytesLimit());
    // a hero draws 3 submeshes in the colour pass and the shadow cascades
    EXPECT_GT(perHero, 3 * 256.0);
    EXPECT_LE(empty + perHero * 4000.0, double(SystemRender::getUniformBytesLimit())) << "4,000 shadowed heroes fit";
    EXPECT_EQ(SystemRender::getUniformSkippedDraws(), skippedBefore);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << errorLines();
}

// 3,000 copies of a static glTF and 3,000 of an OBJ: one vertex (or 2 for the glTF's positions
// and normals) and one index buffer each, every copy drawn
TEST_F(SharedMeshBuffersTest, ThousandsOfStaticGltfAndObjCopiesAllDrawFromOneSetOfBuffers){
    const int perFile = 3000;
    const std::string gltf = (tempDir() / "static_quad.gltf").string();
    const std::string obj = (tempDir() / "static_quad.obj").string();
    writeQuadGltf(gltf, 0.6f, 1.6f);
    writeQuadObj(obj, 0.6f, 1.6f);

    AbandonedCrowd crowd(perFile * 2, 1.6f, 2.0f);
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
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene: see addCopies)
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
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene: see addCopies)
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
    std::vector<std::unique_ptr<Model>> models; // (outlive the scene: see addCopies)
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
    if (exhaustionsBefore == 0)
        EXPECT_EQ(errors, 1u) << errorLines();
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
