// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A game's terrain writes (T1 R68, R75, R78, DC9): the op tables Lua and edit_terrain share, the
// maps a runtime write detaches, and the paced image upload.

#include "TerrainTestSupport.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"
#include "object/TerrainOpsJson.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace terraintest;
using nlohmann::json;

namespace {

    constexpr int RUNTIME_MAP_SIZE = 16;

    void runtimeGiveHeight(Texture& texture, const std::string& id, uint8_t fill) {
        const size_t bytes = static_cast<size_t>(RUNTIME_MAP_SIZE) * RUNTIME_MAP_SIZE * 2;
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
        std::memset(raw, fill, bytes);
        TextureData data(RUNTIME_MAP_SIZE, RUNTIME_MAP_SIZE, static_cast<unsigned int>(bytes), ColorFormat::RED16, 1, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    // shareWith: the new terrain uses that one's image, as two terrains on one file do
    Entity runtimeTerrain(Scene& scene, const std::string& heightId, const Texture* shareWith = nullptr) {
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        TerrainComponent component;
        component.terrainSize = static_cast<float>(RUNTIME_MAP_SIZE);
        component.maxHeight = 8.0f;
        scene.addComponent<TerrainComponent>(entity, component);
        if (shareWith) scene.getComponent<TerrainComponent>(entity).heightMap = *shareWith;
        else runtimeGiveHeight(scene.getComponent<TerrainComponent>(entity).heightMap, heightId, 0x40);
        return entity;
    }

    Op runtimeCircle(OpType type, float radius) {
        Op op = circleOp(type, 0.0f, 0.0f, radius);
        op.delta = 1.0f;
        return op;
    }

    terrain::OpSpace runtimeSpace(const Vector3& position, const Vector3& scale) {
        Matrix4 model = Matrix4::translateMatrix(position) * Matrix4::scaleMatrix(scale);
        return terrain::OpSpace::of(model, position, scale, false, true);
    }

}

// R68, R75: world numbers become terrain-local ones, and a bad field is named by its path
TEST(TerrainOpsJson, ReadsWorldSpaceAndNamesTheBadField) {
    const terrain::OpSpace space = runtimeSpace(Vector3(10.0f, 5.0f, -4.0f), Vector3(2.0f, 4.0f, 2.0f));
    terrain::OpJsonParser parser(space, {});
    std::vector<Op> ops;
    const json list = json::parse(R"([
        {"op": "raise", "delta": 8, "shape": {"type": "circle", "center": [14, 0], "radius": 6}},
        {"op": "set_height", "height": 13, "filter": {"slope": [0, 30]}}
    ])");
    ASSERT_TRUE(parser.parseList(list, ops)) << parser.error;
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].type, OpType::Raise);
    ASSERT_TRUE(ops[0].delta.has_value());
    EXPECT_FLOAT_EQ(*ops[0].delta, 2.0f) << "a world delta over the Y scale";
    EXPECT_FLOAT_EQ(ops[0].shape.center.x, 2.0f);
    EXPECT_FLOAT_EQ(ops[0].shape.center.y, 2.0f);
    EXPECT_FLOAT_EQ(ops[0].shape.radius, 3.0f);
    ASSERT_TRUE(ops[1].height.has_value());
    EXPECT_FLOAT_EQ(*ops[1].height, 2.0f) << "a world height less the origin, over the Y scale";
    ASSERT_TRUE(ops[1].filter.slope.has_value());

    std::vector<Op> bad;
    EXPECT_FALSE(parser.parseList(json::parse(R"([{"op": "raise", "delta": 1}, {"op": "lower", "shape": {"type": "circle", "radius": 2, "size": 1, "colour": 1}}])"), bad));
    EXPECT_EQ(parser.error.rfind("ops[1].shape.colour:", 0), 0u) << parser.error;
    EXPECT_FALSE(parser.parseList(json::parse(R"([{"op": "rise"}])"), bad));
    EXPECT_NE(parser.error.find("raise"), std::string::npos) << "the valid names are listed: " << parser.error;
}

