// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// VertexAnimationPool (spec 3.1): .vat + .vat.glb loaded through Data::open under one key for the
// asset-relative and absolute forms, the asset's path = the key, warnings kept with the entry,
// the layout hash checked against the glb, failures not cached, generations from invalidate / put
// / reloads, put of in-memory assets, clear / clearUnused, and Engine::clearPools.

#include <gtest/gtest.h>

#include "VatTestEnvironment.h"

#include "Engine.h"
#include "animation/VertexAnimation.h"
#include "pool/TextureDataPool.h"
#include "pool/VertexAnimationPool.h"
#include "texture/Texture.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    // each test gets fresh file names: generations outlive clear()
    class VatPool : public ::testing::Test {
    protected:
        void SetUp() override {
            static int run = 0;
            prefix = "vat/r" + std::to_string(run++) + "/";
            VertexAnimationPool::clear();
        }
        void TearDown() override { VertexAnimationPool::clear(); }

        std::string p(const std::string& name) const { return prefix + name; }

        // writes "<name>.vat" + its mesh (data.mesh, relative to the .vat) and returns the .vat's
        // asset-relative path
        std::string writePair(const std::string& name, VatData data, const std::string& glbHash = std::string()){
            std::string vat = p(name + ".vat");
            vattest::writeAsset(vat, writeVat(data));
            std::string mesh = std::filesystem::path(vat).parent_path().generic_string() + "/" + data.mesh;
            vattest::writeAsset(mesh, vattest::makeVatGlb(glbHash.empty() ? data.layout.layoutHash : glbHash,
                                                          vattest::samplePayload(data), vattest::sampleSockets(data)));
            return vat;
        }

        std::string prefix;
    };

}

TEST_F(VatPool, LoadsThePairUnderOneKey){
    std::string vat = writePair("hero", vattest::sampleData());
    std::string absolute = vattest::assetDir() + "/" + vat;
    EXPECT_EQ(VertexAnimationPool::getKey(vat), absolute);
    EXPECT_EQ(VertexAnimationPool::getKey(absolute), absolute);
    // the generated mesh names its .vat
    EXPECT_EQ(VertexAnimationPool::getKey(p("hero.vat.glb")), absolute);

    EXPECT_EQ(VertexAnimationPool::get(vat), nullptr);
    EXPECT_EQ(VertexAnimationPool::getGeneration(vat), 0u);
    std::string error;
    std::vector<ClipWarning> warnings;
    std::shared_ptr<const VertexAnimation> asset = VertexAnimationPool::getOrLoad(vat, &error, &warnings);
    ASSERT_TRUE(asset) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(asset->getPath(), absolute);
    EXPECT_EQ(asset->getData().clips.size(), 2u);
    EXPECT_EQ(asset->getPayload().bytes, vattest::samplePayload(vattest::sampleData()).bytes);
    EXPECT_EQ(asset->getSocketTracks().data, vattest::sampleSockets(vattest::sampleData()).data);
    EXPECT_EQ(VertexAnimationPool::size(), 1u);
    EXPECT_GT(VertexAnimationPool::getGeneration(vat), 0u);

    // both forms hit the same entry
    EXPECT_EQ(VertexAnimationPool::get(absolute), asset);
    EXPECT_EQ(VertexAnimationPool::getOrLoad(absolute), asset);
    EXPECT_EQ(VertexAnimationPool::getGeneration(absolute), VertexAnimationPool::getGeneration(vat));
}

TEST_F(VatPool, WarningsAreKeptWithTheEntry){
    VatData data = vattest::sampleData();
    data.clips[1].name = "Walk";
    std::string vat = writePair("dup", data);
    std::vector<ClipWarning> warnings;
    std::shared_ptr<const VertexAnimation> asset = VertexAnimationPool::getOrLoad(vat, nullptr, &warnings);
    ASSERT_TRUE(asset);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].path, "clips[1].name");
    EXPECT_EQ(asset->getData().clips[1].name, "Walk 2");
    // a cached hit reports them again
    std::vector<ClipWarning> again;
    EXPECT_EQ(VertexAnimationPool::getOrLoad(vat, nullptr, &again), asset);
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].path, "clips[1].name");
}

