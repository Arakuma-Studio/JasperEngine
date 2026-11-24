// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vertex Colors component on its meshes (docs/specs/V1-vertex-colors.md R11-R16, R80-R82, R95,
// AC3, AC10, AC11), on the headless device:
//
// vertex_color_bound_boxes: three boxes whose Vertex Colors are their positions (two RGBA8 copies of
// the same values sharing one pooled stream, one RGBA16F), drawn by the built-in Lit (Tint).
//
// vertex_color_model_asset_colors: a glTF grid with COLOR_0 and no component (AC11: the asset's
// colours as before); an enabled component whose file holds no target for it draws it the same.
//
// vertex_color_face_hard_edges: a smooth sphere (one vertex per position) whose upper faces are set
// red through VertexColors:applyOps (faces: corner overrides, split vertices), in the Draw Mode
// Vertex Color: the band's edge follows the triangles, hard.
//
// A skinned, morphed glTF grid with detail levels draws the same pixels on split vertices as on the
// shared ones where no corner differs (AC3), at level 0 and level 1.

#include "Engine.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexColorsComponent.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/Mesh.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "object/VertexColors.h"
#include "render/ViewDrawMode.h"
#include "script/LuaBinding.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/VertexColorBinding.h"
#include "subsystem/VertexColorRenderer.h"
#include "subsystem/VertexColorScene.h"
#include "texture/Material.h"
#include "util/Angle.h"
#include "vertexcolor/VertexColorRuntimeLayer.h"
#include "vertexcolor/VertexColorSplit.h"

