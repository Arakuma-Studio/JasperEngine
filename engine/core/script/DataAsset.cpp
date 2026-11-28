// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DataAsset.h"

#include "Log.h"
#include "LuaBinding.h"
#include "asset/AssetMap.h"
#include "io/Data.h"
#include "manager/PrefabAsset.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>

using namespace jasperengine;

namespace {

    using Json = nlohmann::json;

    // registry key of the run's tables: {assets = {path -> table}, classes = {name -> module}}
    const char* const DATA_ASSET_STATE = "jasperengine.dataassets";

    struct IndexState {
        DataAsset::IndexProvider provider;
        DataAsset::Index index;
        bool loaded = false;
    };

    IndexState& dataAssetIndexState() {
        static IndexState state;
        return state;
    }

    bool dataAssetFailWith(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    std::string dataAssetLowered(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // the state table, made on first use, pushed; its absolute index
    int pushDataAssetState(lua_State* L) {
        if (lua_getfield(L, LUA_REGISTRYINDEX, DATA_ASSET_STATE) != LUA_TTABLE) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_newtable(L);
            lua_setfield(L, -2, "assets");
            lua_newtable(L);
            lua_setfield(L, -2, "classes");
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, DATA_ASSET_STATE);
        }
        return lua_gettop(L);
    }

