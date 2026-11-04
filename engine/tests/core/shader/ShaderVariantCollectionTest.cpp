// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Variant keys (ShaderPool: type, property bits, custom shader id) and the collections of
// them a build warms up (Unity's ShaderVariantCollection). Nothing is compiled here.

#include "shader/ShaderVariantCollection.h"
#include "pool/ShaderPool.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

TEST(ShaderKey, PacksTypePropertiesAndCustomId){
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, 0x41, 3);
    EXPECT_EQ(key.type, uint16_t(ShaderType::MESH));
    EXPECT_EQ(key.properties, 0x41u);
    EXPECT_EQ(key.customId, 3u);
    EXPECT_EQ(ShaderPool::getShaderTypeFromKey(key), ShaderType::MESH);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), 0x41u);
    EXPECT_EQ(ShaderPool::getCustomIdFromKey(key), 3u);
}

TEST(ShaderKey, BuiltInKeysHaveNoCustomId){
    EXPECT_EQ(ShaderPool::getShaderKey(ShaderType::UI, 2), ShaderPool::getShaderKey(ShaderType::UI, 2, 0));
    EXPECT_EQ(ShaderPool::registerCustomShader(""), 0u);
}

TEST(ShaderKey, StorageKeyDropsTheSessionCustomId){
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, 7, 12);
    EXPECT_EQ(ShaderPool::getStorageKey(key), ShaderPool::getShaderKey(ShaderType::MESH, 7));
}

TEST(ShaderKey, CustomShadersAreInterned){
    const uint16_t first = ShaderPool::registerCustomShader("shaders/test_variant_a");
    EXPECT_NE(first, 0u);
    EXPECT_EQ(ShaderPool::registerCustomShader("shaders/test_variant_a"), first);
    EXPECT_NE(ShaderPool::registerCustomShader("shaders/test_variant_b"), first);
    EXPECT_EQ(ShaderPool::getCustomShaderName(first), "shaders/test_variant_a");
}

TEST(ShaderKey, NormalizeDropsBitsWithoutAProperty){
    const uint64_t valid = ShaderPool::getValidPropertyMask(ShaderType::MESH);
    // MESH bit 5, the old 'Pcf', is Stochastic Tiling since the 64-bit keys (the stale bit of
    // older files is stripped where they are read, editor::Stream); bit 63 names nothing
    EXPECT_NE(valid & (uint64_t(1) << 5), 0u);
    EXPECT_EQ(valid & (uint64_t(1) << 63), 0u);
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, ~uint64_t(0), 2);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(ShaderPool::normalizeKey(key)), valid);
    EXPECT_EQ(ShaderPool::getCustomIdFromKey(ShaderPool::normalizeKey(key)), 2u);
}

TEST(ShaderKey, ShadowsHQNeedsShadows){
    // MESH bit 33 ('Shq', S10; bit 5 is upstream's 'Sto'): a key keeps it only with 'Shw' (bit 4)
    const uint64_t hq = ShaderPool::getShadowsHQProperty();
    EXPECT_EQ(hq, uint64_t(1) << 33);
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 33, true), "Shq");
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 5, true), "Sto");
    EXPECT_NE(ShaderPool::getValidPropertyMask(ShaderType::MESH) & hq, 0u);
    const ShaderKey shadowed = ShaderPool::getShaderKey(ShaderType::MESH, 0x8 | (uint64_t(1) << 4) | hq);
    EXPECT_EQ(ShaderPool::normalizeKey(shadowed), shadowed);
    const ShaderKey unshadowed = ShaderPool::getShaderKey(ShaderType::MESH, 0x8 | hq);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(ShaderPool::normalizeKey(unshadowed)), 0x8u);
}

TEST(ShaderKey, NormalizeDropsStaleTwoDTangents){
    // the built-in Lit's unlit 2D light variant ('Ult' 0, 'L2d' 22) took 'Tan' (8) without a
    // normal map ('Nmp' 7) until November 2026; the 2D lights read tangents only for the map
    const uint64_t ult = uint64_t(1) << 0, nmp = uint64_t(1) << 7, tan = uint64_t(1) << 8, l2d = uint64_t(1) << 22;
    EXPECT_EQ(ShaderPool::getShaderPropertyName(ShaderType::MESH, 22, true), "L2d");
    const ShaderKey stale = ShaderPool::getShaderKey(ShaderType::MESH, ult | tan | l2d);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(ShaderPool::normalizeKey(stale)), ult | l2d);
    // with a normal map, lit, or a shader of its own (a library shader may @require tangents) it stays
    const ShaderKey mapped = ShaderPool::getShaderKey(ShaderType::MESH, ult | nmp | tan | l2d);
    EXPECT_EQ(ShaderPool::normalizeKey(mapped), mapped);
    const ShaderKey lit = ShaderPool::getShaderKey(ShaderType::MESH, tan | l2d | (uint64_t(1) << 3));
    EXPECT_EQ(ShaderPool::normalizeKey(lit), lit);
    const ShaderKey custom = ShaderPool::getShaderKey(ShaderType::MESH, ult | tan | l2d, 3);
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(ShaderPool::normalizeKey(custom)), ult | tan | l2d);
}