#include "GpuEnvironment.h"
#include "HeadlessContext.h"
#include "ImageCompare.h"
#include "LogCapture.h"
#include "Readback.h"
#include "TempDir.h"
#include "../vertexcolor/VertexColorSceneFixture.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace jasperengine;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_render_gpu_test {
    using namespace jasperengine::test;
    using namespace jasperengine::vertexcolor;

    bool vcRenderIsGoldenBackend(){
        return std::strcmp(HeadlessContext::getBackendName(), "glcore") == 0;
    }

    CompareOptions vcRenderExactOnGolden(){
        CompareOptions options;
        if (vcRenderIsGoldenBackend()){
            options.pixelThreshold = 0;
            options.maxBadFraction = 0.0;
            options.maxMeanDiff = 0.0;
        }else{
            options.pixelThreshold = 16;
            options.maxBadFraction = 0.03;
            options.maxMeanDiff = 3.0;
        }
        return options;
    }

    TargetKey submeshKey(int submesh){
        TargetKey key;
        key.submesh = submesh;
        return key;
    }

    TargetKey nodeKey(int node, int primitive){
        TargetKey key;
        key.node = node;
        key.primitive = primitive;
        return key;
    }

    // a UV sphere of radius 1 with one vertex per position but the seam's (the shared vertices a
    // face colour has to split)
    void buildSmoothSphere(Scene& scene, Mesh& sphere){
        constexpr int RINGS = 12;
        constexpr int SEGMENTS = 16;
        MeshComponent& mesh = sphere.getComponent<MeshComponent>();
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        for (int r = 0; r <= RINGS; r++){
            const float v = (float)r / RINGS;
            const float theta = v * 3.14159265f;
            for (int s = 0; s <= SEGMENTS; s++){
                const float u = (float)s / SEGMENTS;
                const float phi = u * 2.0f * 3.14159265f;
                const Vector3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
                mesh.buffer.addVector3(position, n);
                mesh.buffer.addVector2(texcoord, Vector2(u, v));
                mesh.buffer.addVector3(normal, n);
            }
        }
        std::vector<uint16_t> indices;
        const int row = SEGMENTS + 1;
        for (int r = 0; r < RINGS; r++){
            for (int s = 0; s < SEGMENTS; s++){
                const uint16_t a = (uint16_t)(r * row + s);
                const uint16_t b = (uint16_t)((r + 1) * row + s);
                const uint16_t c = (uint16_t)((r + 1) * row + s + 1);
                const uint16_t d = (uint16_t)(r * row + s + 1);
                indices.insert(indices.end(), {a, d, b, d, c, b});
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), (char*)&indices[0], sizeof(uint16_t));
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    }

    class VertexColorRenderGpu : public ::testing::Test {
    protected:
        Scene scene;
        Camera camera{&scene};
        Light sun{&scene};

        void SetUp() override{
            GpuEnvironment::resetView();
            scene.setBackgroundColor(0.12f, 0.14f, 0.18f, 1.0f);
            camera.setType(CameraType::CAMERA_PERSPECTIVE);
            camera.setPosition(0.0f, 1.2f, 4.5f);
            camera.setTarget(0.0f, 0.0f, 0.0f);
            camera.activate();
            sun.setType(LightType::DIRECTIONAL);
            sun.setDirection(-0.5f, -1.0f, -0.6f);
            sun.setIntensity(2.0f);
            sun.setShadows(false);
            Engine::setScene(&scene);
        }

        void TearDown() override{
            scene.getSystem<RenderSystem>()->setDrawMode(ViewDrawMode::SHADED);
            Engine::removeScene(&scene);
        }

        RenderSystem& render(){
            return *scene.getSystem<RenderSystem>();
        }

        VertexColorRenderer& colors(){
            return render().getVertexColors();
        }

        // loaded, no split building, and `bound` holds (the binding's reload is done)
        bool settle(std::function<bool()> bound = nullptr){
            Scene* current = &scene;
            return GpuEnvironment::runFrames([current, bound](){
                RenderSystem* system = current->getSystem<RenderSystem>().get();
                const VertexColorRenderer* vc = system->findVertexColors();
                return system->isAllLoaded() && !system->isVertexColorViewPending() &&
                       (!vc || vc->getPendingSplits() == 0) && (!bound || bound());
            });
        }

        std::vector<uint8_t> capture(){
            GpuEnvironment::drawFrames(2);
            Framebuffer& target = GpuEnvironment::target();
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), (int)target.getWidth(), (int)target.getHeight(), rgba));
            return rgba;
        }

        void expectGolden(const std::string& name){
            const CompareResult result = compareTargetWithGolden(name, vcRenderExactOnGolden());
            std::printf("[gpu] %s on %s: largest difference %d, %zu pixels over the threshold, mean %.4f\n",
                        name.c_str(), HeadlessContext::getBackendName(), result.maxDiff, result.badPixels, result.meanDiff);
            EXPECT_TRUE(result.passed) << result.message;
        }

        // the component with one target whose values are a function of the vertex position
        VertexColorsComponent& paint(Entity owner, const TargetKey& key, const std::function<Vector4(const Vector3&)>& color,
                                     Format format = Format::RGBA8){
            if (!scene.findComponent<VertexColorsComponent>(owner)){
                VertexColorsComponent component;
                component.format = format;
                scene.addComponent<VertexColorsComponent>(owner, component);
            }
            VertexColorsComponent& component = scene.getComponent<VertexColorsComponent>(owner);
            std::shared_ptr<const MeshView> view = sceneMeshView(&scene, owner, key);
            EXPECT_TRUE(view) << key.describe();
            if (!view)
                return component;
            auto data = std::make_shared<vertexcolor::Data>();
            data->format = format;
            TargetData& target = data->add(key);
            target.fingerprint = view->fingerprint();
            target.base.resize(view->vertexCount());
            for (uint32_t v = 0; v < view->vertexCount(); v++)
                target.base[v] = quantize(color(view->positions[v]), format);
            component.loaded = data;
            markVertexColorsChanged(&scene, owner);
            return component;
        }

        const SubmeshVertexColors* bindingOf(Entity meshEntity, unsigned int submesh = 0){
            return scene.getComponent<MeshComponent>(meshEntity).submeshes[submesh].vertexColors.get();
        }
    };

    Vector4 positionColor(const Vector3& p){
        return Vector4(0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y, 0.5f + 0.5f * p.z, 1.0f);
    }

