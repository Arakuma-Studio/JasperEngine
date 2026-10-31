// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "io/JPak.h"
#include "io/ResourcePack.h"
#include "util/Fnv.h"
#include "TempDir.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    void put32(std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void put64(std::vector<uint8_t>& out, uint64_t v) {
        for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }

    // a pack written by the format in JPak.h (independent of the editor's writer); `corrupt`
    // flips a byte of the last blob after its hash was taken
    std::vector<uint8_t> makePack(const std::vector<std::pair<std::string, std::string>>& files, bool corrupt = false,
                                  uint32_t version = JPak::VERSION) {
        std::vector<uint8_t> index;
        uint64_t indexBytes = 0;
        for (const auto& [path, data] : files) indexBytes += 4 + path.size() + 24;
        uint64_t offset = JPak::HEADER_BYTES + indexBytes;
        std::vector<uint64_t> offsets;
        for (const auto& [path, data] : files) {
            offset = (offset + 15) / 16 * 16;
            offsets.push_back(offset);
            put32(index, static_cast<uint32_t>(path.size()));
            index.insert(index.end(), path.begin(), path.end());
            put64(index, offset);
            put64(index, data.size());
            put64(index, fnv1a64(data.data(), data.size()));
            offset += data.size();
        }
        std::vector<uint8_t> pack(JPak::MAGIC, JPak::MAGIC + 8);
        put32(pack, version);
        put32(pack, static_cast<uint32_t>(files.size()));
        put64(pack, indexBytes);
        pack.insert(pack.end(), index.begin(), index.end());
        for (size_t i = 0; i < files.size(); i++) {
            pack.resize(offsets[i], 0);
            pack.insert(pack.end(), files[i].second.begin(), files[i].second.end());
        }
        if (corrupt && !files.empty() && !files.back().second.empty()) pack[offsets.back()] ^= 0xFF;
        return pack;
    }

    std::string asString(const std::vector<uint8_t>& bytes) {
        return std::string(bytes.begin(), bytes.end());
    }

}

TEST(JPak, ReadsEntriesFromMemoryAndFile) {
    const std::vector<std::pair<std::string, std::string>> files = {
        {"game.json", "{\"schemaVersion\":1}"},
        {"assets/images/logo.png", std::string(1000, 'p')},
        {"lua/main.lua", "print('hi')"},
        {"empty.txt", ""},
    };
    const std::vector<uint8_t> bytes = makePack(files);

    JPak memoryPack;
    std::string error;
    ASSERT_TRUE(memoryPack.openMemory(bytes, &error)) << error;
    ASSERT_EQ(memoryPack.entries().size(), 4u);
    for (const auto& [path, data] : files) {
        std::vector<uint8_t> out;
        ASSERT_TRUE(memoryPack.read(path, out, &error)) << path << ": " << error;
        EXPECT_EQ(asString(out), data);
        std::span<const uint8_t> view;
        ASSERT_TRUE(memoryPack.view(path, view, &error)) << error;
        EXPECT_EQ(std::string(view.begin(), view.end()), data);
    }
    for (const JPak::Entry& entry : memoryPack.entries()) EXPECT_EQ(entry.offset % JPak::ALIGNMENT, 0u);

    test::TempDir dir;
    const auto path = dir.write("content.jpak", asString(bytes));
    JPak filePack;
    ASSERT_TRUE(filePack.openFile(path, &error)) << error;
    std::vector<uint8_t> out;
    ASSERT_TRUE(filePack.read("assets/images/logo.png", out, &error)) << error;
    EXPECT_EQ(asString(out), files[1].second);
    EXPECT_FALSE(filePack.read("assets/missing.png", out, &error));
    EXPECT_NE(error.find("assets/missing.png"), std::string::npos);
}