    // the class's module table pushed (require'd once per run), or nothing and false
    bool pushClassModule(lua_State* L, const std::string& className, std::string* error) {
        const int state = pushDataAssetState(L);
        lua_getfield(L, state, "classes");
        if (lua_getfield(L, -1, className.c_str()) == LUA_TTABLE) {
            lua_replace(L, state);
            lua_settop(L, state);
            return true;
        }
        lua_pop(L, 1);
        const auto found = DataAsset::index().classes.find(className);
        if (found == DataAsset::index().classes.end()) {
            lua_settop(L, state - 1);
            return dataAssetFailWith(error, "no data class '" + className + "' (a Lua module with ---@dataasset on its first lines)");
        }
        lua_getglobal(L, "require");
        lua_pushstring(L, DataAsset::moduleName(found->second.module).c_str());
        if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK) {
            const std::string message = LuaBinding::getLuaStackErrorString(L, -1);
            lua_settop(L, state - 1);
            return dataAssetFailWith(error, "data class '" + className + "' (" + found->second.module + ") failed to load: " + message);
        }
        if (!lua_istable(L, -1)) {
            lua_settop(L, state - 1);
            return dataAssetFailWith(error, "data class '" + className + "' (" + found->second.module + ") did not return a table");
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, className.c_str());
        lua_replace(L, state);
        lua_settop(L, state);
        return true;
    }

    std::string dataClassStringField(lua_State* L, int table, const char* key) {
        lua_getfield(L, table, key);
        std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : std::string();
        lua_pop(L, 1);
        return value;
    }

    // the schema of the class table at `classIndex`; each entry's `default` stays in its table,
    // read again for each new table (defaults[i] is the entry's index in `properties`)
    std::vector<DataAsset::Field> readFields(lua_State* L, int classIndex, const std::string& className, std::vector<int>& entries) {
        std::vector<DataAsset::Field> fields;
        lua_getfield(L, classIndex, "properties");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return fields;
        }
        const int properties = lua_gettop(L);
        const auto isDataClass = [](const std::string& name) { return DataAsset::index().classes.count(name) > 0; };
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, properties));
        for (lua_Integer i = 1; i <= count; i++) {
            if (lua_rawgeti(L, properties, i) != LUA_TTABLE) {
                lua_pop(L, 1);
                continue;
            }
            const int entry = lua_gettop(L);
            DataAsset::Field field;
            field.name = dataClassStringField(L, entry, "name");
            std::string type = dataClassStringField(L, entry, "type");
            if (type.empty()) type = "float";
            std::string error;
            if (field.name.empty() ||
                !DataAsset::fieldType(type, dataClassStringField(L, entry, "of"), dataClassStringField(L, entry, "assetType"), isDataClass, field, &error)) {
                Log::warn("Data class '%s': property '%s' is left out: %s", className.c_str(), field.name.c_str(),
                          error.empty() ? "it has no name" : error.c_str());
            } else {
                fields.push_back(std::move(field));
                entries.push_back(static_cast<int>(i));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return fields;
    }

    template<typename V>
    void pushDataValueOrNil(lua_State* L, const V& value) {
        if (!luabridge::push<V>(L, value)) lua_pushnil(L);
    }

    bool isDataVectorType(ScriptPropertyType type, int& size) {
        switch (type) {
            case ScriptPropertyType::Vector2: size = 2; return true;
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: size = 3; return true;
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: size = 4; return true;
            default: return false;
        }
    }

    void pushDataVector(lua_State* L, int size, const float* values) {
        if (size == 2) pushDataValueOrNil(L, Vector2(values[0], values[1]));
        else if (size == 3) pushDataValueOrNil(L, Vector3(values[0], values[1], values[2]));
        else pushDataValueOrNil(L, Vector4(values[0], values[1], values[2], values[3]));
    }

    // nothing to push: nil for a field, false in a list (a Lua array keeps its length)
    void pushDataNone(lua_State* L, bool element) {
        if (element) lua_pushboolean(L, 0);
        else lua_pushnil(L);
    }

    // an asset reference ({guid, path}, a GUID or a path) pushed as an AssetRef or a Data Asset's table
    bool pushReference(lua_State* L, const DataAsset::Field& field, const std::string& guid, const std::string& path, bool element, std::string& why) {
        if (guid.empty() && path.empty()) {
            pushDataNone(L, element);
            return true;
        }
        if (field.assetType == DataAsset::ASSET_TYPE) {
            std::string error;
            if (!DataAsset::push(L, guid.empty() ? path : guid, &error)) {
                // a guid whose asset is gone still has its path
                if (guid.empty() || path.empty() || !DataAsset::push(L, path, &error)) {
                    why = error;
                    pushDataNone(L, element);
                    return false;
                }
            }
            if (!field.dataClass.empty()) {
                std::string className;
                DataAsset::classOf(L, -1, className);
                if (className != field.dataClass) {
                    why = "it is a " + className + ", not a " + field.dataClass;
                    lua_pop(L, 1);
                    pushDataNone(L, element);
                    return false;
                }
            }
            return true;
        }
        AssetRef ref(path, Guid::isValidText(guid) ? Guid::parse(guid) : Guid());
        if (!luabridge::push<ScriptAssetRef>(L, ScriptAssetRef(ref))) pushDataNone(L, element);
        return true;
    }

    // one value of the file, as `type` of the field, pushed; false (the type's none pushed) when it does not fit
    bool pushJsonValue(lua_State* L, const DataAsset::Field& field, ScriptPropertyType type, const Json& value, bool element, std::string& why) {
        int size = 0;
        if (isDataVectorType(type, size)) {
            float values[4] = {0, 0, 0, 1};
            if (!value.is_array() || value.size() < 2 || value.size() > 4) {
                why = "expected an array of " + std::to_string(size) + " numbers";
                pushDataVector(L, size, values);
                return false;
            }
            for (size_t i = 0; i < value.size() && i < 4; i++) {
                if (!value[i].is_number()) {
                    why = "expected numbers";
                    pushDataVector(L, size, values);
                    return false;
                }
                values[i] = value[i].get<float>();
            }
            pushDataVector(L, size, values);
            return true;
        }
        switch (type) {
            case ScriptPropertyType::Bool:
                lua_pushboolean(L, value.is_boolean() && value.get<bool>());
                if (!value.is_boolean()) why = "expected true or false";
                return value.is_boolean();
            case ScriptPropertyType::Int:
                if (value.is_number()) {
                    lua_pushinteger(L, value.is_number_integer() ? value.get<lua_Integer>() : static_cast<lua_Integer>(value.get<double>()));
                    return true;
                }
                lua_pushinteger(L, 0);
                why = "expected an integer";
                return false;
            case ScriptPropertyType::Float:
                lua_pushnumber(L, value.is_number() ? value.get<double>() : 0.0);
                if (!value.is_number()) why = "expected a number";
                return value.is_number();
            case ScriptPropertyType::String:
                lua_pushstring(L, value.is_string() ? value.get<std::string>().c_str() : "");
                if (!value.is_string()) why = "expected a string";
                return value.is_string();
            case ScriptPropertyType::Prefab: {
                if (value.is_null()) {
                    pushDataNone(L, element);
                    return true;
                }
                const std::string guid = value.is_object() ? value.value("guid", std::string()) : std::string();
                const std::string path = value.is_object() ? value.value("path", std::string()) : value.is_string() ? value.get<std::string>() : std::string();
                if (guid.empty() && path.empty()) {
                    pushDataNone(L, element);
                    return value.is_object();
                }
                if (!luabridge::push<PrefabAsset>(L, PrefabAsset(guid, path))) pushDataNone(L, element);
                return true;
            }
            case ScriptPropertyType::Asset: {
                if (value.is_null()) {
                    pushDataNone(L, element);
                    return true;
                }
                if (value.is_string()) {
                    const std::string text = value.get<std::string>();
                    const bool guid = text.size() == 32 && Guid::isValidText(text);
                    return pushReference(L, field, guid ? text : std::string(), guid ? std::string() : text, element, why);
                }
                if (!value.is_object()) {
                    why = "expected {guid, path}";
                    pushDataNone(L, element);
                    return false;
                }
                return pushReference(L, field, value.value("guid", std::string()), value.value("path", std::string()), element, why);
            }
            case ScriptPropertyType::List: {
                lua_newtable(L);
                if (value.is_null()) return true;
                if (!value.is_array()) {
                    why = "expected an array";
                    return false;
                }
                bool ok = true;
                lua_Integer n = 1;
                for (const Json& item : value) {
                    std::string itemWhy;
                    if (!pushJsonValue(L, field, field.elementType, item, true, itemWhy) && ok) {
                        why = "[" + std::to_string(n) + "]: " + itemWhy;
                        ok = false;
                    }
                    lua_rawseti(L, -2, n++);
                }
                return ok;
            }
            default:
                break;
        }
        lua_pushnil(L);
        return true;
    }

    // the entry's `default` (at `index`, 0 = none) as `type` pushed; the editor's parser reads the same forms
    void pushDefault(lua_State* L, const DataAsset::Field& field, ScriptPropertyType type, int index, bool element) {
        const bool has = index != 0 && !lua_isnoneornil(L, index);
        int size = 0;
        if (isDataVectorType(type, size)) {
            float values[4] = {0, 0, 0, 1};
            if (has && lua_istable(L, index)) {
                for (int i = 0; i < size; i++) {
                    lua_rawgeti(L, index, i + 1);
                    if (lua_isnumber(L, -1)) values[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
            }
            pushDataVector(L, size, values);
            return;
        }
        switch (type) {
            case ScriptPropertyType::Bool: lua_pushboolean(L, has && lua_toboolean(L, index)); return;
            case ScriptPropertyType::Int: lua_pushinteger(L, has && lua_isnumber(L, index) ? static_cast<lua_Integer>(lua_tonumber(L, index)) : 0); return;
            case ScriptPropertyType::Float: lua_pushnumber(L, has && lua_isnumber(L, index) ? lua_tonumber(L, index) : 0.0); return;
            case ScriptPropertyType::String: lua_pushstring(L, has && lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : ""); return;
            case ScriptPropertyType::Asset: {
                std::string why;
                const std::string path = has && lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : std::string();
                if (!pushReference(L, field, std::string(), path, element, why) && !why.empty()) {
                    Log::warn("Data class default '%s': %s", path.c_str(), why.c_str());
                }
                return;
            }
            case ScriptPropertyType::List: {
                lua_newtable(L);
                if (!has || !lua_istable(L, index)) return;
                const int list = lua_gettop(L);
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, index));
                for (lua_Integer i = 1; i <= count; i++) {
                    lua_rawgeti(L, index, i);
                    const int item = lua_gettop(L);
                    pushDefault(L, field, field.elementType, item, true);
                    lua_rawseti(L, list, i);
                    lua_pop(L, 1);
                }
                return;
            }
            default:
                pushDataNone(L, element);
                return;
        }
    }

    int dataAssetToString(lua_State* L) {
        std::string className, path;
        DataAsset::classOf(L, 1, className);
        DataAsset::pathOf(L, 1, path);
        lua_pushstring(L, ("DataAsset(" + className + (path.empty() ? "" : ": " + path) + ")").c_str());
        return 1;
    }

    // a new table of the class at `classIndex`, its metatable set, pushed; `values` fills it (null: the defaults)
    void pushAssetTable(lua_State* L, int classIndex, const std::string& className, const std::string& path) {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushvalue(L, classIndex);
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, className.c_str());
        lua_setfield(L, -2, "__dataclass");
        if (!path.empty()) {
            lua_pushstring(L, path.c_str());
            lua_setfield(L, -2, "__datapath");
        }
        lua_pushcfunction(L, dataAssetToString);
        lua_setfield(L, -2, "__tostring");
        lua_setmetatable(L, -2);
    }

    // the fields of the table at `tableIndex` from `values` (an object, or null), the class at `classIndex`
    void fillAsset(lua_State* L, int tableIndex, int classIndex, const std::string& className, const std::string& path, const Json& values) {
        std::vector<int> entries;
        const std::vector<DataAsset::Field> fields = readFields(L, classIndex, className, entries);
        lua_getfield(L, classIndex, "properties");
        const int properties = lua_gettop(L);
        for (size_t i = 0; i < fields.size(); i++) {
            const DataAsset::Field& field = fields[i];
            const auto found = values.is_object() ? values.find(field.name) : values.end();
            std::string why;
            if (values.is_object() && found != values.end()) {
                if (!pushJsonValue(L, field, field.type, *found, false, why)) {
                    Log::warn("Data Asset '%s': %s: %s", path.c_str(), field.name.c_str(), why.c_str());
                }
            } else {
                lua_rawgeti(L, properties, entries[i]);
                lua_getfield(L, -1, "default");
                const int defaultIndex = lua_gettop(L);
                pushDefault(L, field, field.type, defaultIndex, false);
                lua_replace(L, defaultIndex - 1);
                lua_pop(L, 1);
            }
            lua_setfield(L, tableIndex, field.name.c_str());
        }
        lua_pop(L, 1);
    }

    std::string dataAssetPathOf(std::string path) {
        if (path.rfind("asset://", 0) == 0) path = path.substr(8);
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.rfind("./", 0) == 0) path = path.substr(2);
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        return path;
    }

}