// R11, R13: no override binds one colour stream over the shared geometry, pooled by content
TEST_F(VertexColorRenderGpu, BindsOneColorStreamPooledByContent){
    JASPER_REQUIRE_GPU();
    Primitive left(&scene), middle(&scene), right(&scene);
    for (Primitive* box : {&left, &middle, &right})
        box->createBox(0.9f, 0.9f, 0.9f);
    left.setPosition(-1.4f, 0.0f, 0.0f);
    right.setPosition(1.4f, 0.0f, 0.0f);
    paint(left.getEntity(), submeshKey(0), positionColor);
    paint(middle.getEntity(), submeshKey(0), positionColor);
    paint(right.getEntity(), submeshKey(0), positionColor, Format::RGBA16F);
    ASSERT_TRUE(settle([&](){ return bindingOf(left.getEntity()) && bindingOf(middle.getEntity()) && bindingOf(right.getEntity()); }))
        << "the scene never finished loading";

    for (Primitive* box : {&left, &middle, &right}){
        const Submesh& submesh = scene.getComponent<MeshComponent>(box->getEntity()).submeshes[0];
        ASSERT_TRUE(submesh.vertexColors);
        EXPECT_FALSE(submesh.vertexColors->split);
        EXPECT_TRUE(submesh.hasVertexColor4) << "the Vc4 variant (R11)";
        EXPECT_EQ(submesh.vertexColors->attributes.size(), 1u) << "only COLOR: the shared positions, normals, UVs and indices stay";
        EXPECT_EQ(boundSplitVertices(&scene, box->getEntity(), submeshKey(0)), 0u);
    }
    const Attribute& rgba8 = bindingOf(left.getEntity())->attributes.at(AttributeType::COLOR);
    EXPECT_EQ(rgba8.getDataType(), AttributeDataType::UNSIGNED_BYTE);
    EXPECT_TRUE(rgba8.getNormalized());
    EXPECT_EQ(bindingOf(right.getEntity())->attributes.at(AttributeType::COLOR).getDataType(), AttributeDataType::HALF);
    // R13: the same bytes, one GPU buffer, so the copies still instance
    const uint32_t leftId = bindingOf(left.getEntity())->color->getRender()->backend.get().id;
    EXPECT_NE(leftId, 0u);
    EXPECT_EQ(bindingOf(middle.getEntity())->color->getRender()->backend.get().id, leftId);
    EXPECT_NE(bindingOf(right.getEntity())->color->getRender()->backend.get().id, leftId);
    EXPECT_EQ(bindingOf(left.getEntity())->color->getUsage(), BufferUsage::IMMUTABLE);
    expectGolden("vertex_color_bound_boxes");
    EXPECT_NO_ENGINE_ERRORS();
}

// R15: off draws as without it, and so does a removed component; nothing stays bound
TEST_F(VertexColorRenderGpu, DisabledOrRemovedDrawsAsWithout){
    JASPER_REQUIRE_GPU();
    Primitive box(&scene);
    box.createBox(1.4f, 1.4f, 1.4f);
    ASSERT_TRUE(settle());
    const std::vector<uint8_t> without = capture();

    paint(box.getEntity(), submeshKey(0), positionColor);
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) != nullptr; }));
    const std::vector<uint8_t> painted = capture();
    EXPECT_NE(painted, without) << "the colours draw";

    scene.getComponent<VertexColorsComponent>(box.getEntity()).enabled = false;
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) == nullptr; }));
    EXPECT_EQ(capture(), without) << "disabled: drawn as without the component";
    EXPECT_FALSE(colors().isBound(box.getEntity(), submeshKey(0)));

    scene.getComponent<VertexColorsComponent>(box.getEntity()).enabled = true;
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) != nullptr; }));
    EXPECT_EQ(capture(), painted);

    scene.removeComponent<VertexColorsComponent>(box.getEntity());
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) == nullptr; }));
    EXPECT_EQ(capture(), without) << "removed: drawn as without the component";
    EXPECT_NO_ENGINE_ERRORS();
}

// R95: a scene that never had the component makes nothing of it
TEST_F(VertexColorRenderGpu, NoComponentNoWork){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    vctest::GridPrimitive grid;
    grid.cells = 4;
    grid.size = 2.0f;
    grid.colors = true;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(vctest::writeGridModel(dir, "plain", {vctest::GltfNode{"Grid", {grid}}})));
    Primitive box(&scene);
    box.createBox(0.5f, 0.5f, 0.5f);
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(4);
    EXPECT_EQ(render().findVertexColors(), nullptr) << "no Vertex Colors renderer made";
    EXPECT_EQ(bindingOf(box.getEntity()), nullptr);
    EXPECT_EQ(bindingOf(model.getEntity()), nullptr);
}