TEST(ShaderKey, SpecsNameTypeAndProperties){
    const std::string first = ShaderPool::getShaderPropertyName(ShaderType::MESH, 0, true);
    const std::string second = ShaderPool::getShaderPropertyName(ShaderType::MESH, 1, true);
    ShaderKey key;
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:" + first + "," + second, key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::MESH, 0x3));
    ASSERT_TRUE(ShaderPool::parseShaderSpec("mesh:0x41", key));
    EXPECT_EQ(ShaderPool::getPropertiesFromKey(key), 0x41u);
    ASSERT_TRUE(ShaderPool::parseShaderSpec("ui", key));
    EXPECT_EQ(key, ShaderPool::getShaderKey(ShaderType::UI, 0));
}

TEST(ShaderKey, BadSpecsAreRefusedWithAReason){
    ShaderKey key;
    std::string error;
    EXPECT_FALSE(ShaderPool::parseShaderSpec("wobbly:Ult", key, &error));
    EXPECT_NE(error.find("Unknown shader type"), std::string::npos);
    EXPECT_FALSE(ShaderPool::parseShaderSpec("mesh:NoSuchProperty", key, &error));
    EXPECT_NE(error.find("Unknown shader property"), std::string::npos);
    EXPECT_FALSE(ShaderPool::parseShaderSpec("mesh/named", key, &error));
}

TEST(ShaderKey, ShaderStringsRoundTrip){
    const ShaderKey key = ShaderPool::getShaderKey(ShaderType::MESH, 0x3);
    const std::string str = ShaderPool::getShaderStr(ShaderType::MESH, 0x3);
    ShaderKey back;
    ASSERT_TRUE(ShaderPool::parseShaderStr(str, back)) << str;
    EXPECT_EQ(back, key);
}

TEST(ShaderVariantCollection, AddsEachVariantOnce){
    ShaderVariantCollection collection;
    EXPECT_TRUE(collection.add(ShaderType::MESH, 0x3));
    EXPECT_FALSE(collection.add(ShaderType::MESH, 0x3));
    EXPECT_TRUE(collection.add(ShaderType::UI, 0));
    EXPECT_EQ(collection.getVariantCount(), 2u);
    EXPECT_EQ(collection.getVariants()[0], ShaderPool::getShaderKey(ShaderType::MESH, 0x3));
}

TEST(ShaderVariantCollection, KeysAreNormalized){
    ShaderVariantCollection collection;
    EXPECT_TRUE(collection.addKey(ShaderPool::getShaderKey(ShaderType::MESH, 0x1 | (uint64_t(1) << 63))));
    EXPECT_TRUE(collection.contains(ShaderType::MESH, 0x1));
    EXPECT_FALSE(collection.add(ShaderType::MESH, 0x1));
}

TEST(ShaderVariantCollection, CustomShadersAreSeparateVariants){
    ShaderVariantCollection collection;
    EXPECT_TRUE(collection.add(ShaderType::MESH, 0x1));
    EXPECT_TRUE(collection.add(ShaderType::MESH, 0x1, "shaders/collection_fork"));
    EXPECT_TRUE(collection.contains(ShaderType::MESH, 0x1, "shaders/collection_fork"));
    EXPECT_EQ(collection.getVariantCount(), 2u);
}

TEST(ShaderVariantCollection, SpecsAddContainAndRemove){
    ShaderVariantCollection collection;
    EXPECT_TRUE(collection.add("mesh:0x1"));
    EXPECT_TRUE(collection.contains("mesh:0x1"));
    EXPECT_TRUE(collection.contains(ShaderType::MESH, 0x1));
    EXPECT_FALSE(collection.add("wobbly"));
    EXPECT_TRUE(collection.remove("mesh:0x1"));
    EXPECT_FALSE(collection.remove("mesh:0x1"));
    EXPECT_EQ(collection.getVariantCount(), 0u);
}

TEST(ShaderVariantCollection, ClearAndCopy){
    ShaderVariantCollection collection;
    collection.add(ShaderType::MESH, 0x1);
    collection.add(ShaderType::SKYBOX, 0);
    ShaderVariantCollection copy(collection);
    collection.clear();
    EXPECT_EQ(collection.getVariantCount(), 0u);
    EXPECT_EQ(copy.getVariantCount(), 2u);
    ShaderVariantCollection assigned;
    assigned = copy;
    EXPECT_TRUE(assigned.contains(ShaderType::SKYBOX, 0));
}

TEST(ShaderVariantCollection, NothingIsWarmedUpWithoutWarmUp){
    ShaderVariantCollection collection;
    collection.add(ShaderType::MESH, 0x1);
    EXPECT_EQ(collection.getWarmedUpVariantCount(), 0u);
    EXPECT_FALSE(collection.isWarmedUp());
}