// R75: what a caller does not take is refused before anything runs, saying what to use
TEST(TerrainOpsJson, RefusesWhatTheCallerDoesNotTake) {
    const terrain::OpSpace space = runtimeSpace(Vector3(0.0f), Vector3(1.0f));
    terrain::OpJsonHooks hooks;
    hooks.refused = {{OpType::RemapLayers, "is for the editor only."}};
    terrain::OpJsonParser parser(space, hooks);
    std::vector<Op> ops;
    EXPECT_FALSE(parser.parseList(json::parse(R"([{"op": "remap_layers", "order": [1, 0]}])"), ops));
    EXPECT_EQ(parser.error, "ops[0].op: remap_layers is for the editor only.");
    EXPECT_FALSE(parser.parseList(json::parse(R"([{"op": "import_heightmap", "path": "a.r16"}])"), ops));
    EXPECT_NE(parser.error.find("editor only"), std::string::npos) << parser.error;
    EXPECT_FALSE(parser.parseList(json::parse(R"([{"op": "raise", "delta": 1, "shape": {"type": "mask", "path": "m.png", "size": [4, 4]}}])"), ops));
    EXPECT_EQ(parser.error.rfind("ops[0].shape.path:", 0), 0u) << parser.error;
}

// The result has the edit_terrain shape, in the caller's space
TEST(TerrainOpsJson, ResultInTheCallersSpace) {
    RawMapStore store(16.0f, 8.0f);
    addFlatHeight(store, 17, 0.25f);
    const terrain::OpSpace space = runtimeSpace(Vector3(0.0f, 1.0f, 0.0f), Vector3(1.0f, 2.0f, 1.0f));
    terrain::OpJsonParser parser(space, {});
    std::vector<Op> ops;
    ASSERT_TRUE(parser.parseList(json::parse(R"([{"op": "raise", "delta": 2, "shape": {"type": "circle", "center": [0, 0], "radius": 3}}])"), ops));
    const TerrainOpResult result = applyOps(store, ops);
    ASSERT_TRUE(result.ok) << result.error;
    const json out = terrain::opResultJson(result, space, ops);
    ASSERT_EQ(out["changed"].size(), 1u);
    EXPECT_EQ(out["changed"][0]["map"], "height");
    EXPECT_GT(out["changed"][0]["texels"].get<int64_t>(), 0);
    EXPECT_EQ(out["ops"][0]["op"], "raise");
    EXPECT_NEAR(out["stats"]["max_delta"].get<double>(), 2.0, 1e-3) << "a world delta again";
    EXPECT_NEAR(out["stats"]["height_min_before"].get<double>(), 1.0 + 0.25 * 8.0 * 2.0, 1e-3);
}

// AC33 (R78): a game's write moves the maps under private ids first, so the file's pixels and a
// second terrain on the same file stay as authored. Red without the detach: both read the write.
TEST(TerrainRuntimeEdit, WriteDetachesFromTheSharedMap) {
    Scene scene;
    const std::string shared = "t1-runtime-shared-height";
    const Entity edited = runtimeTerrain(scene, shared);
    const Texture first = scene.getComponent<TerrainComponent>(edited).heightMap;
    const Entity other = runtimeTerrain(scene, shared, &first);
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(edited);
    ASSERT_EQ(terrain.heightMap.getData().getData(), scene.getComponent<TerrainComponent>(other).heightMap.getData().getData())
        << "the two terrains start on one image";

    ASSERT_TRUE(detachTerrainMapsForRuntime(terrain, edited));
    EXPECT_EQ(terrain.heightMap.getId().rfind("__terrain_runtime_", 0), 0u) << terrain.heightMap.getId();
    const void* privatePixels = terrain.heightMap.getData().getData();
    ASSERT_TRUE(detachTerrainMapsForRuntime(terrain, edited));
    EXPECT_EQ(terrain.heightMap.getData().getData(), privatePixels) << "once per terrain";

    TerrainComponentStore store(&scene, edited, terrain);
    ASSERT_TRUE(apply(store, runtimeCircle(OpType::Raise, 4.0f)).ok);

    TerrainComponentStore otherStore(&scene, other, scene.getComponent<TerrainComponent>(other));
    const std::vector<float> untouched = readHeights(otherStore);
    for (float value : untouched) {
        ASSERT_FLOAT_EQ(value, static_cast<float>(0x4040) / 65535.0f);
    }
    auto pooled = TextureDataPool::get(shared);
    ASSERT_TRUE(pooled);
    const uint8_t* bytes = static_cast<const uint8_t*>(pooled->at(0).getData());
    for (size_t i = 0; i < static_cast<size_t>(RUNTIME_MAP_SIZE) * RUNTIME_MAP_SIZE * 2; i++) {
        ASSERT_EQ(bytes[i], 0x40);
    }
    EXPECT_NE(readHeights(store), untouched);
}