TEST_F(VatPool, MeshPathIsRelativeToTheVat){
    VatData data = vattest::sampleData();
    data.mesh = "../meshes/hero.vat.glb";
    std::string vat = p("assets/hero.vat");
    vattest::writeAsset(vat, writeVat(data));
    vattest::writeAsset(p("meshes/hero.vat.glb"), vattest::makeVatGlb(data.layout.layoutHash, vattest::samplePayload(data),
                                                                       vattest::sampleSockets(data)));
    std::string error;
    EXPECT_TRUE(VertexAnimationPool::getOrLoad(vat, &error)) << error;
}

TEST_F(VatPool, FailuresAreReportedAndNotCached){
    std::string error;
    EXPECT_FALSE(VertexAnimationPool::getOrLoad("", &error));
    EXPECT_EQ(error, "no vertex animation path");
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(p("missing.vat"), &error));
    EXPECT_EQ(error, "cannot read " + p("missing.vat"));

    // not a .vat
    vattest::writeAsset(p("bad.vat"), std::string("{\"format\": \"x\"}"));
    std::vector<ClipWarning> warnings;
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(p("bad.vat"), &error, &warnings));
    EXPECT_EQ(error, "not a vertex animation: \"format\" must be \"jasperengine.vat\"");

    // the glb is missing
    VatData data = vattest::sampleData();
    vattest::writeAsset(p("nomesh.vat"), writeVat(data));
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(p("nomesh.vat"), &error));
    EXPECT_EQ(error.rfind("cannot read " + p("hero.vat.glb") + " (the generated mesh of " + p("nomesh.vat") + "): rebake", 0), 0u)
        << error;

    // the glb is from another bake
    std::string stale = writePair("stale", data, "0000000000000000");
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(stale, &error));
    EXPECT_EQ(error, "generated field layout.layout_hash changed: rebake (Inspector > Rebake, or bake_vertex_animation {rebake})");

    // a corrupt glb: the reader's error after the mesh path
    vattest::writeAsset(p("hero.vat.glb"), std::string("glTF garbage"));
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(p("nomesh.vat"), &error));
    EXPECT_EQ(error.rfind(p("hero.vat.glb") + ": invalid vertex animation mesh: ", 0), 0u) << error;

    // payload / layout disagreement found by create()
    VatData wide = data;
    wide.mesh = "wide.vat.glb";
    std::string wideVat = p("wide.vat");
    vattest::writeAsset(wideVat, writeVat(wide));
    VatData other = wide;
    other.layout.width = 16;
    vattest::writeAsset(p("wide.vat.glb"), vattest::makeVatGlb(wide.layout.layoutHash, vattest::samplePayload(other),
                                                                vattest::sampleSockets(wide)));
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(wideVat, &error));
    EXPECT_NE(error.find("the payload is 16 x 6 float, the layout says 8 x 6 float"), std::string::npos) << error;

    EXPECT_EQ(VertexAnimationPool::size(), 0u);
    // fixed on disk: the next call loads
    writePair("stale", data);
    EXPECT_TRUE(VertexAnimationPool::getOrLoad(stale, &error)) << error;
}

TEST_F(VatPool, InvalidateReloadsAndBumpsTheGeneration){
    VatData data = vattest::sampleData();
    std::string vat = writePair("hero", data);
    std::shared_ptr<const VertexAnimation> first = VertexAnimationPool::getOrLoad(vat);
    ASSERT_TRUE(first);
    uint64_t generation = VertexAnimationPool::getGeneration(vat);

    data.clips[0].name = "Run";
    writePair("hero", data);
    // cached until invalidated
    EXPECT_EQ(VertexAnimationPool::getOrLoad(vat), first);
    VertexAnimationPool::invalidate(vattest::assetDir() + "/" + vat);
    EXPECT_EQ(VertexAnimationPool::get(vat), nullptr);
    uint64_t invalidated = VertexAnimationPool::getGeneration(vat);
    EXPECT_GT(invalidated, generation);
    // a holder keeps its asset
    EXPECT_EQ(first->getData().clips[0].name, "Walk");

    std::shared_ptr<const VertexAnimation> second = VertexAnimationPool::getOrLoad(vat);
    ASSERT_TRUE(second);
    EXPECT_NE(second, first);
    EXPECT_EQ(second->getData().clips[0].name, "Run");
    EXPECT_GT(VertexAnimationPool::getGeneration(vat), invalidated);

    // invalidating the generated mesh invalidates its .vat
    uint64_t before = VertexAnimationPool::getGeneration(vat);
    VertexAnimationPool::invalidate(p("hero.vat.glb"));
    EXPECT_EQ(VertexAnimationPool::get(vat), nullptr);
    EXPECT_GT(VertexAnimationPool::getGeneration(vat), before);

    // invalidating something never loaded does no harm
    VertexAnimationPool::invalidate(p("never.vat"));
    VertexAnimationPool::invalidate("");
    EXPECT_EQ(VertexAnimationPool::getGeneration(p("never.vat")), 0u);
}