void DataAsset::setIndexProvider(IndexProvider provider) {
    IndexState& state = dataAssetIndexState();
    state.provider = std::move(provider);
    state.loaded = false;
}

const DataAsset::Index& DataAsset::index() {
    IndexState& state = dataAssetIndexState();
    if (state.loaded) return state.index;
    state.loaded = true;
    state.index = Index();
    if (state.provider) {
        state.index = state.provider();
        return state.index;
    }
    Data data;
    if (data.open(INDEX_FILE) != FileErrors::FILEDATA_OK) return state.index;
    std::string error;
    if (!decodeIndex(data.readString(), state.index, &error)) {
        Log::warn("Data Assets: %s does not read (%s): no data class is known", INDEX_FILE, error.c_str());
        state.index = Index();
    }
    return state.index;
}

std::string DataAsset::encodeIndex(const Index& index) {
    Json classes = Json::object();
    for (const auto& [name, info] : index.classes) {
        classes[name] = {{"module", info.module}, {"assets", info.assets}};
    }
    return Json{{"version", VERSION}, {"classes", classes}}.dump(2) + "\n";
}

bool DataAsset::decodeIndex(std::string_view text, Index& index, std::string* error) {
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return dataAssetFailWith(error, "not JSON");
    if (!json.contains("version") || !json["version"].is_number_integer() || json["version"].get<int>() > VERSION) {
        return dataAssetFailWith(error, "a version this game does not read");
    }
    index = Index();
    const Json classes = json.value("classes", Json::object());
    if (!classes.is_object()) return dataAssetFailWith(error, "classes is not an object");
    for (auto it = classes.begin(); it != classes.end(); ++it) {
        if (!it.value().is_object()) continue;
        ClassInfo info;
        info.module = it.value().value("module", std::string());
        const Json assets = it.value().value("assets", Json::array());
        for (const Json& asset : assets) {
            if (asset.is_string()) info.assets.push_back(asset.get<std::string>());
        }
        index.classes[it.key()] = std::move(info);
    }
    return true;
}