// AC11, R2: a model's own COLOR_0 draws as before; a component whose file has no target for it
// leaves it so (no binding)
TEST_F(VertexColorRenderGpu, AssetColorsWithoutATarget){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.0f, 3.2f);
    TempDir dir;
    vctest::GridPrimitive grid;
    grid.cells = 6;
    grid.size = 2.4f;
    grid.colors = true;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(vctest::writeGridModel(dir, "asset", {vctest::GltfNode{"Grid", {grid}}})));
    ASSERT_TRUE(settle());
    expectGolden("vertex_color_model_asset_colors");
    const std::vector<uint8_t> asset = capture();

    VertexColorsComponent component;
    component.loaded = std::make_shared<vertexcolor::Data>();
    scene.addComponent<VertexColorsComponent>(model.getEntity(), component);
    ASSERT_TRUE(settle());
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(bindingOf(model.getEntity()), nullptr) << "no target, nothing bound";
    EXPECT_EQ(capture(), asset);
    EXPECT_NO_ENGINE_ERRORS();
}

// R12, R24: face colours split the shared vertices of a smooth mesh into hard edges
TEST_F(VertexColorRenderGpu, FaceColorsDrawHardEdges){
    JASPER_REQUIRE_GPU();
    camera.setPosition(0.0f, 0.8f, 3.4f);
    Mesh sphere(&scene);
    buildSmoothSphere(scene, sphere);
    sphere.setMaterial(Material());
    scene.addComponent<VertexColorsComponent>(sphere.getEntity(), VertexColorsComponent());
    VertexColors colorsApi(&scene, sphere.getEntity());
    // the faces whose centre is above y = 0.35 (world, the sphere at the origin)
    std::shared_ptr<const MeshView> view = sceneMeshView(&scene, sphere.getEntity(), submeshKey(0));
    ASSERT_TRUE(view);
    nlohmann::json ids = nlohmann::json::array();
    for (uint32_t t = 0; t < view->triangleCount(); t++){
        const Vector3 centre = (view->positions[view->indices[t * 3]] + view->positions[view->indices[t * 3 + 1]] +
                                view->positions[view->indices[t * 3 + 2]]) * (1.0f / 3.0f);
        if (centre.y > 0.35f)
            ids.push_back(t);
    }
    const nlohmann::json ops = nlohmann::json::parse(R"([{"op": "set", "target": {"kind": "all"}, "value": [0.15, 0.3, 0.9, 1]}])");
    nlohmann::json faces = nlohmann::json::parse(R"({"op": "set", "value": [0.95, 0.2, 0.1, 1]})");
    faces["target"] = {{"kind", "faces"}, {"ids", ids}};
    nlohmann::json list = ops;
    list.push_back(faces);
    nlohmann::json result;
    std::string error;
    ASSERT_TRUE(colorsApi.applyOps(list, nullptr, result, error)) << error;
    ASSERT_EQ(result.size(), 1u);

    render().setDrawMode(ViewDrawMode::VERTEX_COLOR, ViewVertexColorChannel::RGB);
    ASSERT_TRUE(settle([&](){ return bindingOf(sphere.getEntity()) != nullptr; })) << "the split never bound";
    const SubmeshVertexColors* binding = bindingOf(sphere.getEntity());
    ASSERT_TRUE(binding->split);
    // the band's ring of shared vertices takes a copy each (its border vertices, one per position
    // and the seam's twin)
    EXPECT_GT(binding->splitVertices, 0u);
    EXPECT_EQ(boundSplitVertices(&scene, sphere.getEntity(), submeshKey(0)), binding->splitVertices);
    EXPECT_EQ(binding->attributes.count(AttributeType::INDEX), 1u);
    EXPECT_EQ(binding->attributes.count(AttributeType::POSITION), 1u);
    EXPECT_EQ(binding->attributes.count(AttributeType::NORMAL), 1u);
    expectGolden("vertex_color_face_hard_edges");
    EXPECT_NO_ENGINE_ERRORS();
}

