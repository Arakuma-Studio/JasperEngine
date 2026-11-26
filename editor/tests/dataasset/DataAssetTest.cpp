// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Data Assets (docs/specs/S95-lua-runtime-gaps.md REQ-20 to REQ-23, AC-21): data classes and their
// schemas (the property parser's `list` type and data class names), the .dataasset document, and the
// Lua DataAsset module on the scripts' runtime (shared tables, references by GUID, defaults, nothing
// written back).

#include "../ScriptTestHarness.h"

#include "../stream/StreamTestAccess.h"
#include "asset/AssetMap.h"
#include "dataasset/DataAssets.h"
#include "script/DataAsset.h"
#include "serialization/codecs/CustomCodecs.h"
#include "util/ProjectUtils.h"

#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const ITEM_DATA =
        "---@dataasset\n"
        "-- Items of the shop\n"
        "local ItemData = {\n"
        "  properties = {\n"
        "    { name = 'displayName', type = 'string', default = 'Item' },\n"
        "    { name = 'damage', type = 'int', default = 5 },\n"
        "    { name = 'tint', type = 'color3', default = {1, 0.5, 0} },\n"
        "    { name = 'tags', type = 'list', of = 'string', default = {'a', 'b'} },\n"
        "    { name = 'upgrade', type = 'ItemData' },\n"
        "    { name = 'drops', type = 'list', of = 'ItemData' },\n"
        "    { name = 'icon', type = 'asset', assetType = 'texture' },\n"
        "  }\n"
        "}\n"
        "function ItemData:describe() return self.displayName .. ':' .. self.damage end\n"
        "return ItemData\n";

    class DataAssetTest : public scripttest::ScriptTest {
    protected:
        void SetUp() override {
            ScriptTest::SetUp();
            writeScript("ItemData", ITEM_DATA);
            std::filesystem::create_directories(dir / "items");
            DataAsset::setIndexProvider([this]() {
                DataAsset::Index index;
                for (const DataAssets::Class& dataClass : DataAssets::listClasses(dir)) index.classes[dataClass.name].module = dataClass.luaPath;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
                    if (entry.path().extension() != ".dataasset") continue;
                    const std::string className = DataAssets::classOfFile(entry.path());
                    if (index.classes.count(className)) index.classes[className].assets.push_back(entry.path().lexically_relative(dir).generic_string());
                }
                for (auto& [name, info] : index.classes) std::sort(info.assets.begin(), info.assets.end());
                return index;
            });
        }

        void TearDown() override {
            DataAsset::setIndexProvider(nullptr);
            AssetMap::reset();
            ScriptTest::TearDown();
        }

        void writeAsset(const std::string& relative, const std::string& json) {
            std::ofstream(dir / relative) << json;
        }

        std::string readFile(const std::string& relative) {
            std::ifstream in(dir / relative, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        std::vector<ScriptProperty> schemaOf(const std::string& className) {
            DataAssets::Class dataClass;
            EXPECT_TRUE(DataAssets::findClass(dir, className, dataClass));
            std::vector<ScriptProperty> schema;
            std::string error;
            EXPECT_TRUE(DataAssets::readSchema(dataClass, schema, error)) << error;
            return schema;
        }

        static const ScriptProperty* field(const std::vector<ScriptProperty>& schema, const std::string& name) {
            for (const ScriptProperty& property : schema) {
                if (property.name == name) return &property;
            }
            return nullptr;
        }
    };

}