// AC36 (DC9): a map bigger than the frame's budget uploads every ceil(bytes / budget) frames, and
// the last edit always uploads
TEST(TerrainRuntimeEdit, UploadsArePacedAndTheLastGoes) {
    EXPECT_EQ(terrainUploadInterval(512, 0), 1u);
    EXPECT_EQ(terrainUploadInterval(512, 512), 1u);
    EXPECT_EQ(terrainUploadInterval(513, 512), 2u);
    EXPECT_EQ(terrainUploadInterval(1025u * 1025u * 2u, 1024u * 1024u), 3u) << "AC36's 1025 squared at 1 MB";

    Scene scene;
    const Entity entity = runtimeTerrain(scene, "t1-runtime-paced");
    TerrainComponent& terrain = scene.getComponent<TerrainComponent>(entity);
    TerrainComponentStore store(&scene, entity, terrain);
    ASSERT_TRUE(apply(store, runtimeCircle(OpType::Raise, 2.0f)).ok);
    ASSERT_TRUE(terrain.editState);
    terrain.editState->uploadBudget = 200;   // the 512-byte map goes every third frame
    MeshSystem& system = *scene.getSystem<MeshSystem>();

    std::vector<uint64_t> uploadedAt;
    for (int frame = 0; frame < 7; frame++) {
        if (frame > 0) {
            ASSERT_TRUE(apply(store, runtimeCircle(frame % 2 ? OpType::Lower : OpType::Raise, 2.0f)).ok);
        }
        system.flushTerrainEdit(terrain, 100.0 + frame);
        ASSERT_EQ(terrain.editState->uploads.size(), 1u);
        if (terrain.editState->uploads[0].lastFrame == terrain.editState->frame) uploadedAt.push_back(terrain.editState->frame);
    }
    EXPECT_EQ(uploadedAt, (std::vector<uint64_t>{1, 4, 7}));

    // the edit of frame 8 waits for frame 10, and then goes with no further edit
    ASSERT_TRUE(apply(store, runtimeCircle(OpType::Raise, 2.0f)).ok);
    system.flushTerrainEdit(terrain, 108.0);
    system.flushTerrainEdit(terrain, 109.0);
    EXPECT_TRUE(terrain.editState->uploads[0].waiting);
    system.flushTerrainEdit(terrain, 110.0);
    EXPECT_FALSE(terrain.editState->uploads[0].waiting);
    EXPECT_EQ(terrain.editState->uploads[0].lastFrame, 10u);
}

// AC65 (R46): what a device cannot upload in one go is refused for runtime edits, with the reason
TEST(TerrainRuntimeEdit, TooLargeForRuntimeIsRefused) {
    EXPECT_EQ(terrainRuntimeEditRefusalFor(2049, 2049, true, false, 4, 16 << 20), "") << "2049 is the mobile and web limit";
    EXPECT_NE(terrainRuntimeEditRefusalFor(4097, 4097, true, false, 4, 16 << 20).find("over 2049"), std::string::npos);
    EXPECT_EQ(terrainRuntimeEditRefusalFor(4097, 4097, false, false, 2, 1 << 20), "") << "desktop GL has no staging budget";
    EXPECT_EQ(terrainRuntimeEditRefusalFor(1025, 1025, false, true, 2, 48 << 20), "");
    EXPECT_NE(terrainRuntimeEditRefusalFor(1025, 1025, false, true, 2, 1 << 20).find("stream staging"), std::string::npos)
        << "a small streamStagingBytes refuses on Vulkan";
}

// R45: a GLES3 or WebGL2 upload converts only the rectangle that changed into the float copy
TEST(TerrainRuntimeEdit, HeightRowsConvertToFloats) {
    std::vector<uint16_t> heights(16, 0);
    heights[5] = 65535;
    heights[6] = 32768;
    std::vector<uint8_t> shadow(16 * 4, 0xAB);
    convertTerrainHeightRect(heights.data(), 4, terrain::TexelRect{1, 1, 2, 1}, TerrainUploadFormat::Float32, shadow.data());
    float value = 0.0f;
    std::memcpy(&value, shadow.data() + 5 * 4, 4);
    EXPECT_FLOAT_EQ(value, 1.0f);
    std::memcpy(&value, shadow.data() + 6 * 4, 4);
    EXPECT_NEAR(value, 0.5f, 1e-4f);
    EXPECT_EQ(shadow[0], 0xAB) << "texels outside the rectangle keep what they had";
    EXPECT_EQ(shadow[7 * 4], 0xAB);

    std::vector<uint8_t> halves(16 * 2, 0);
    convertTerrainHeightRect(heights.data(), 4, terrain::TexelRect{0, 0, 4, 4}, TerrainUploadFormat::Half, halves.data());
    uint16_t half = 0;
    std::memcpy(&half, halves.data() + 5 * 2, 2);
    EXPECT_EQ(half, 0x3C00) << "1.0 as an IEEE half";
}
