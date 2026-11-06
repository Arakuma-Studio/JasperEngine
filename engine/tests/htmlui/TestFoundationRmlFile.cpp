// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/TestSystem.h"

#include "RmlFile.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    // fixtures/common/rmlfile.txt
    const std::string fixtureContent = "RmlFile fixture: 0123456789abcd\n";

    std::string readAll(RmlFile& files, Rml::FileHandle handle, size_t chunk){
        std::string result;
        char buffer[64];
        size_t count;
        while ((count = files.Read(buffer, chunk, handle)) > 0)
            result.append(buffer, count);
        return result;
    }
}

TEST(RmlFileTest, OpenReadSeekTellLength){
    RmlFile files;
    Rml::FileHandle handle = files.Open("common/rmlfile.txt");
    ASSERT_NE(handle, 0u);

    EXPECT_EQ(files.Length(handle), fixtureContent.size());
    EXPECT_EQ(files.Tell(handle), 0u);

    char buffer[64] = {};
    EXPECT_EQ(files.Read(buffer, 7, handle), 7u);
    EXPECT_EQ(std::string(buffer, 7), "RmlFile");
    EXPECT_EQ(files.Tell(handle), 7u);

    EXPECT_TRUE(files.Seek(handle, 0, SEEK_END));
    EXPECT_EQ(files.Tell(handle), fixtureContent.size());
    EXPECT_EQ(files.Read(buffer, 4, handle), 0u);

    EXPECT_TRUE(files.Seek(handle, -5, SEEK_END));
    EXPECT_EQ(files.Read(buffer, 64, handle), 5u);
    EXPECT_EQ(std::string(buffer, 5), "abcd\n");

    EXPECT_TRUE(files.Seek(handle, 3, SEEK_SET));
    EXPECT_TRUE(files.Seek(handle, 2, SEEK_CUR));
    EXPECT_EQ(files.Tell(handle), 5u);
    EXPECT_TRUE(files.Seek(handle, -5, SEEK_CUR));
    EXPECT_EQ(files.Tell(handle), 0u);

    // small reads put the whole file back together
    EXPECT_EQ(readAll(files, handle, 3), fixtureContent);

    files.Close(handle);
}

TEST(RmlFileTest, MissingFileReturnsZero){
    RmlFile files;
    EXPECT_EQ(files.Open("common/does-not-exist.txt"), 0u);
    EXPECT_EQ(files.Open("asset://common/does-not-exist.txt"), 0u);
    EXPECT_EQ(files.Open(""), 0u);

    Rml::String data = "unchanged";
    EXPECT_FALSE(files.LoadFile("common/does-not-exist.txt", data));
    EXPECT_FALSE(files.LoadFile("", data));
}

TEST(RmlFileTest, LoadFile){
    RmlFile files;
    Rml::String data;
    ASSERT_TRUE(files.LoadFile("common/rmlfile.txt", data));
    EXPECT_EQ(data, fixtureContent);
}

TEST(RmlFileTest, AssetScheme){
    RmlFile files;
    Rml::FileHandle handle = files.Open("asset://common/rmlfile.txt");
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(files.Length(handle), fixtureContent.size());
    EXPECT_EQ(readAll(files, handle, 64), fixtureContent);
    files.Close(handle);

    Rml::String data;
    ASSERT_TRUE(files.LoadFile("asset://common/rmlfile.txt", data));
    EXPECT_EQ(data, fixtureContent);
    // "." and ".." resolve like any engine path
    ASSERT_TRUE(files.LoadFile("asset://common/../common/./rmlfile.txt", data));
    EXPECT_EQ(data, fixtureContent);
}

TEST(RmlFileTest, DataScheme){
    const std::string content = "written by RmlFileTest\n";
    const std::string systemPath = TestSystem::get().getUserDataPath() + "/rmlfile-data.txt";
    {
        std::ofstream out(systemPath, std::ios::binary);
        out << content;
    }

    RmlFile files;
    Rml::String data;
    ASSERT_TRUE(files.LoadFile("data://rmlfile-data.txt", data));
    EXPECT_EQ(data, content);

    std::remove(systemPath.c_str());
    EXPECT_EQ(files.Open("data://rmlfile-data.txt"), 0u);
}

TEST(RmlFileTest, EmptyFile){
    const std::string systemPath = TestSystem::get().getUserDataPath() + "/rmlfile-empty.txt";
    {
        std::ofstream out(systemPath, std::ios::binary);
    }

    RmlFile files;
    Rml::FileHandle handle = files.Open("data://rmlfile-empty.txt");
    ASSERT_NE(handle, 0u);
    char buffer[4];
    EXPECT_EQ(files.Length(handle), 0u);
    EXPECT_EQ(files.Read(buffer, 4, handle), 0u);
    EXPECT_TRUE(files.Seek(handle, 0, SEEK_END));
    EXPECT_FALSE(files.Seek(handle, 1, SEEK_SET));
    files.Close(handle);

    Rml::String data = "unchanged";
    EXPECT_TRUE(files.LoadFile("data://rmlfile-empty.txt", data));
    EXPECT_EQ(data, "");

    std::remove(systemPath.c_str());
}

TEST(RmlFileTest, SeekOutOfRangeFails){
    RmlFile files;
    Rml::FileHandle handle = files.Open("common/rmlfile.txt");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(files.Seek(handle, 4, SEEK_SET));
    EXPECT_FALSE(files.Seek(handle, -1, SEEK_SET));
    EXPECT_FALSE(files.Seek(handle, long(fixtureContent.size()) + 1, SEEK_SET));
    EXPECT_FALSE(files.Seek(handle, 1, SEEK_END));
    EXPECT_FALSE(files.Seek(handle, -long(fixtureContent.size()) - 1, SEEK_END));
    EXPECT_FALSE(files.Seek(handle, -5, SEEK_CUR));
    EXPECT_FALSE(files.Seek(handle, 0, 12345));
    // a failed seek leaves the position where it was
    EXPECT_EQ(files.Tell(handle), 4u);

    files.Close(handle);
}