TEST_F(DataAssetTest, ParsesTheDataClass) {
    writeScript("NotAClass", "local M = {}\n---@dataasset\nreturn M\n");
    const std::vector<DataAssets::Class> classes = DataAssets::listClasses(dir);
    ASSERT_EQ(classes.size(), 1u) << "only the annotation among the opening comment lines marks a class";
    EXPECT_EQ(classes[0].name, "ItemData");
    EXPECT_EQ(classes[0].luaPath, "ItemData.lua");

    const std::vector<ScriptProperty> schema = schemaOf("ItemData");
    ASSERT_EQ(schema.size(), 7u);
    ASSERT_NE(field(schema, "damage"), nullptr);
    EXPECT_EQ(field(schema, "damage")->type, ScriptPropertyType::Int);
    const ScriptProperty* upgrade = field(schema, "upgrade");
    ASSERT_NE(upgrade, nullptr);
    EXPECT_EQ(upgrade->type, ScriptPropertyType::Asset) << "a data class name types a Data Asset reference";
    EXPECT_EQ(upgrade->assetType, DataAsset::ASSET_TYPE);
    EXPECT_EQ(upgrade->ptrTypeName, "ItemData");
    const ScriptProperty* drops = field(schema, "drops");
    ASSERT_NE(drops, nullptr);
    EXPECT_EQ(drops->type, ScriptPropertyType::List);
    EXPECT_EQ(drops->elementType, ScriptPropertyType::Asset);
    EXPECT_EQ(drops->ptrTypeName, "ItemData");
    EXPECT_TRUE(drops->isDataAssetReference());
    const ScriptProperty* tags = field(schema, "tags");
    ASSERT_NE(tags, nullptr);
    ASSERT_TRUE(std::holds_alternative<ScriptPropertyList>(tags->defaultValue));
    const ScriptPropertyList& defaults = std::get<ScriptPropertyList>(tags->defaultValue);
    ASSERT_EQ(defaults.items.size(), 2u);
    EXPECT_EQ(std::get<std::string>(defaults.items[1]), "b");
}

TEST_F(DataAssetTest, RefusesGameObjectsAndUnknownTypes) {
    writeScript("Bad", "---@dataasset\nreturn { properties = { { name = 'target', type = 'entity' } } }\n");
    writeScript("Odd", "---@dataasset\nreturn { properties = { { name = 'x', type = 'NoSuchThing' } } }\n");
    DataAssets::Class dataClass;
    std::vector<ScriptProperty> schema;
    std::string error;
    ASSERT_TRUE(DataAssets::findClass(dir, "Bad", dataClass));
    EXPECT_FALSE(DataAssets::readSchema(dataClass, schema, error));
    EXPECT_NE(error.find("cannot reference a GameObject"), std::string::npos) << error;
    ASSERT_TRUE(DataAssets::findClass(dir, "Odd", dataClass));
    EXPECT_FALSE(DataAssets::readSchema(dataClass, schema, error));
    EXPECT_NE(error.find("'NoSuchThing' is neither"), std::string::npos) << error;
}