bool DataAsset::resolvePath(const std::string& guidOrPath, std::string& path, std::string* error) {
    if (guidOrPath.size() == 32 && Guid::isValidText(guidOrPath)) {
        AssetType type = AssetType::Unknown;
        if (!AssetMap::findPath(Guid::parse(guidOrPath), path, &type)) return dataAssetFailWith(error, "no asset has the GUID " + guidOrPath);
        if (type != AssetType::DataAsset && type != AssetType::Unknown) return dataAssetFailWith(error, guidOrPath + " is no Data Asset (" + path + ")");
    } else {
        path = guidOrPath;
    }
    path = dataAssetPathOf(path);
    const std::string extension = EXTENSION;
    if (path.size() <= extension.size() || dataAssetLowered(path.substr(path.size() - extension.size())) != extension) {
        return dataAssetFailWith(error, "'" + guidOrPath + "' is no .dataasset");
    }
    return true;
}

std::string DataAsset::moduleName(const std::string& luaPath) {
    std::string name = dataAssetPathOf(luaPath);
    if (name.size() > 4 && dataAssetLowered(name.substr(name.size() - 4)) == ".lua") name.resize(name.size() - 4);
    std::replace(name.begin(), name.end(), '/', '.');
    return name;
}

std::string DataAsset::normalizedAssetType(const std::string& assetType) {
    const std::string lower = dataAssetLowered(assetType);
    return lower == "dataasset" || lower == ASSET_TYPE ? std::string(ASSET_TYPE) : assetType;
}

