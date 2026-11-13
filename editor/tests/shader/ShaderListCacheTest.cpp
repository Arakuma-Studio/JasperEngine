// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the shader list keeps of closed scenes between sessions (docs/specs/S64-shader-lists.md). The
// list itself runs in the editor: tools/mcp/tests/test_shader_lists.py.

#include <gtest/gtest.h>

#include "pool/ShaderPool.h"
#include "shader/ShaderListCache.h"
#include "util/FileUtils.h"

#include "support/PosixCompat.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    fs::path scratchCacheFile(const std::string& name) {
        const fs::path dir = fs::temp_directory_path() / ("jasper-shader-list-test-" + name + "-" + std::to_string(::getpid()));
        fs::remove_all(dir);
        // made by the save, as in a project that has no .jasperengine yet
        return dir / ".jasperengine" / "shader-lists.yaml";
    }

    ShaderListCache::Entry sampleEntry() {
        ShaderListCache::Entry entry;
        entry.sha1 = "abc";
        entry.writeTime = 1234567890123;
        entry.size = 42;
        entry.context = "ctx";
        entry.keys.insert(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, 0x41)));
        entry.keys.insert(ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::SKYBOX, 0x1)));
        entry.prefabs = {"prefabs/A.prefab", "prefabs/B.prefab"};
        return entry;
    }

}

TEST(ShaderListCache, KeepsEntriesBetweenSessions) {
    const fs::path file = scratchCacheFile("roundtrip");
    ShaderListCache cache;
    ShaderListCache::Entry entry = sampleEntry();
    // a fork's key is written by its path: the id is this session's
    const uint16_t fork = ShaderPool::registerCustomShader("shaders/forked");
    const ShaderKey forked = ShaderPool::normalizeKey(ShaderPool::getShaderKey(ShaderType::MESH, 0x41, fork));
    entry.keys.insert(forked);
    cache.store("scenes/A.scene", entry);
    EXPECT_TRUE(cache.isDirty());
    ASSERT_TRUE(ShaderListCache::write(file, cache.takeEntriesToWrite()));
    EXPECT_FALSE(cache.isDirty());
    cache.markDirty();
    EXPECT_TRUE(cache.isDirty()) << "a failed write is tried again";

    ShaderListCache next;
    ASSERT_TRUE(next.load(file));
    EXPECT_FALSE(next.isDirty());
    const ShaderListCache::Entry* read = next.find("scenes/A.scene");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->sha1, entry.sha1);
    EXPECT_EQ(read->writeTime, entry.writeTime);
    EXPECT_EQ(read->size, entry.size);
    EXPECT_EQ(read->context, entry.context);
    EXPECT_EQ(read->keys, entry.keys);
    EXPECT_EQ(read->prefabs, entry.prefabs);
    EXPECT_EQ(next.find("scenes/B.scene"), nullptr);
    fs::remove_all(file.parent_path().parent_path());
}

TEST(ShaderListCache, StampChangesKeepTheAnswer) {
    ShaderListCache cache;
    cache.store("scenes/A.scene", sampleEntry());
    const std::set<ShaderKey> keys = cache.find("scenes/A.scene")->keys;
    cache.updateStamp("scenes/A.scene", 7, 8);
    cache.updateStamp("scenes/Unknown.scene", 7, 8);
    EXPECT_EQ(cache.find("scenes/A.scene")->writeTime, 7);
    EXPECT_EQ(cache.find("scenes/A.scene")->size, 8u);
    EXPECT_EQ(cache.find("scenes/A.scene")->keys, keys);
    EXPECT_EQ(cache.find("scenes/Unknown.scene"), nullptr);
}

// A cache is only worth its answers: whatever is wrong with the file, the keys are computed again
TEST(ShaderListCache, AnUnusableFileIsAnEmptyCache) {
    const fs::path file = scratchCacheFile("unusable");
    ShaderListCache cache;
    cache.store("scenes/A.scene", sampleEntry());
    ASSERT_TRUE(ShaderListCache::write(file, cache.takeEntriesToWrite()));
    const auto write = [&file](const std::string& text) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    };

    for (const std::string& text : {std::string("version: [not yaml\n"), std::string("version: 999\nscenes: {}\n"),
                                    std::string("scenes: {}\n"), std::string("version: 1\nscenes:\n  a: {sha1: x}\n")}) {
        write(text);
        ShaderListCache read;
        read.store("scenes/Earlier.scene", sampleEntry());
        EXPECT_FALSE(read.load(file)) << text;
        EXPECT_EQ(read.find("scenes/Earlier.scene"), nullptr) << text;
        EXPECT_FALSE(read.isDirty()) << text;
    }

    ShaderListCache none;
    EXPECT_FALSE(none.load(file.parent_path() / "missing.yaml"));
    fs::remove_all(file.parent_path().parent_path());
}