TEST_F(DataAssetTest, ListPropertiesOfScriptsReachLuaAndRoundTrip) {
    const std::string code =
        "local M = { properties = {\n"
        "  { name = 'waypoints', type = 'list', of = 'vector3', default = {{1, 2, 3}} },\n"
        "  { name = 'item', type = 'ItemData' },\n"
        "} }\n"
        "function M:init() log[#log + 1] = #self.waypoints .. ':' .. self.waypoints[1].y .. ':' .. self.item:describe() end\n"
        "return M\n";
    writeScript("Patrol", code);
    writeAsset("items/sword.dataasset", R"({"version": 1, "class": "ItemData", "values": {"displayName": "Sword", "damage": 12}})");

    ScriptEntry parsed;
    const auto result = ProjectUtils::readLuaScriptProperties(parsed, code, "Patrol.lua");
    ASSERT_EQ(result.status, ProjectUtils::LuaPropertiesResult::Ok) << result.message;
    ASSERT_EQ(parsed.properties.size(), 2u);
    ScriptProperty waypoints = parsed.properties[0].name == "waypoints" ? parsed.properties[0] : parsed.properties[1];
    ScriptProperty item = parsed.properties[0].name == "item" ? parsed.properties[0] : parsed.properties[1];
    EXPECT_EQ(waypoints.type, ScriptPropertyType::List);
    EXPECT_EQ(waypoints.elementType, ScriptPropertyType::Vector3);
    EXPECT_TRUE(item.isDataAssetReference());
    item.value = AssetRef("items/sword.dataasset");

    // the scene file and the cooked file keep the list and its element type
    const ScriptProperty decoded = StreamTestAccess::decodeScriptProperty(StreamTestAccess::encodeScriptProperty(waypoints));
    EXPECT_EQ(decoded.type, ScriptPropertyType::List);
    EXPECT_EQ(decoded.elementType, ScriptPropertyType::Vector3);
    EXPECT_EQ(std::get<ScriptPropertyList>(decoded.value), std::get<ScriptPropertyList>(waypoints.value));
    const ScriptProperty decodedItem = StreamTestAccess::decodeScriptProperty(StreamTestAccess::encodeScriptProperty(item));
    EXPECT_EQ(decodedItem.ptrTypeName, "ItemData");
    EXPECT_EQ(decodedItem.assetType, DataAsset::ASSET_TYPE);
    // cooked: the scripts, then each property's element type; content cooked before 1.12 ends
    // after the scripts and still reads
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.className = "Patrol";
    entry.properties = {waypoints, item};
    const std::vector<ScriptEntry> scripts = {entry};
    auto cookedScripts = [](const std::vector<uint8_t>& bytes, const std::vector<std::string>& names, std::vector<ScriptEntry>& out) {
        jasperengine::cook::Reader reader(std::span<const uint8_t>(bytes.data(), bytes.size()), names,
                                          [](uint32_t id) { return static_cast<Entity>(id); });
        jasperengine::cook::Codec<std::vector<ScriptEntry>>::read(reader, out);
        return !reader.failed() && reader.atEnd();
    };
    jasperengine::cook::StringTable strings;
    jasperengine::cook::Writer writer(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    jasperengine::cook::Codec<std::vector<ScriptEntry>>::write(writer, scripts);
    std::vector<ScriptEntry> cooked;
    ASSERT_TRUE(cookedScripts(writer.data(), strings.strings(), cooked));
    ASSERT_EQ(cooked.size(), 1u);
    EXPECT_EQ(cooked[0].properties[0].elementType, ScriptPropertyType::Vector3);
    EXPECT_EQ(std::get<ScriptPropertyList>(cooked[0].properties[0].value), std::get<ScriptPropertyList>(waypoints.value));
    EXPECT_EQ(cooked[0].properties[1].assetType, DataAsset::ASSET_TYPE);
    ScriptEntry older = entry;
    older.properties = {item};
    jasperengine::cook::Writer olderWriter(strings, [](Entity e) { return static_cast<uint32_t>(e); });
    olderWriter.varint(1);
    jasperengine::cook::Codec<ScriptEntry>::write(olderWriter, older);
    ASSERT_TRUE(cookedScripts(olderWriter.data(), strings.strings(), cooked)) << "1.5 content: no element types after the scripts";
    EXPECT_EQ(std::get<AssetRef>(cooked[0].properties[0].value).path, "items/sword.dataasset");

    Scene* scene = newScene();
    Engine::setScene(scene);
    const Entity guard = entity(scene, "Guard");
    addScript(scene, guard, "Patrol");
    scene->getComponent<ScriptComponent>(guard).scripts[0].properties = {waypoints, item};
    LuaBinding::initializeLuaScripts(scene);
    frame();
    EXPECT_EQ(log(), "1:2.0:Sword:12");
}

TEST_F(DataAssetTest, DocumentKeepsUnknownValues) {
    DataAssets::Document document;
    std::string error;
    ASSERT_TRUE(DataAssets::parse(R"({"version": 1, "class": "ItemData", "values": {"damage": 3, "oldName": "kept"}})", document, &error)) << error;
    const std::vector<ScriptProperty> schema = schemaOf("ItemData");
    EXPECT_EQ(std::get<int>(DataAssets::valueOf(nullptr, *field(schema, "damage"), document)), 3);
    EXPECT_EQ(std::get<std::string>(DataAssets::valueOf(nullptr, *field(schema, "displayName"), document)), "Item") << "a missing value is its default";
    DataAssets::Document reread;
    ASSERT_TRUE(DataAssets::parse(DataAssets::write(document), reread, &error));
    EXPECT_EQ(reread.values.value("oldName", std::string()), "kept");
    EXPECT_FALSE(DataAssets::parse(R"({"version": 2, "class": "ItemData"})", document, &error));
    EXPECT_NE(error.find("newer editor"), std::string::npos);
}

TEST_F(DataAssetTest, LoadsSharedTablesAndReferencesByGuid) {
    const Guid realGuid = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    std::vector<AssetMap::Entry> entries = {{realGuid, AssetType::DataAsset, false, "items/axe.dataasset"}};
    const std::vector<uint8_t> map = AssetMap::encode(entries);
    ASSERT_TRUE(AssetMap::loadFromMemory(map.data(), map.size()));
    writeAsset("items/axe.dataasset", R"({"version": 1, "class": "ItemData", "values": {"displayName": "Axe", "damage": 7,
        "upgrade": {"guid": "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "path": "items/old-name.dataasset"}}})");
    // a cycle: sword drops the axe, the axe upgrades to itself
    writeAsset("items/sword.dataasset", R"({"version": 1, "class": "ItemData", "values": {"displayName": "Sword",
        "drops": [{"guid": "4b1e9c0d2a7f4e31b6c58d0f9e2a7c13", "path": "items/gone.dataasset"}, null]}})");

    EXPECT_EQ(lua("local a = DataAsset.load('items/axe.dataasset'); local b = DataAsset.load('asset://items/axe.dataasset')\n"
                  "return tostring(rawequal(a, b)) .. ' ' .. tostring(rawequal(a, a.upgrade)) .. ' ' .. a:describe()"),
              "true true Axe:7") << "one table per asset and run; a GUID reference finds the moved file";
    EXPECT_EQ(lua("local s = DataAsset.load('4b1e9c0d2a7f4e31b6c58d0f9e2a7c13'); return s.displayName"), "Axe");
    EXPECT_EQ(lua("local s = DataAsset.load('items/sword.dataasset')\n"
                  "return #s.drops .. ' ' .. tostring(rawequal(s.drops[1], DataAsset.load('items/axe.dataasset'))) .. ' ' .. tostring(s.drops[2]) .. ' ' ..\n"
                  "  DataAsset.classOf(s) .. ' ' .. DataAsset.pathOf(s) .. ' ' .. s.tags[2] .. ' ' .. s.tint.y"),
              "2 true false ItemData items/sword.dataasset b 0.5") << "an unset element of a list of references is false";
    EXPECT_EQ(lua("local t, err = DataAsset.load('items/none.dataasset'); return tostring(t) .. ' ' .. err"), "nil cannot open items/none.dataasset");
    EXPECT_EQ(lua("local all = DataAsset.loadAll('ItemData'); return #all .. ' ' .. DataAsset.pathOf(all[1]) .. ' ' .. DataAsset.pathOf(all[2])"),
              "2 items/axe.dataasset items/sword.dataasset");
    EXPECT_EQ(lua("local t, err = DataAsset.loadAll('Nope'); return tostring(t) .. ' ' .. err:sub(1, 20)"), "nil no data class 'Nope'");
}