bool DataAsset::fieldType(const std::string& typeName, const std::string& ofName, const std::string& assetType,
                          const std::function<bool(const std::string&)>& isDataClass, Field& field, std::string* error) {
    ScriptPropertyType type = ScriptPropertyType::Float;
    if (!scriptPropertyTypeFromName(typeName, type)) {
        if (!isDataClass || !isDataClass(typeName)) {
            return dataAssetFailWith(error, "type '" + typeName + "' is neither a property type nor a data class");
        }
        field.type = ScriptPropertyType::Asset;
        field.assetType = ASSET_TYPE;
        field.dataClass = typeName;
        return true;
    }
    if (type == ScriptPropertyType::EntityReference) {
        return dataAssetFailWith(error, "a data class cannot reference a GameObject (an asset outlives the scenes): use a prefab or a Data Asset");
    }
    field.type = type;
    if (type == ScriptPropertyType::Asset) field.assetType = normalizedAssetType(assetType);
    if (type != ScriptPropertyType::List) return true;
    if (ofName.empty()) return dataAssetFailWith(error, "a list needs `of` (the type of its elements)");
    ScriptPropertyType element = ScriptPropertyType::Float;
    if (!scriptPropertyTypeFromName(ofName, element)) {
        if (!isDataClass || !isDataClass(ofName)) {
            return dataAssetFailWith(error, "of '" + ofName + "' is neither a property type nor a data class");
        }
        field.elementType = ScriptPropertyType::Asset;
        field.assetType = ASSET_TYPE;
        field.dataClass = ofName;
        return true;
    }
    if (element == ScriptPropertyType::List) return dataAssetFailWith(error, "a list cannot hold lists");
    if (element == ScriptPropertyType::EntityReference) {
        return dataAssetFailWith(error, "a data class cannot reference a GameObject (an asset outlives the scenes): use a prefab or a Data Asset");
    }
    field.elementType = element;
    if (element == ScriptPropertyType::Asset) field.assetType = normalizedAssetType(assetType);
    return true;
}