TEST_F(VatPool, PutRegistersInMemoryAssets){
    VatData data = vattest::sampleData();
    std::shared_ptr<const VertexAnimation> asset =
        VertexAnimation::create(data, vattest::samplePayload(data), vattest::sampleSockets(data), nullptr);
    ASSERT_TRUE(asset);
    std::string path = p("preview.vat");          // no file behind it
    VertexAnimationPool::put(path, asset);
    std::string key = vattest::assetDir() + "/" + path;
    EXPECT_EQ(asset->getPath(), key);
    EXPECT_EQ(VertexAnimationPool::get(key), asset);
    EXPECT_EQ(VertexAnimationPool::getOrLoad(path), asset);
    uint64_t generation = VertexAnimationPool::getGeneration(path);
    EXPECT_GT(generation, 0u);

    // replacing bumps the generation
    std::shared_ptr<const VertexAnimation> other =
        VertexAnimation::create(data, vattest::samplePayload(data), vattest::sampleSockets(data), nullptr);
    VertexAnimationPool::put(path, other);
    EXPECT_EQ(VertexAnimationPool::get(path), other);
    EXPECT_GT(VertexAnimationPool::getGeneration(path), generation);

    // put(null) drops it
    generation = VertexAnimationPool::getGeneration(path);
    VertexAnimationPool::put(path, nullptr);
    EXPECT_EQ(VertexAnimationPool::get(path), nullptr);
    EXPECT_GT(VertexAnimationPool::getGeneration(path), generation);
    std::string error;
    EXPECT_FALSE(VertexAnimationPool::getOrLoad(path, &error));
}

TEST_F(VatPool, ClearAndClearUnused){
    std::string a = writePair("a", vattest::sampleData());
    std::string b = writePair("b", vattest::sampleData());
    std::shared_ptr<const VertexAnimation> held = VertexAnimationPool::getOrLoad(a);
    ASSERT_TRUE(held);
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(b));
    EXPECT_EQ(VertexAnimationPool::size(), 2u);

    VertexAnimationPool::clearUnused();
    EXPECT_EQ(VertexAnimationPool::size(), 1u);
    EXPECT_EQ(VertexAnimationPool::get(a), held);
    EXPECT_EQ(VertexAnimationPool::get(b), nullptr);

    uint64_t generation = VertexAnimationPool::getGeneration(a);
    VertexAnimationPool::clear();
    EXPECT_EQ(VertexAnimationPool::size(), 0u);
    EXPECT_GT(VertexAnimationPool::getGeneration(a), generation);
    EXPECT_EQ(held->getData().clips.size(), 2u);

    // the engine's pool hooks
    ASSERT_TRUE(VertexAnimationPool::getOrLoad(b));
    Engine::clearUnusedPools();
    EXPECT_EQ(VertexAnimationPool::size(), 0u);
    std::shared_ptr<const VertexAnimation> again = VertexAnimationPool::getOrLoad(b);
    ASSERT_TRUE(again);
    Engine::clearPools();
    EXPECT_EQ(VertexAnimationPool::size(), 0u);
}

TEST_F(VatPool, AssetsWithATextureAreReleasedByThePool){
    std::string vat = writePair("tex", vattest::sampleData());
    std::shared_ptr<const VertexAnimation> asset = VertexAnimationPool::getOrLoad(vat);
    ASSERT_TRUE(asset);
    Texture* texture = asset->getTexture();
    ASSERT_NE(texture, nullptr);
    std::string id = texture->getId();
    EXPECT_EQ(id.rfind("vat:" + asset->getPath() + "#", 0), 0u) << id;
    EXPECT_NE(TextureDataPool::get(id), nullptr);
    asset.reset();
    VertexAnimationPool::clearUnused();
    EXPECT_EQ(TextureDataPool::get(id), nullptr);
}