TEST_F(DataAssetTest, CreateGivesTheDefaults) {
    EXPECT_EQ(lua("local t = DataAsset.create('ItemData')\n"
                  "return t:describe() .. ' ' .. #t.tags .. ' ' .. tostring(t.upgrade) .. ' ' .. #t.drops .. ' ' ..\n"
                  "  DataAsset.classOf(t) .. ' ' .. tostring(DataAsset.pathOf(t)) .. ' ' .. tostring(DataAsset.classOf({}))"),
              "Item:5 2 nil 0 ItemData nil nil");
}

TEST_F(DataAssetTest, NothingIsWrittenBackAfterPlay) {
    const std::string text = R"({"version": 1, "class": "ItemData", "values": {"displayName": "Shield", "damage": 1}})";
    writeAsset("items/shield.dataasset", text);
    EXPECT_EQ(lua("local s = DataAsset.load('items/shield.dataasset'); s.damage = 99; table.insert(s.tags, 'x'); "
                  "return DataAsset.load('items/shield.dataasset').damage"), "99") << "the run shares the changed table";
    // Play stops and starts again: the run's tables go, the file is as it was
    LuaBinding::clearLoadedProjectModules();
    EXPECT_EQ(readFile("items/shield.dataasset"), text);
    EXPECT_EQ(lua("return DataAsset.load('items/shield.dataasset').damage"), "1");
}
