// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Spec S14 R12 / AC13: the texture quality setting drops top mips of streamable cooked textures
// and leaves their logical size; the memory budget drops one more mip from the largest
// streamable texture until the resident bytes fit, each keeping at least 64 px, never touching
// sprites and UI.

#include "Engine.h"
#include "jobs/JobSystem.h"
#include "pool/TextureDataPool.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <map>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a 256 px RGBA8 cook with its full chain, as the raw writer stores it
    std::vector<uint8_t> budgetKtx2(int seed){
        std::vector<std::vector<uint8_t>> levels;
        int size = 256;
        while (true){
            std::vector<uint8_t> level((size_t)size * size * 4);
            for (size_t i = 0; i < level.size(); i++)
                level[i] = (uint8_t)((i * 31 + (size_t)seed * 17 + (size_t)size) & 0xFF);
            levels.push_back(std::move(level));
            if (size == 1)
                break;
            size /= 2;
        }
        Ktx2::WriteDesc desc;
        desc.format = GpuTextureFormat::RGBA8;
        desc.srgb = true;
        desc.width = desc.height = 256;
        for (const std::vector<uint8_t>& level : levels)
            desc.levels.push_back(std::span<const uint8_t>(level));
        std::vector<uint8_t> out;
        std::string error;
        EXPECT_TRUE(Ktx2::write(desc, out, error)) << error;
        return out;
    }

    class TextureBudget : public ::testing::Test {
    protected:
        TempDir dir;
        std::map<std::string, CookedTextureEntry> entries;
        std::vector<std::string> ids;

        void addCooked(const std::string& assetPath, bool streamable, int seed){
            const std::vector<uint8_t> bytes = budgetKtx2(seed);
            const std::string file = dir.file(assetPath + ".ktx2").generic_string();
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            CookedTextureEntry entry;
            entry.file = file;
            entry.width = entry.height = 256;
            entry.format = "RGBA8";
            entry.flags = streamable ? TEXTURE_FLAG_STREAMABLE : 0;
            entry.levels = 9;
            entry.bytes = bytes.size();
            entry.tailBytes = bytes.size();
            entries[assetPath] = entry;
        }

        void SetUp() override{
            for (int i = 0; i < 4; i++)
                addCooked("budget/texture" + std::to_string(i) + ".png", true, i);
            addCooked("budget/sprite.png", false, 9);
            std::map<std::string, CookedTextureEntry>* table = &entries;
            CookedTextures::setProvider([table](const std::string& path, CookedTextureEntry& out){
                auto it = table->find(path);
                if (it == table->end())
                    return false;
                out = it->second;
                return true;
            }, "desktop");
        }

        void TearDown() override{
            for (const std::string& id : ids)
                TextureDataPool::remove(id);
            TextureDataPool::setMemoryBudget(0);
            TextureDataPool::setTextureQuality(0);
            settle();
            CookedTextures::setProvider(nullptr, "");
        }

        std::shared_ptr<std::array<TextureData, 6>> load(const std::string& path){
            ids.push_back(path);
            TextureLoadResult result;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
                result = TextureDataPool::loadFromFile(path, {path, "", "", "", "", ""}, 1);
                if (result.state == ResourceLoadState::Loading){
                    jobs::drainMainQueue();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } while (result.state == ResourceLoadState::Loading && std::chrono::steady_clock::now() < deadline);
            EXPECT_EQ(result.state, ResourceLoadState::Finished) << path << ": " << result.errorMessage;
            return result.data;
        }

        // the reloads the budget planned, adopted
        void settle(){
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
                TextureDataPool::update();
                jobs::drainMainQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (TextureDataPool::pendingWorkCount() > 0 && std::chrono::steady_clock::now() < deadline);
            TextureDataPool::update();
        }

        TextureResidentInfo resident(const std::string& id){
            for (const TextureResidentInfo& info : TextureDataPool::getResidentTextures())
                if (info.id == id)
                    return info;
            ADD_FAILURE() << id << " is not resident";
            return {};
        }
    };

}

TEST_F(TextureBudget, QualityDropsTopMipsAndKeepsTheLogicalSize){
    TextureDataPool::setTextureQuality(1);
    auto data = load("budget/texture0.png");
    ASSERT_TRUE(data);
    const TextureResidentInfo info = resident("budget/texture0.png");
    EXPECT_TRUE(info.cooked);
    EXPECT_TRUE(info.streamable);
    EXPECT_EQ(info.storedWidth, 128);
    EXPECT_EQ(info.logicalWidth, 256);
    EXPECT_EQ(info.droppedLevels, 1);
    // a quarter of the bytes: half the edge
    EXPECT_EQ(info.bytes, (uint64_t)((128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4));
    EXPECT_EQ(data->at(0).getOriginalWidth(), 256);
    // sprites and UI keep every level
    load("budget/sprite.png");
    EXPECT_EQ(resident("budget/sprite.png").storedWidth, 256);
    // back to full quality: the next update reloads it
    TextureDataPool::setTextureQuality(0);
    settle();
    EXPECT_EQ(resident("budget/texture0.png").storedWidth, 256);
}

TEST_F(TextureBudget, TheLargestStreamableDropsUntilItFits){
    for (int i = 0; i < 4; i++)
        load("budget/texture" + std::to_string(i) + ".png");
    load("budget/sprite.png");
    const uint64_t full = TextureDataPool::getResidentBytes();
    EXPECT_GT(full, 1700000u);

    TextureDataPool::setMemoryBudget(1000000);
    settle();
    EXPECT_LE(TextureDataPool::getResidentBytes(), 1000000u);
    int dropped = 0;
    for (int i = 0; i < 4; i++){
        const TextureResidentInfo info = resident("budget/texture" + std::to_string(i) + ".png");
        dropped += info.storedWidth < 256 ? 1 : 0;
        EXPECT_EQ(info.logicalWidth, 256);
    }
    EXPECT_EQ(dropped, 3);      // three drops fit it, the fourth stays whole
    EXPECT_EQ(resident("budget/sprite.png").storedWidth, 256);

    // a budget that cannot be met: every streamable texture keeps 64 px, the sprite all of it
    TextureDataPool::setMemoryBudget(100000);
    settle();
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(resident("budget/texture" + std::to_string(i) + ".png").storedWidth, 64);
    EXPECT_EQ(resident("budget/sprite.png").storedWidth, 256);

    // no budget: everything comes back
    TextureDataPool::setMemoryBudget(0);
    settle();
    EXPECT_EQ(TextureDataPool::getResidentBytes(), full);
}