TEST(JPak, CorruptEntryFailsItsCheck) {
    const std::vector<uint8_t> bytes = makePack({{"a.txt", "aaaa"}, {"b.txt", "bbbbbbbb"}}, true);
    JPak pack;
    std::string error;
    ASSERT_TRUE(pack.openMemory(bytes, &error)) << error;
    std::vector<uint8_t> out;
    EXPECT_TRUE(pack.read("a.txt", out, &error));
    EXPECT_FALSE(pack.read("b.txt", out, &error));
    EXPECT_NE(error.find("b.txt"), std::string::npos);
    EXPECT_NE(error.find("FNV"), std::string::npos);
    std::span<const uint8_t> view;
    EXPECT_FALSE(pack.view("b.txt", view, &error));
}

TEST(JPak, RefusesIllegalPathsAndCaseCollisions) {
    for (const char* path : {"/abs.txt", "C:/x.txt", "a/../b.txt", "a/./b.txt", "a//b.txt", "a\\b.txt", "a/"}) {
        const std::vector<uint8_t> bytes = makePack({{path, "x"}});
        JPak pack;
        std::string error;
        EXPECT_FALSE(pack.openMemory(bytes, &error)) << path;
        EXPECT_NE(error.find("illegal"), std::string::npos) << error;
    }
    const std::vector<uint8_t> bytes = makePack({{"Foo.png", "1"}, {"foo.png", "2"}});
    JPak pack;
    std::string error;
    EXPECT_FALSE(pack.openMemory(bytes, &error));
    EXPECT_NE(error.find("case"), std::string::npos) << error;
}

TEST(JPak, RefusesNewerVersionsAndBadMagic) {
    JPak pack;
    std::string error;
    EXPECT_FALSE(pack.openMemory(makePack({{"a", "b"}}, false, 2), &error));
    EXPECT_NE(error.find("version 2"), std::string::npos) << error;
    std::vector<uint8_t> bad = makePack({{"a", "b"}});
    bad[0] = 'X';
    EXPECT_FALSE(pack.openMemory(bad, &error));
    EXPECT_NE(error.find("magic"), std::string::npos) << error;
    const std::vector<uint8_t> truncated(bad.begin(), bad.begin() + 10);
    EXPECT_FALSE(pack.openMemory(truncated, &error));
}

TEST(JPak, PackKeysOfTheEnginePaths) {
    EXPECT_EQ(ResourcePack::packKey("asset://images/a.png"), "assets/images/a.png");
    EXPECT_EQ(ResourcePack::packKey("lua://main.lua"), "lua/main.lua");
    EXPECT_EQ(ResourcePack::packKey("shader://MESH_x.sdat"), "assets/shaders/MESH_x.sdat");
    EXPECT_EQ(ResourcePack::packKey("content://scenes/3.jscn"), "scenes/3.jscn");
    EXPECT_EQ(ResourcePack::packKey("models/../textures/a.png"), "assets/textures/a.png");
    EXPECT_EQ(ResourcePack::packKey("data://save.json"), "");
    EXPECT_EQ(ResourcePack::packKey("/abs/path.png"), "");
}

TEST(JPak, MountedPackServesDataOpens) {
    static const std::vector<uint8_t> bytes = makePack({{"assets/hello.txt", "hello pack"}, {"game.json", "{}"}});
    std::string error;
    ASSERT_TRUE(ResourcePack::mountContentPack(bytes, &error)) << error;
    EXPECT_TRUE(ResourcePack::contains("asset://hello.txt"));
    EXPECT_TRUE(ResourcePack::contains("content://game.json"));
    std::vector<unsigned char> out;
    ASSERT_TRUE(ResourcePack::read("asset://hello.txt", out));
    EXPECT_EQ(std::string(out.begin(), out.end()), "hello pack");
    std::unique_ptr<unsigned char[]> owned;
    unsigned int size = 0;
    ASSERT_TRUE(ResourcePack::readOwned("hello.txt", owned, size));
    EXPECT_EQ(std::string(reinterpret_cast<char*>(owned.get()), size), "hello pack");
    EXPECT_FALSE(ResourcePack::contains("asset://nothing.txt"));
    // the next test's pack replaces it; an empty one cannot be mounted
    EXPECT_FALSE(ResourcePack::mountContentPack(std::span<const uint8_t>(), &error));
}
