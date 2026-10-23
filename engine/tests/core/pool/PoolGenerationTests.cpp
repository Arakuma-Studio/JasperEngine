// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The pools' generation counters (S05 R13, AC9): a replaced entry reaches every holder of the
// id on its next use, without a destroy().

#include "pool/FontPool.h"
#include "pool/ModelPool.h"
#include "pool/ObjModelData.h"
#include "pool/SoundPool.h"
#include "pool/TextureDataPool.h"
#include "texture/Texture.h"

#include "soloud_wav.h"

#include <gtest/gtest.h>

using namespace jasperengine;

namespace {

    std::shared_ptr<std::array<TextureData, 6>> solid(unsigned char* pixels) {
        auto faces = std::make_shared<std::array<TextureData, 6>>();
        faces->at(0) = TextureData(1, 1, 4, ColorFormat::RGBA, 4, pixels);
        return faces;
    }

}

TEST(PoolGenerationTests, TextureCopiesRefresh) {
    static unsigned char red[4] = {255, 0, 0, 255};
    static unsigned char green[4] = {0, 255, 0, 255};
    const std::string id = "s05-test:copies";

    Texture first(id, TextureData(1, 1, 4, ColorFormat::RGBA, 4, red));
    Texture second = first;   // a copy, as the undo history and prefab caches keep them
    ASSERT_TRUE(first.load());
    EXPECT_EQ(static_cast<unsigned char*>(first.getData().getData())[0], 255);
    const uint32_t before = TextureDataPool::generation(id);

    TextureDataPool::replace(id, solid(green));
    EXPECT_EQ(TextureDataPool::generation(id), before + 1);

    // both see the new pixels on their next use; neither was destroyed
    TextureLoadResult loaded = first.load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(static_cast<unsigned char*>(loaded.data->at(0).getData())[1], 255);
    EXPECT_EQ(static_cast<unsigned char*>(second.getData().getData())[1], 255);
    EXPECT_EQ(second.getId(), id);

    TextureDataPool::remove(id);
}

TEST(PoolGenerationTests, UntouchedIdsKeepTheirGeneration) {
    static unsigned char blue[4] = {0, 0, 255, 255};
    Texture texture("s05-test:untouched", TextureData(1, 1, 4, ColorFormat::RGBA, 4, blue));
    const uint32_t generation = TextureDataPool::generation("s05-test:untouched");
    TextureDataPool::replace("s05-test:other", solid(blue));
    EXPECT_EQ(TextureDataPool::generation("s05-test:untouched"), generation);
    ASSERT_TRUE(texture.load());
    EXPECT_EQ(static_cast<unsigned char*>(texture.getData().getData())[2], 255);
}

TEST(PoolGenerationTests, SoundReplaceBumpsTheGeneration) {
    const std::string id = "s05-test:sound.wav";
    const uint32_t before = SoundPool::generation(id);
    EXPECT_EQ(SoundPool::get(id), nullptr);
    std::shared_ptr<SoLoud::Wav> held = SoundPool::get(id);
    SoundPool::replace(id, std::make_shared<SoLoud::Wav>());
    EXPECT_EQ(SoundPool::generation(id), before + 1);
    EXPECT_NE(SoundPool::get(id), nullptr);
    EXPECT_NE(SoundPool::get(id), held);
}

TEST(PoolGenerationTests, ModelReplaceBumpsTheGeneration) {
    const std::string id = "s05-test:model.obj";
    auto old = std::make_shared<ObjModelData>();
    ModelPool::addObj(id, old);
    const uint32_t before = ModelPool::generation(id);
    auto fresh = std::make_shared<ObjModelData>();
    ModelPool::replaceObj(id, fresh);
    EXPECT_EQ(ModelPool::generation(id), before + 1);
    EXPECT_EQ(ModelPool::getObj(id), fresh);
    // the old parse lives while someone holds it
    EXPECT_EQ(old.use_count(), 1);
    ModelPool::removeObj(id);
}

TEST(PoolGenerationTests, FontReplaceBumpsTheGeneration) {
    const std::string id = "s05-test:font|12";
    const uint32_t before = FontPool::generation(id);
    auto font = std::make_shared<STBText>();
    FontPool::replace(id, font);
    EXPECT_EQ(FontPool::generation(id), before + 1);
    EXPECT_EQ(FontPool::get(id), font);
    FontPool::remove(id);
}