// AC3, R12: split vertices duplicate every input (skin, morph targets) and remap every detail
// level: where no corner differs the drawn pixels are the unsplit ones
TEST_F(VertexColorRenderGpu, SplitDrawsTheUnsplitResultWhereNoOverride){
    JASPER_REQUIRE_GPU();
    TempDir dir;
    vctest::GridPrimitive grid;
    grid.cells = 24;
    grid.size = 2.0f;
    grid.y = 0.5f;
    grid.skin = true;
    grid.morph = true;
    Model model(&scene);
    ASSERT_TRUE(model.loadModel(vctest::writeGridModel(dir, "skinned", {vctest::GltfNode{"Strip", {grid}}})));
    camera.setPosition(-0.45f, 0.3f, 1.6f);
    camera.setTarget(-0.45f, 0.3f, 0.0f);
    // shadows on: the depth passes draw it too (on the shared geometry)
    sun.setShadows(true);
    ASSERT_TRUE(settle());

    // the mesh that draws the node, posed: the tip joint bent, the morph target half on
    const ModelComponent& component = model.getComponent<ModelComponent>();
    const Entity strip = component.meshNodesMapping.empty() ? model.getEntity() : component.meshNodesMapping.begin()->second;
    MeshComponent& mesh = scene.getComponent<MeshComponent>(strip);
    ASSERT_TRUE(mesh.submeshes[0].hasSkinning);
    ASSERT_TRUE(mesh.submeshes[0].hasMorphTarget);
    ASSERT_TRUE(mesh.submeshes[0].lodData && mesh.submeshes[0].lodData->numLevels > 1) << "the grid gets detail levels";
    for (Entity entity : scene.getEntityList()){
        if (scene.getEntityName(entity) == "Tip"){
            Transform& tip = scene.getComponent<Transform>(entity);
            tip.rotation = Quaternion(Angle::degToDefault(20.0f), Vector3(0, 0, 1));
            tip.needUpdate = true;
        }
    }
    mesh.morphWeights[0] = 0.5f;

    const TargetKey key = nodeKey(0, 0);
    VertexColorsComponent& painted = paint(model.getEntity(), key, [](const Vector3& p){
        return Vector4(0.5f + 0.4f * p.x, 0.3f + 0.6f * p.y, 0.6f, 1.0f); });
    ASSERT_TRUE(settle([&](){ return bindingOf(strip) != nullptr; }));
    ASSERT_FALSE(bindingOf(strip)->split);
    mesh.lodForced = 0;
    const std::vector<uint8_t> unsplit0 = capture();
    mesh.lodForced = 1;
    const std::vector<uint8_t> unsplit1 = capture();

    // one corner of the far corner's last triangle (off screen) differs: the split
    std::shared_ptr<const MeshView> view = sceneMeshView(&scene, model.getEntity(), key);
    ASSERT_TRUE(view);
    TargetData& target = *painted.loaded->find(key);
    const uint32_t corner = (view->triangleCount() - 1) * 3 + 1;
    target.setCorner(corner, Vector4(1.0f, 0.0f, 1.0f, 1.0f), view->indices);
    ASSERT_TRUE(target.hasOverrides());
    markVertexColorsChanged(&scene, model.getEntity());
    ASSERT_TRUE(settle([&](){ return bindingOf(strip) && bindingOf(strip)->split; })) << "the split never bound";

    const SubmeshVertexColors* binding = bindingOf(strip);
    EXPECT_EQ(binding->splitVertices, 1u) << "one vertex copy for one differing corner";
    // the colour pass draws the split's indices, the depth passes the shared ones (R12)
    const Submesh& drawn = mesh.submeshes[0];
    EXPECT_EQ(drawn.render.getIndexBufferId(), binding->index->getRender()->backend.get().id);
    EXPECT_NE(drawn.depthRender.getIndexBufferId(), binding->index->getRender()->backend.get().id);
    EXPECT_NE(drawn.depthRender.getIndexBufferId(), 0u);
    EXPECT_EQ(boundSplitVertices(&scene, model.getEntity(), key), 1u);
    for (AttributeType type : {AttributeType::POSITION, AttributeType::NORMAL, AttributeType::BONEIDS, AttributeType::BONEWEIGHTS,
                               AttributeType::MORPHTARGET0, AttributeType::COLOR, AttributeType::INDEX})
        EXPECT_EQ(binding->attributes.count(type), 1u) << "a split stream for input " << (int)type;
    // every level's triangles over the copies: each split index shows the source vertex the pool's does
    ASSERT_TRUE(mesh.submeshes[0].lodData) << "the split mesh keeps its detail levels";
    const MeshLodData& lod = *mesh.submeshes[0].lodData;
    ASSERT_EQ(binding->levels.size(), (size_t)lod.numLevels);
    std::vector<uint32_t> splitIndices(binding->index->getCount());
    std::memcpy(splitIndices.data(), binding->index->getData(), splitIndices.size() * 4);
    // the GPU vertex each split vertex copies: its position (the first vertexCount are the originals)
    const uint32_t originals = view->vertexCount();
    std::vector<Vector3> splitPositions(originals + binding->splitVertices);
    const ExternalBuffer* positions = nullptr;
    for (const ExternalBuffer& buffer : binding->buffers){
        if (buffer.getName() == binding->attributes.at(AttributeType::POSITION).getBufferName())
            positions = &buffer;
    }
    ASSERT_TRUE(positions);
    std::memcpy(splitPositions.data(), positions->getData(), splitPositions.size() * sizeof(Vector3));
    for (unsigned int l = 1; l < lod.numLevels; l++){
        ASSERT_EQ(binding->levels[l].second, lod.levels[l].indexCount) << "level " << l;
        for (uint32_t i = 0; i < lod.levels[l].indexCount; i++){
            uint32_t pool = 0;
            if (lod.indexType == AttributeDataType::UNSIGNED_INT){
                std::memcpy(&pool, &lod.indices[((size_t)lod.levels[l].indexBase + i) * 4], 4);
            }else{
                uint16_t v;
                std::memcpy(&v, &lod.indices[((size_t)lod.levels[l].indexBase + i) * 2], 2);
                pool = v;
            }
            const uint32_t split = splitIndices[binding->levels[l].first + i];
            ASSERT_LT(split, splitPositions.size());
            EXPECT_EQ(splitPositions[split], view->positions[pool]) << "level " << l << " index " << i;
        }
    }

    mesh.lodForced = 0;
    EXPECT_EQ(capture(), unsplit0) << "level 0 on split vertices draws the unsplit pixels";
    mesh.lodForced = 1;
    EXPECT_EQ(capture(), unsplit1) << "level 1 on split vertices draws the unsplit pixels";
    EXPECT_NO_ENGINE_ERRORS();
}

