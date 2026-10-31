// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The save-file helpers (feature script-data, spec 6.3 and 9): FileUtils path rules, atomic
// writes, list, and the Lua File.* / Json.readFile / Json.writeFile functions over them.

#include "ScriptDataTestSupport.h"

#include "io/FileUtils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using scriptdata::ScriptDataTest;
namespace fs = std::filesystem;

namespace {

class FileUtilsTest : public ScriptDataTest {};

TEST_F(FileUtilsTest, PathRules) {
    std::string normalized, systemPath, error;
    ASSERT_TRUE(jasperengine::FileUtils::resolve("data://saves//slot1.json", true, normalized, systemPath, error)) << error;
    EXPECT_EQ(normalized, "data://saves/slot1.json");
    EXPECT_EQ(fs::path(systemPath).lexically_normal(), (root / "data" / "saves" / "slot1.json").lexically_normal());
    // a bare relative path is data://; '\' separates like '/'; '.' segments go
    ASSERT_TRUE(jasperengine::FileUtils::resolve("saves\\./slot1.json", true, normalized, systemPath, error)) << error;
    EXPECT_EQ(normalized, "data://saves/slot1.json");
    ASSERT_TRUE(jasperengine::FileUtils::resolve("asset://data/items.json", false, normalized, systemPath, error));
    EXPECT_EQ(fs::path(systemPath).lexically_normal(), (root / "assets" / "data" / "items.json").lexically_normal());
    ASSERT_TRUE(jasperengine::FileUtils::resolve("lua://data/items.json", false, normalized, systemPath, error));
    EXPECT_EQ(fs::path(systemPath).lexically_normal(), (root / "lua" / "data" / "items.json").lexically_normal());

    EXPECT_FALSE(jasperengine::FileUtils::resolve("data://../escape.json", true, normalized, systemPath, error));
    EXPECT_EQ(error, "'..' is not allowed in paths: data://../escape.json");
    EXPECT_FALSE(jasperengine::FileUtils::resolve("saves/../../x", false, normalized, systemPath, error));
    EXPECT_FALSE(jasperengine::FileUtils::resolve("asset://a.json", true, normalized, systemPath, error));
    EXPECT_EQ(error, "read-only path: asset://a.json");
    EXPECT_FALSE(jasperengine::FileUtils::resolve("lua://a.lua", true, normalized, systemPath, error));
    EXPECT_EQ(error, "read-only path: lua://a.lua");
    EXPECT_FALSE(jasperengine::FileUtils::resolve("http://x/a.json", false, normalized, systemPath, error));
    EXPECT_NE(error.find("unknown path scheme"), std::string::npos);
    EXPECT_FALSE(jasperengine::FileUtils::resolve("data://C:/x", true, normalized, systemPath, error));
    EXPECT_NE(error.find("':' is not allowed"), std::string::npos);
    EXPECT_FALSE(jasperengine::FileUtils::resolve("", false, normalized, systemPath, error));
    // absolute paths: reads on desktop and in the editor, never writes
    const std::string absolute = (root / "data" / "a.txt").string();
    EXPECT_FALSE(jasperengine::FileUtils::resolve(absolute, true, normalized, systemPath, error));
    EXPECT_NE(error.find("absolute paths are not allowed for writes"), std::string::npos);
    ASSERT_TRUE(jasperengine::FileUtils::resolve(absolute, false, normalized, systemPath, error)) << error;
    EXPECT_EQ(systemPath, absolute);
    EXPECT_FALSE(jasperengine::FileUtils::resolve((root / ".." / "x").string(), false, normalized, systemPath, error));
}

TEST_F(FileUtilsTest, AtomicWritesAppendsAndRemovals) {
    std::string error, text;
    ASSERT_TRUE(jasperengine::FileUtils::writeText("data://saves/deep/slot.txt", "first", error)) << error;
    EXPECT_EQ(scriptdata::readFile(root / "data" / "saves" / "deep" / "slot.txt"), "first") << "missing folders made";
    ASSERT_TRUE(jasperengine::FileUtils::writeText("saves/deep/slot.txt", std::string("second\0bytes", 12), error));
    EXPECT_EQ(scriptdata::readFile(root / "data" / "saves" / "deep" / "slot.txt"), std::string("second\0bytes", 12));
    EXPECT_FALSE(fs::exists(root / "data" / "saves" / "deep" / "slot.txt.tmp")) << "no temporary file left behind";
    ASSERT_TRUE(jasperengine::FileUtils::readText("data://saves/deep/slot.txt", text, error));
    EXPECT_EQ(text, std::string("second\0bytes", 12));
    ASSERT_TRUE(jasperengine::FileUtils::writeText("data://empty.txt", "", error));
    ASSERT_TRUE(jasperengine::FileUtils::readText("data://empty.txt", text, error));
    EXPECT_EQ(text, "");

    ASSERT_TRUE(jasperengine::FileUtils::appendText("data://logs/run.log", "a\n", error)) << error;
    ASSERT_TRUE(jasperengine::FileUtils::appendText("data://logs/run.log", "b\n", error));
    EXPECT_EQ(scriptdata::readFile(root / "data" / "logs" / "run.log"), "a\nb\n");

    EXPECT_FALSE(jasperengine::FileUtils::readText("data://nothing.json", text, error));
    EXPECT_EQ(error, "not found: data://nothing.json");
    EXPECT_FALSE(jasperengine::FileUtils::readText("data://saves", text, error));
    EXPECT_EQ(error, "is a folder: data://saves");
    EXPECT_FALSE(jasperengine::FileUtils::writeText("data://saves", "x", error));
    EXPECT_EQ(error, "is a folder: data://saves");
    EXPECT_FALSE(jasperengine::FileUtils::writeText("data://", "x", error));

    EXPECT_TRUE(jasperengine::FileUtils::exists("data://saves"));
    EXPECT_TRUE(jasperengine::FileUtils::exists("saves/deep/slot.txt"));
    EXPECT_FALSE(jasperengine::FileUtils::exists("data://nope"));
    EXPECT_FALSE(jasperengine::FileUtils::exists("data://../x"));
    ASSERT_TRUE(jasperengine::FileUtils::remove("data://saves/deep/slot.txt", error)) << error;
    EXPECT_FALSE(fs::exists(root / "data" / "saves" / "deep" / "slot.txt"));
    EXPECT_TRUE(jasperengine::FileUtils::remove("data://saves/deep/slot.txt", error)) << "nothing there: fine";
    EXPECT_FALSE(jasperengine::FileUtils::remove("data://saves", error));
    EXPECT_NE(error.find("is a folder"), std::string::npos);
    EXPECT_FALSE(jasperengine::FileUtils::remove("asset://x", error));

    ASSERT_TRUE(jasperengine::FileUtils::makeDirs("data://a/b/c", error)) << error;
    EXPECT_TRUE(fs::is_directory(root / "data" / "a" / "b" / "c"));
    EXPECT_TRUE(jasperengine::FileUtils::makeDirs("data://a/b", error)) << "already there";
    EXPECT_FALSE(jasperengine::FileUtils::makeDirs("data://empty.txt", error));
    EXPECT_EQ(error, "a file is there: data://empty.txt");

    // assets are read, never written
    scriptdata::writeFile(root / "assets" / "data" / "items.json", "[1]");
    ASSERT_TRUE(jasperengine::FileUtils::readText("asset://data/items.json", text, error)) << error;
    EXPECT_EQ(text, "[1]");
    EXPECT_TRUE(jasperengine::FileUtils::exists("asset://data/items.json"));
    EXPECT_FALSE(jasperengine::FileUtils::writeText("asset://data/items.json", "x", error));
    EXPECT_EQ(scriptdata::readFile(root / "assets" / "data" / "items.json"), "[1]");
}

TEST_F(FileUtilsTest, ListsDataFolders) {
    std::string error;
    std::vector<std::string> names;
    ASSERT_TRUE(jasperengine::FileUtils::writeText("data://slots/b.json", "{}", error));
    ASSERT_TRUE(jasperengine::FileUtils::writeText("data://slots/a.json", "{}", error));
    ASSERT_TRUE(jasperengine::FileUtils::makeDirs("data://slots/sub", error));
    ASSERT_TRUE(jasperengine::FileUtils::list("data://slots", names, error)) << error;
    EXPECT_EQ(names, (std::vector<std::string>{"a.json", "b.json", "sub"}));
    ASSERT_TRUE(jasperengine::FileUtils::list("data://", names, error)) << error;
    EXPECT_NE(std::find(names.begin(), names.end(), "slots"), names.end());
    EXPECT_FALSE(jasperengine::FileUtils::list("data://missing", names, error));
    EXPECT_EQ(error, "not found: data://missing");
    EXPECT_FALSE(jasperengine::FileUtils::list("data://slots/a.json", names, error));
    EXPECT_EQ(error, "not a folder: data://slots/a.json");
    EXPECT_FALSE(jasperengine::FileUtils::list("asset://", names, error));
    EXPECT_NE(error.find("only data:// folders can be listed"), std::string::npos);
}

TEST_F(FileUtilsTest, LuaFileFunctions) {
    EXPECT_EQ(eval("return tostring(File.writeText('data://save.txt', 'hello'))"), "true");
    EXPECT_EQ(eval("return File.readText('data://save.txt')"), "hello");
    EXPECT_EQ(eval("return File.readText('save.txt')"), "hello");
    EXPECT_EQ(eval("local ok, e = File.readText('data://missing.txt') return tostring(ok) .. ' ' .. e"), "nil not found: data://missing.txt");
    EXPECT_EQ(eval("File.appendText('log.txt', 'a') File.appendText('log.txt', 'b') return File.readText('log.txt')"), "ab");
    EXPECT_EQ(eval("return tostring(File.exists('save.txt')) .. tostring(File.exists('nope.txt'))"), "truefalse");
    EXPECT_EQ(eval("return tostring(File.makeDirs('data://slots/old'))"), "true");
    EXPECT_EQ(eval("File.writeText('slots/2.json', '{}') File.writeText('slots/1.json', '{}') return table.concat(File.list('data://slots'), ',')"),
              "1.json,2.json,old");
    EXPECT_EQ(eval("return tostring(File.remove('save.txt')) .. tostring(File.exists('save.txt'))"), "truefalse");
    EXPECT_EQ(eval("local ok, e = File.writeText('asset://x.txt', 'x') return e"), "read-only path: asset://x.txt");
    EXPECT_EQ(eval("local ok, e = File.writeText('../x.txt', 'x') return e"), "'..' is not allowed in paths: ../x.txt");
    // wrong argument types are programming errors: they raise
    EXPECT_EQ(eval("File.writeText('a.txt', 5)"), "ERROR: File.writeText: text must be a string, got number");
    EXPECT_EQ(eval("File.readText()"), "ERROR: File.readText: path must be a string, got no value");
    // the old File object API still works next to them
    EXPECT_EQ(eval("local f = File('data://obj.txt', true) f:writeString('obj') f:close() return File.readText('obj.txt')"), "obj");
}

TEST_F(FileUtilsTest, JsonReadFileAndWriteFile) {
    EXPECT_EQ(eval("return tostring(Json.writeFile('data://garden.json', {coins = 120, plots = {{plant = 'rose'}, Json.null}}))"), "true");
    EXPECT_EQ(scriptdata::readFile(root / "data" / "garden.json"), "{\"coins\":120,\"plots\":[{\"plant\":\"rose\"},null]}");
    EXPECT_EQ(eval("local s = Json.readFile('data://garden.json') return s.coins .. s.plots[1].plant .. tostring(s.plots[2])"),
              "120roseJson.null");
    EXPECT_EQ(eval("Json.writeFile('pretty.json', {a = 1}, {pretty = true}) return File.readText('pretty.json')"), "{\n  \"a\": 1\n}");
    EXPECT_EQ(eval("local v, e = Json.readFile('data://none.json') return tostring(v) .. ' ' .. e"), "nil not found: data://none.json");
    scriptdata::writeFile(root / "data" / "bad.json", "{\n  \"a\": }");
    EXPECT_EQ(eval("local v, e = Json.readFile('data://bad.json') return e"),
              "data://bad.json: syntax error at line 2, column 8: unexpected '}'; expected '[', '{', or a literal");
    EXPECT_EQ(eval("local ok, e = Json.writeFile('data://f.json', {f = print}) return tostring(ok) .. ' ' .. e"),
              "nil cannot encode a function at $.f");
    EXPECT_FALSE(fs::exists(root / "data" / "f.json")) << "nothing written when the value does not encode";
    EXPECT_EQ(eval("local ok, e = Json.writeFile('asset://f.json', {}) return e"), "read-only path: asset://f.json");
    EXPECT_NE(eval("Json.writeFile('data://x.json')").find("Json.writeFile: a value to write expected"), std::string::npos);
    EXPECT_NE(eval("Json.readFile('data://garden.json', {bad = 1})").find("unknown option 'bad'"), std::string::npos);
    // a data asset: a JSON file next to the Lua scripts
    scriptdata::writeFile(root / "lua" / "data" / "items.json", "{\"sword\": {\"damage\": 12}}");
    EXPECT_EQ(eval("return Json.readFile('lua://data/items.json').sword.damage"), "12");
    scriptdata::writeFile(root / "assets" / "levels" / "1.json", "[3, 1, 4]");
    EXPECT_EQ(eval("return #Json.readFile('asset://levels/1.json')"), "3");
    // comments in a hand-written data file
    scriptdata::writeFile(root / "assets" / "levels" / "2.json", "// level 2\n[1]");
    EXPECT_EQ(eval("return Json.readFile('asset://levels/2.json', {allowComments = true})[1]"), "1");
}

}
