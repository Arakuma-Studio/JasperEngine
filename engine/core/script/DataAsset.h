// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DATAASSET_H
#define DATAASSET_H

#include "Export.h"
#include "ScriptProperty.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine {

    // Data Assets (Unity's ScriptableObject, docs/specs/S95-lua-runtime-gaps.md REQ-20 to REQ-24,
    // D-12): a .dataasset is JSON {version: 1, class, values}; its class is a Lua module whose first
    // lines hold `---@dataasset`, whose `properties` table is the schema and whose functions are the
    // methods of every asset of the class. In Lua an asset is a table: its values (references
    // resolved: assets by GUID, other Data Assets as their own tables), the class its __index. One
    // table per asset and run, made on its first use; nothing is ever written back to the file.
    //
    // Which modules are data classes and which assets each has is the index: in the editor's Play the
    // editor provides it (setIndexProvider), an exported game reads je-dataassets.json from its
    // assets (the exporter writes it), on the first query. A game that never uses one pays nothing.
    class JASPERENGINE_API DataAsset {
    public:
        static constexpr const char* EXTENSION = ".dataasset";
        // the assetType of a script property that holds one (assetTypeName(AssetType::DataAsset));
        // a `properties` entry may write it "dataasset"
        static constexpr const char* ASSET_TYPE = "data_asset";
        static constexpr int VERSION = 1;
        // at the exported assets root
        static constexpr const char* INDEX_FILE = "je-dataassets.json";
        // the annotation that makes a Lua module a data class
        static constexpr const char* ANNOTATION = "---@dataasset";

        struct ClassInfo {
            std::string module;               // its Lua file, relative to the Lua directory ("data/ItemData.lua")
            std::vector<std::string> assets;  // its assets, relative to the assets directory, sorted
        };
        struct Index {
            std::map<std::string, ClassInfo> classes;
        };

        // One field of a data class's schema, read from its `properties` entry
        struct Field {
            std::string name;
            ScriptPropertyType type = ScriptPropertyType::Float;
            ScriptPropertyType elementType = ScriptPropertyType::Float;  // a list's
            std::string assetType;   // "texture", ...; ASSET_TYPE for a Data Asset reference
            std::string dataClass;   // the class a Data Asset reference names ("" = any)
        };

        // Editor Play: the index from the editor's project (nullptr: the exported file)
        using IndexProvider = std::function<Index()>;
        static void setIndexProvider(IndexProvider provider);
        // The index this run uses (asked once per run: clear() forgets it)
        static const Index& index();

        static std::string encodeIndex(const Index& index);
        static bool decodeIndex(std::string_view text, Index& index, std::string* error);

        // "items/sword.dataasset", "asset://items/sword.dataasset" or a GUID -> the assets-relative
        // path; false (with the reason) when it names no Data Asset
        static bool resolvePath(const std::string& guidOrPath, std::string& path, std::string* error);
        // The module name require() takes for a class's file ("data/ItemData.lua" -> "data.ItemData")
        static std::string moduleName(const std::string& luaPath);
        // `type` (and `of`) of a data class's field: a built-in type name, or the name of a data class
        // of the index; false with the reason for anything else (entities are refused: assets
        // outlive scenes)
        // a `properties` entry's assetType as stored: "dataasset" is ASSET_TYPE
        static std::string normalizedAssetType(const std::string& assetType);
        static bool fieldType(const std::string& typeName, const std::string& ofName, const std::string& assetType,
                              const std::function<bool(const std::string&)>& isDataClass, Field& field, std::string* error);

        // Lua, main thread. The shared table of an asset pushed (true), or nothing pushed (false and why)
        static bool push(lua_State* L, const std::string& guidOrPath, std::string* error);
        // a new table of the class with its defaults (no file: pathOf gives nil)
        static bool pushNew(lua_State* L, const std::string& className, std::string* error);
        // every asset of the class, an array (sorted by path; an asset that does not load is left out, warned)
        static bool pushAll(lua_State* L, const std::string& className, std::string* error);
        // the class and file of the asset table at index; false for any other value
        static bool classOf(lua_State* L, int index, std::string& className);
        static bool pathOf(lua_State* L, int index, std::string& path);
        // Forgets every table, class and the index: the next use reads the files again (a new Play)
        static void clear(lua_State* L);
    };

}

#endif // DATAASSET_H