// R14: the first change after binding makes the stream DYNAMIC (one rebind); later ones write it in
// place, once a frame; a stream that stops changing goes back to an immutable pooled one
TEST_F(VertexColorRenderGpu, EditsWriteTheStreamInPlace){
    JASPER_REQUIRE_GPU();
    Primitive box(&scene);
    box.createBox(1.2f, 1.2f, 1.2f);
    ASSERT_TRUE(settle());
    paint(box.getEntity(), submeshKey(0), positionColor);
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) != nullptr; }));
    const uint64_t bindings = colors().getStats().bindings;
    const uint64_t uploads = colors().getStats().colorUploads;

    VertexColors api(&scene, box.getEntity());
    std::string error;
    const std::vector<Vector4> red(4, Vector4(1.0f, 0.0f, 0.0f, 1.0f));
    ASSERT_TRUE(api.setValues(submeshKey(0), 0, red, error)) << error;
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) && bindingOf(box.getEntity())->color->getUsage() == BufferUsage::DYNAMIC; }));
    EXPECT_EQ(colors().getStats().bindings, bindings + 1) << "one rebind: the stream of its own";

    // three edits in one frame: one upload
    for (int i = 0; i < 3; i++){
        const std::vector<Vector4> value(4, Vector4(0.0f, 0.2f * (float)i, 1.0f, 1.0f));
        ASSERT_TRUE(api.setValues(submeshKey(0), 4, value, error)) << error;
    }
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(colors().getStats().colorUploads, uploads + 1);
    EXPECT_EQ(colors().getStats().bindings, bindings + 1) << "written in place";
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(colors().getStats().colorUploads, uploads + 1) << "no change, no upload";
    const std::vector<uint8_t> dynamicImage = capture();

    GpuEnvironment::drawFrames(100);
    ASSERT_TRUE(settle());
    EXPECT_EQ(bindingOf(box.getEntity())->color->getUsage(), BufferUsage::IMMUTABLE) << "settled: pooled again";
    EXPECT_EQ(colors().getStats().bindings, bindings + 2);
    EXPECT_EQ(capture(), dynamicImage) << "the stream written in place draws what the immutable one of its bytes draws";
    EXPECT_NO_ENGINE_ERRORS();
}