bool DataAsset::push(lua_State* L, const std::string& guidOrPath, std::string* error) {
    std::string path;
    if (!resolvePath(guidOrPath, path, error)) return false;
    const int top = lua_gettop(L);
    const int state = pushDataAssetState(L);
    lua_getfield(L, state, "assets");
    const int assets = lua_gettop(L);
    if (lua_getfield(L, assets, path.c_str()) == LUA_TTABLE) {
        lua_replace(L, top + 1);
        lua_settop(L, top + 1);
        return true;
    }
    lua_pop(L, 1);

    Data data;
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK) {
        lua_settop(L, top);
        return dataAssetFailWith(error, "cannot open " + path);
    }
    const Json json = Json::parse(data.readString(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        lua_settop(L, top);
        return dataAssetFailWith(error, path + " is not JSON");
    }
    if (!json.contains("version") || !json["version"].is_number_integer()) {
        lua_settop(L, top);
        return dataAssetFailWith(error, path + " has no version");
    }
    if (json["version"].get<int>() > VERSION) {
        lua_settop(L, top);
        return dataAssetFailWith(error, path + " was made by a newer editor (version " + std::to_string(json["version"].get<int>()) + ")");
    }
    const std::string className = json.value("class", std::string());
    if (className.empty()) {
        lua_settop(L, top);
        return dataAssetFailWith(error, path + " names no class");
    }
    if (!pushClassModule(L, className, error)) {
        lua_settop(L, top);
        return false;
    }
    const int classIndex = lua_gettop(L);
    pushAssetTable(L, classIndex, className, path);
    const int table = lua_gettop(L);
    // known before its values: assets that refer to each other share their tables
    lua_pushvalue(L, table);
    lua_setfield(L, assets, path.c_str());
    fillAsset(L, table, classIndex, className, path, json.contains("values") ? json["values"] : Json(nullptr));
    lua_replace(L, top + 1);
    lua_settop(L, top + 1);
    return true;
}

bool DataAsset::pushNew(lua_State* L, const std::string& className, std::string* error) {
    const int top = lua_gettop(L);
    if (!pushClassModule(L, className, error)) return false;
    const int classIndex = lua_gettop(L);
    pushAssetTable(L, classIndex, className, std::string());
    fillAsset(L, lua_gettop(L), classIndex, className, std::string(), Json(nullptr));
    lua_replace(L, top + 1);
    lua_settop(L, top + 1);
    return true;
}

bool DataAsset::pushAll(lua_State* L, const std::string& className, std::string* error) {
    const auto found = index().classes.find(className);
    if (found == index().classes.end()) {
        return dataAssetFailWith(error, "no data class '" + className + "' (a Lua module with ---@dataasset on its first lines)");
    }
    const std::vector<std::string> assets = found->second.assets;
    lua_newtable(L);
    const int list = lua_gettop(L);
    lua_Integer n = 1;
    for (const std::string& asset : assets) {
        std::string why;
        if (!push(L, asset, &why)) {
            Log::warn("DataAsset.loadAll('%s'): %s is left out: %s", className.c_str(), asset.c_str(), why.c_str());
            continue;
        }
        lua_rawseti(L, list, n++);
    }
    return true;
}

bool DataAsset::classOf(lua_State* L, int index, std::string& className) {
    if (!lua_istable(L, index) || !lua_getmetatable(L, index)) return false;
    lua_getfield(L, -1, "__dataclass");
    const bool found = lua_type(L, -1) == LUA_TSTRING;
    if (found) className = lua_tostring(L, -1);
    lua_pop(L, 2);
    return found;
}

bool DataAsset::pathOf(lua_State* L, int index, std::string& path) {
    std::string className;
    if (!classOf(L, index, className) || !lua_getmetatable(L, index)) return false;
    lua_getfield(L, -1, "__datapath");
    const bool found = lua_type(L, -1) == LUA_TSTRING;
    if (found) path = lua_tostring(L, -1);
    lua_pop(L, 2);
    return found;
}

void DataAsset::clear(lua_State* L) {
    dataAssetIndexState().loaded = false;
    if (!L) return;
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, DATA_ASSET_STATE);
}
