// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The save file format (docs/specs/savegame.md R18, R19, R45; AC5-AC7): version 1 frozen by a
// committed golden, every damage rejected before an allocation it would size.

#include "save/SaveFormat.h"
#include "util/ContentHash.h"

#include "support/AllocationCounter.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace SaveGameFormatTest_cpp {

using namespace jasperengine;

namespace {

    std::filesystem::path saveGoldenPath(){
        return std::filesystem::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "fixtures" / "save" / "v1.jsave";
    }

    std::string savePayload(const char* json){
        std::string payload;
        savegame::SaveError error;
        EXPECT_TRUE(savegame::fromJson(nlohmann::json::parse(json), payload, error)) << error.message;
        return payload;
    }

    // The SaveFile the golden holds
    savegame::SaveFile saveGoldenFile(){
        savegame::SaveFile file;
        file.generation = 7;
        file.savedAtMs = 1767225600000;
        file.dataVersion = 3;
        savegame::SaveMetadata metadata;
        metadata.meta = savePayload(R"({"title":"Forest - 2h 13m","playTime":7980})");
        metadata.requirements = savePayload(R"({"dlc1":2})");
        savegame::appendString(metadata.engine, "golden");
        savegame::appendString(metadata.platform, "linux");
        file.metadata = savegame::buildMetadata(metadata);
        file.sections.push_back({"inventory", std::make_shared<const std::string>(savePayload(R"({"#1":"sword","#3":"shield","gold":120})"))});
        file.sections.push_back({"main", std::make_shared<const std::string>(savePayload(
            R"({"level":"forest","pos":{"$vec3":[4.0,0.0,2.0]},"hp":0.75,"flags":[true,false,null]})"))});
        return file;
    }

    std::string readSaveBytes(const std::filesystem::path& path){
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void putLE32(std::string& bytes, size_t offset, uint32_t value){
        for (int i = 0; i < 4; i++) bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }

    // A footer checksum that matches the (patched) bytes again
    void resealSave(std::string& bytes){
        const size_t body = bytes.size() - savegame::FOOTER_BYTES;
        const uint64_t hash = hashContent64(bytes.data(), body, 0);
        for (int i = 0; i < 8; i++) bytes[body + i] = static_cast<char>((hash >> (8 * i)) & 0xFF);
    }

}

TEST(SaveGameFormat, Version1Golden){
    const std::string built = savegame::buildSaveFile(saveGoldenFile());
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")){
        std::filesystem::create_directories(saveGoldenPath().parent_path());
        std::ofstream(saveGoldenPath(), std::ios::binary).write(built.data(), static_cast<std::streamsize>(built.size()));
    }
    const std::string golden = readSaveBytes(saveGoldenPath());
    ASSERT_FALSE(golden.empty()) << saveGoldenPath();
    EXPECT_EQ(built, golden) << "buildSaveFile no longer writes format 1 byte for byte";

    savegame::SaveFile parsed;
    savegame::SaveError error;
    ASSERT_TRUE(savegame::parseSaveFile(golden, parsed, error)) << error.message;
    EXPECT_EQ(parsed.format, 1);
    EXPECT_EQ(parsed.generation, 7u);
    EXPECT_EQ(parsed.savedAtMs, 1767225600000);
    EXPECT_EQ(parsed.dataVersion, 3u);
    ASSERT_EQ(parsed.sections.size(), 2u);
    EXPECT_EQ(parsed.sections[0].name, "inventory");
    EXPECT_EQ(parsed.sections[1].name, "main");
    nlohmann::json main;
    ASSERT_TRUE(savegame::toJson(*parsed.sections[1].payload, main, error)) << error.message;
    EXPECT_EQ(main.dump(), R"({"flags":[true,false,null],"hp":0.75,"level":"forest","pos":{"$vec3":[4.0,0.0,2.0]}})");
    savegame::SaveMetadata metadata;
    ASSERT_TRUE(savegame::splitMetadata(parsed.metadata, metadata));
    nlohmann::json meta;
    ASSERT_TRUE(savegame::toJson(metadata.meta, meta, error));
    EXPECT_EQ(meta["title"], "Forest - 2h 13m");

    savegame::SaveFile header;
    ASSERT_TRUE(savegame::parseSaveHeader(std::string_view(golden).substr(0, savegame::MAX_HEADER_BYTES), header, error)) << error.message;
    EXPECT_EQ(header.generation, 7u);
    EXPECT_EQ(header.sections.size(), 2u);
}

TEST(SaveGameFormat, RejectsDamage){
    const std::string golden = savegame::buildSaveFile(saveGoldenFile());
    const bool counting = testsupport::countsAllocations();

    auto rejected = [&](const std::string& bytes, const std::string& what){
        savegame::SaveFile out;
        savegame::SaveError error;
        testsupport::resetLargestAllocation();
        const bool ok = savegame::parseSaveFile(bytes, out, error);
        EXPECT_FALSE(ok) << what;
        EXPECT_FALSE(error.message.empty()) << what;
        if (counting) {
            EXPECT_LE(testsupport::largestAllocation(), 64u * 1024u) << what << ": " << error.message;
        }
        return error;
    };

    for (size_t length = 0; length < golden.size(); length++){
        rejected(golden.substr(0, length), "truncated to " + std::to_string(length));
    }
    for (size_t offset = 0; offset < golden.size(); offset++){
        std::string bytes = golden;
        bytes[offset] = static_cast<char>(bytes[offset] ^ 0x01);
        rejected(bytes, "byte " + std::to_string(offset) + " flipped");
    }

    // a sealed file whose first section claims 4 GiB
    {
        std::string bytes = golden;
        const savegame::SaveFile file = saveGoldenFile();
        const size_t section = savegame::HEADER_FIXED_BYTES + file.metadata.size();
        const size_t lengthAt = section + 2 + file.sections[0].name.size();
        putLE32(bytes, lengthAt, 0xFFFFFFFFu);
        resealSave(bytes);
        const savegame::SaveError error = rejected(bytes, "4 GiB section");
        EXPECT_EQ(error.message, "bad section length 4294967295 at offset " + std::to_string(lengthAt));
    }
    // a sealed file with an 11-byte varint in a payload
    {
        savegame::SaveFile file = saveGoldenFile();
        // a table {x = <integer>} whose integer's varint runs 11 bytes
        std::string payload{'\x05', '\x00', '\x01', '\x04', '\x01', 'x', '\x02'};
        payload += std::string(10, '\x80');
        payload += '\x01';
        file.sections[0].payload = std::make_shared<const std::string>(payload);
        const std::string bytes = savegame::buildSaveFile(file);
        const savegame::SaveError error = rejected(bytes, "11-byte varint");
        EXPECT_NE(error.message.find("varint longer than 10 bytes"), std::string::npos) << error.message;
    }
    // a sealed file whose header claims 65 sections
    {
        std::string bytes = golden;
        putLE32(bytes, 36, 65);
        resealSave(bytes);
        EXPECT_NE(rejected(bytes, "65 sections").message.find("bad section count 65"), std::string::npos);
    }
    // a file above the 64 MiB cap is refused before anything is read
    {
        savegame::SaveFile out;
        savegame::SaveError error;
        const std::string huge(savegame::MAX_FILE_BYTES + 1, '\0');
        EXPECT_FALSE(savegame::parseSaveFile(huge, out, error));
        EXPECT_NE(error.message.find("64 MiB"), std::string::npos) << error.message;
    }
    // a text-mode copy (CRLF) breaks the magic
    {
        std::string bytes = golden;
        bytes.replace(4, 2, "\n");
        EXPECT_NE(rejected(bytes, "text-mode copy").message.find("bad magic"), std::string::npos);
    }
}

TEST(SaveGameFormat, NewerFormatIsReportedNotRead){
    std::string bytes = savegame::buildSaveFile(saveGoldenFile());
    // a newer engine's file: its footer checks
    bytes[8] = 2;
    resealSave(bytes);
    savegame::SaveFile out;
    savegame::SaveError error;
    EXPECT_FALSE(savegame::parseSaveFile(bytes, out, error));
    EXPECT_EQ(error.code, savegame::SaveErrorCode::NewerFormat);
    EXPECT_STREQ(savegame::errorCodeName(error.code), "newer_format");
    EXPECT_FALSE(savegame::parseSaveHeader(bytes, out, error));
    EXPECT_EQ(error.code, savegame::SaveErrorCode::NewerFormat);
}

// The footer is checked before the format field: a format byte damaged in a format-1 file reads
// as damage, so a read recovers from an older generation instead of reporting a newer engine
TEST(SaveGameFormat, DamagedFormatFieldIsCorrupt){
    std::string bytes = savegame::buildSaveFile(saveGoldenFile());
    bytes[8] = 2;
    savegame::SaveFile out;
    savegame::SaveError error;
    EXPECT_FALSE(savegame::parseSaveFile(bytes, out, error));
    EXPECT_EQ(error.code, savegame::SaveErrorCode::Corrupt) << error.message;
    EXPECT_EQ(error.message, "checksum mismatch");
}

}  // namespace SaveGameFormatTest_cpp