// AC10, R80-R82: Lua paint with hold 1 s and fade 1 s is back to the authored values at 2 s of game
// time, and nothing is uploaded after
TEST_F(VertexColorRenderGpu, RuntimePaintFadesBackToAuthored){
    JASPER_REQUIRE_GPU();
    Primitive box(&scene);
    box.createBox(1.2f, 1.2f, 1.2f);
    ASSERT_TRUE(settle());
    paint(box.getEntity(), submeshKey(0), [](const Vector3&){ return Vector4(0.2f, 0.4f, 0.6f, 1.0f); });
    ASSERT_TRUE(settle([&](){ return bindingOf(box.getEntity()) != nullptr; }));
    const std::vector<uint8_t> authored = bindingOf(box.getEntity())->bytes.front();

    const double fixed = Engine::getFixedDeltatime();
    Engine::setFixedDeltatime(1.0 / 30.0);
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    ASSERT_TRUE(luabridge::push(L, VertexColors(&scene, box.getEntity())));
    lua_setglobal(L, "vc");
    const double start = Engine::getGameTime();
    ASSERT_EQ(luaL_dostring(L, R"(
        touched = vc:paint(Vector3(0.6, 0.6, 0.6), { radius = 0.5, value = {1, 0, 0, 1}, hold = 1, fade = 1 })
        local v = vc:sampleAt(Vector3(0.6, 0.6, 0.6))
        heldRed = v.x
    )"), 0) << lua_tostring(L, -1);
    lua_getglobal(L, "touched");
    EXPECT_GT(lua_tointeger(L, -1), 0) << "the corner's vertices";
    lua_pop(L, 1);
    lua_getglobal(L, "heldRed");
    EXPECT_NEAR(lua_tonumber(L, -1), 1.0, 1e-4) << "full value while it holds";
    lua_pop(L, 1);

    GpuEnvironment::drawFrames(3);
    EXPECT_NE(bindingOf(box.getEntity())->bytes.front(), authored) << "the paint is drawn";
    for (int frame = 0; frame < 600 && Engine::getGameTime() - start < 2.2; frame++)
        GpuEnvironment::drawFrames(1);
    ASSERT_GE(Engine::getGameTime() - start, 2.2) << "game time does not advance";
    GpuEnvironment::drawFrames(3);
    const VertexColorsComponent& component = scene.getComponent<VertexColorsComponent>(box.getEntity());
    ASSERT_EQ(component.runtimeLayers.count(submeshKey(0)), 1u);
    EXPECT_FALSE(component.runtimeLayers.at(submeshKey(0))->live()) << "no live paint after hold + fade";
    EXPECT_EQ(bindingOf(box.getEntity())->bytes.front(), authored) << "back to the authored values (R80: never written to them)";
    ASSERT_EQ(luaL_dostring(L, "back = vc:sampleAt(Vector3(0.6, 0.6, 0.6)).x"), 0) << lua_tostring(L, -1);
    lua_getglobal(L, "back");
    EXPECT_NEAR(lua_tonumber(L, -1), 0.2, 1.0 / 255.0);
    lua_pop(L, 1);
    EXPECT_NEAR(component.loaded->find(submeshKey(0))->base[0].x, 0.2f, 1.0f / 255.0f) << "the authored values are untouched";

    const uint64_t uploads = colors().getStats().colorUploads;
    GpuEnvironment::drawFrames(30);
    EXPECT_EQ(colors().getStats().colorUploads, uploads) << "R82: no upload, no per-frame work once the paint is gone";

    lua_pushnil(L);
    lua_setglobal(L, "vc");
    Engine::setFixedDeltatime(fixed);
    EXPECT_NO_ENGINE_ERRORS();
}

} // namespace vertex_color_render_gpu_test
