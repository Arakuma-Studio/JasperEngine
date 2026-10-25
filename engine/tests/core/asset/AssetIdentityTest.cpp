// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset identity in the engine (docs/specs/S04-asset-identity.md): Guid (text form, the
// frozen derivation), the sub-asset keys, AssetRef and the runtime map (assetmap.bin).

#include <gtest/gtest.h>

#include "asset/AssetMap.h"
#include "asset/AssetRef.h"
#include "asset/Guid.h"
#include "asset/SubAssetKeys.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace jasperengine;

// The derivation is a persistence contract: these never change (AC7). The same vectors run
// in every build of the engine tests (32-bit, wasm).
TEST(GuidDerive, GoldenVectors) {
    struct Vector { const char* parent; const char* key; const char* expected; };
    const Vector vectors[] = {
        {"", "mesh#0", "756be02d06524504bb6870e9e23babed"},
        {"4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "mesh:Body", "e8c1a89d80fc4604a23c94e803460e54"},
        {"4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "mesh#3", "dc9dd6f5d5714824a71306cec7769f0e"},
        {"4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "material:Skin", "8197a941768645b683dc239352a6bba7"},
        {"4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "animation#0", "7022206e7257438fbacb0d62e2caaf9f"},
        {"9c1d0f3e5b7a42c8a1e06d2f4b8c7a19", "skin#1", "e0be425380ea4ace9fa9829c21fa4bf8"},
        {"9c1d0f3e5b7a42c8a1e06d2f4b8c7a19", "image:Albedo.png", "d40fea0dd3de4f6ba2dc5fc8bbc0f965"},
        {"ffffffffffffffffffffffffffffffff", "mesh:\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9", "c1d273bde94b4a3b9723aa6845482aa6"},
        {"00000000000000000000000000000001", "", "0ae4f4d1ff02474da1770c1fb82d379c"},
        {"0123456789abcdef0123456789abcdef", "project.yaml", "7109918c2d1a403485c7f49402610a42"},
    };
    for (const Vector& vector : vectors) {
        EXPECT_EQ(Guid::derive(Guid::parse(vector.parent), vector.key).str(), vector.expected) << vector.parent << " " << vector.key;
    }
}

TEST(GuidDerive, DependsOnParentAndKey) {
    const Guid parent = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    EXPECT_EQ(Guid::derive(parent, "mesh:Body"), Guid::derive(parent, "mesh:Body"));
    EXPECT_NE(Guid::derive(parent, "mesh:Body"), Guid::derive(parent, "mesh:Bodz"));
    EXPECT_NE(Guid::derive(parent, "mesh:Body"), Guid::derive(Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c14"), "mesh:Body"));
}

TEST(Guid, TextRoundTrip) {
    const std::string text = "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13";
    const Guid guid = Guid::parse(text);
    EXPECT_FALSE(guid.isNull());
    EXPECT_EQ(guid.str(), text);
    EXPECT_EQ(guid.hi, 0x4b1e9c0d2a7f4e31ULL);
    EXPECT_EQ(guid.lo, 0xb6c58d0f9e2a7c13ULL);
    // either case and surrounding spaces read the same GUID; it is written lowercase
    EXPECT_EQ(Guid::parse("  4B1E9C0D2A7F4E31B6C58D0F9E2A7C13 "), guid);
    uint8_t bytes[16];
    guid.toBytes(bytes);
    EXPECT_EQ(bytes[0], 0x4b);
    EXPECT_EQ(bytes[15], 0x13);
    EXPECT_EQ(Guid::fromBytes(bytes), guid);
}

TEST(Guid, InvalidTextIsNull) {
    EXPECT_TRUE(Guid::parse("").isNull());
    EXPECT_TRUE(Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c1").isNull());   // 31 digits
    EXPECT_TRUE(Guid::parse("4b1e9c0d-2a7f-4e31-b6c5-8d0f9e2a7c13").isNull());
    EXPECT_TRUE(Guid::parse("zb1e9c0d2a7f4e31b6c58d0f9e2a7c13").isNull());
    EXPECT_FALSE(Guid::isValidText("textures/a.png"));
    EXPECT_EQ(Guid().str(), "");
}

TEST(SubAssetKeys, NamedUniqueElseIndexed) {
    const std::vector<std::string> keys = subAssetKeys(SubAssetKind::Mesh, {"Body", "", "Wheel", "Wheel", "Head"});
    ASSERT_EQ(keys.size(), 5u);
    EXPECT_EQ(keys[0], "mesh:Body");
    EXPECT_EQ(keys[1], "mesh#1");    // unnamed
    EXPECT_EQ(keys[2], "mesh#2");    // a name two meshes share
    EXPECT_EQ(keys[3], "mesh#3");
    EXPECT_EQ(keys[4], "mesh:Head");
    EXPECT_EQ(subAssetKeys(SubAssetKind::Animation, {""})[0], "animation#0");
    EXPECT_EQ(subAssetKeys(SubAssetKind::Image, {"a.png"})[0], "image:a.png");
}

TEST(SubAssetKeys, ParseAndSplit) {
    SubAssetKind kind;
    std::string name;
    long index;
    ASSERT_TRUE(parseSubAssetKey("material:Skin#2", kind, name, index));
    EXPECT_EQ(kind, SubAssetKind::Material);
    EXPECT_EQ(name, "Skin#2");
    EXPECT_EQ(index, -1);
    ASSERT_TRUE(parseSubAssetKey("skin#7", kind, name, index));
    EXPECT_EQ(kind, SubAssetKind::Skin);
    EXPECT_EQ(index, 7);
    EXPECT_FALSE(parseSubAssetKey("texture:x", kind, name, index));
    EXPECT_FALSE(parseSubAssetKey("mesh#x", kind, name, index));

    std::string container, key;
    ASSERT_TRUE(splitSubAssetPath("models/we#ird.glb#mesh:Arm/Left", container, key));
    EXPECT_EQ(container, "models/we#ird.glb");
    EXPECT_EQ(key, "mesh:Arm/Left");
    ASSERT_TRUE(splitSubAssetPath("hero.gltf#animation#3", container, key));
    EXPECT_EQ(container, "hero.gltf");
    EXPECT_EQ(key, "animation#3");
    EXPECT_FALSE(splitSubAssetPath("textures/a#b.png", container, key));
    EXPECT_FALSE(splitSubAssetPath("hero.glb", container, key));
}

TEST(AssetRef, AssigningAPathDropsTheGuid) {
    const Guid guid = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    AssetRef ref("models/a.glb", guid);
    ref = std::string("models/a.glb");   // the same path keeps the identity
    EXPECT_EQ(ref.guid, guid);
    ref = "models/b.glb";                // another file
    EXPECT_TRUE(ref.guid.isNull());
    EXPECT_EQ(ref.path, "models/b.glb");
    ref.set("models/c.glb", guid);
    EXPECT_EQ(ref.guid, guid);
    ref.clear();
    EXPECT_TRUE(ref.empty());
    EXPECT_FALSE(ref.isSet());
}

TEST(AssetRef, BehavesAsItsPath) {
    AssetRef ref("sounds/hit.wav", Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"));
    const std::string& text = ref;
    EXPECT_EQ(text, "sounds/hit.wav");
    EXPECT_TRUE(ref == "sounds/hit.wav");
    EXPECT_TRUE(std::string("sounds/hit.wav") == ref);
    EXPECT_EQ(ref + ".bak", "sounds/hit.wav.bak");
    EXPECT_EQ(ref.substr(0, 6), "sounds");
    EXPECT_EQ(ref.find('/'), 6u);
    EXPECT_STREQ(ref.c_str(), "sounds/hit.wav");
    EXPECT_EQ(std::filesystem::path(ref).filename().string(), "hit.wav");
    // a reference is its path and its GUID
    EXPECT_NE(ref, AssetRef("sounds/hit.wav"));
}

TEST(ShaderRef, CarriesBothFiles) {
    ShaderRef ref("shaders/water", Guid::parse("00000000000000000000000000000001"), Guid::parse("00000000000000000000000000000002"));
    EXPECT_FALSE(ref.isLibrary());
    ref = std::string("library:unlit");
    EXPECT_TRUE(ref.isLibrary());
    EXPECT_TRUE(ref.vertGuid.isNull() && ref.fragGuid.isNull());
}

namespace {

    std::vector<AssetMap::Entry> sampleEntries() {
        return {
            {Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"), AssetType::Texture, false, "textures/hero.png"},
            {Guid::parse("9c1d0f3e5b7a42c8a1e06d2f4b8c7a19"), AssetType::Texture, true, "textures/hero.png"},
            {Guid::parse("0123456789abcdef0123456789abcdef"), AssetType::Model, false, "models/ship.glb"},
            {Guid::parse("e8c1a89d80fc4604a23c94e803460e54"), AssetType::SubAsset, false, "models/ship.glb#mesh:Body"},
            {Guid::parse("ffffffffffffffffffffffffffffffff"), AssetType::Scene, false, "Main Scene.scene"},
        };
    }

}

TEST(AssetMap, EncodedMapAnswersByGuidAndPath) {
    const std::vector<uint8_t> bytes = AssetMap::encode(sampleEntries());
    ASSERT_TRUE(AssetMap::loadFromMemory(bytes.data(), bytes.size()));

    std::string path;
    AssetType type = AssetType::Unknown;
    ASSERT_TRUE(AssetMap::findPath(Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"), path, &type));
    EXPECT_EQ(path, "textures/hero.png");
    EXPECT_EQ(type, AssetType::Texture);
    // a former GUID is a redirect to its asset's path
    ASSERT_TRUE(AssetMap::findPath(Guid::parse("9c1d0f3e5b7a42c8a1e06d2f4b8c7a19"), path, &type));
    EXPECT_EQ(path, "textures/hero.png");
    ASSERT_TRUE(AssetMap::findPath(Guid::parse("e8c1a89d80fc4604a23c94e803460e54"), path, &type));
    EXPECT_EQ(path, "models/ship.glb#mesh:Body");
    EXPECT_EQ(type, AssetType::SubAsset);
    EXPECT_FALSE(AssetMap::findPath(Guid::parse("00000000000000000000000000000009"), path));

    // the live GUID of a path, never a former one
    EXPECT_EQ(AssetMap::findGuid("textures/hero.png").str(), "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    EXPECT_EQ(AssetMap::findGuid("Main Scene.scene").str(), "ffffffffffffffffffffffffffffffff");
    EXPECT_TRUE(AssetMap::findGuid("textures/none.png").isNull());
    EXPECT_EQ(AssetMap::findType("models/ship.glb"), AssetType::Model);

    const ScriptAssetRef ref = ScriptAssetRef::make("models/ship.glb");
    EXPECT_TRUE(ref.isValid());
    EXPECT_EQ(ref.getGuid(), "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(ref.getType(), "model");
    EXPECT_FALSE(ScriptAssetRef::make("00000000000000000000000000000009").isValid());
    AssetMap::reset();
}

TEST(AssetMap, EncodingIsDeterministicAndCompact) {
    std::vector<AssetMap::Entry> entries = sampleEntries();
    const std::vector<uint8_t> first = AssetMap::encode(entries);
    std::reverse(entries.begin(), entries.end());
    EXPECT_EQ(AssetMap::encode(entries), first);
    // header, 20 bytes per entry, then one type byte and the NUL-ended path per entry
    size_t blob = 0;
    for (const AssetMap::Entry& entry : entries) blob += 1 + entry.path.size() + 1;
    EXPECT_EQ(first.size(), 16 + entries.size() * 20 + blob);
}

TEST(AssetMap, MalformedMapIsRejected) {
    std::vector<uint8_t> bytes = AssetMap::encode(sampleEntries());
    bytes[0] = 'X';
    EXPECT_FALSE(AssetMap::loadFromMemory(bytes.data(), bytes.size()));
    bytes = AssetMap::encode(sampleEntries());
    bytes.resize(bytes.size() - 3);
    EXPECT_FALSE(AssetMap::loadFromMemory(bytes.data(), bytes.size()));
    std::string path;
    EXPECT_FALSE(AssetMap::findPath(Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13"), path));
    AssetMap::reset();
}
