// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DataAssets.h"

#include "Project.h"
#include "asset/AssetTypes.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "component/ScriptComponent.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectFileWrites.h"
#include "util/ProjectUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::json;

    bool dataAssetFail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    std::string trimmedLine(const std::string& line) {
        size_t begin = 0, end = line.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(line[begin]))) begin++;
        while (end > begin && std::isspace(static_cast<unsigned char>(line[end - 1]))) end--;
        return line.substr(begin, end - begin);
    }

    // what a look at one Lua file found, kept while its time and size stay
    struct ClassFileEntry {
        fs::file_time_type time{};
        uintmax_t size = 0;
        bool dataClass = false;
    };

    struct ClassCache {
        std::mutex mutex;
        std::map<std::string, ClassFileEntry> files;   // by absolute generic path
    };

    ClassCache& classCache() {
        static auto* cache = new ClassCache();
        return *cache;
    }

    // the opening comment block decides: no more than this is read of a file
    constexpr size_t ANNOTATION_READ_BYTES = 4096;

    bool fileHasAnnotation(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::string head(ANNOTATION_READ_BYTES, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(in.gcount()));
        return DataAssets::hasAnnotation(head);
    }

    // a float as the shortest decimal that reads back as it (0.1f is 0.1 in the file, not 0.100000001)
    Json floatJson(float value) {
        if (!std::isfinite(value)) return 0.0;
        char text[32];
        for (int precision = 6; precision <= 9; precision++) {
            std::snprintf(text, sizeof(text), "%.*g", precision, static_cast<double>(value));
            if (static_cast<float>(std::strtod(text, nullptr)) == value) break;
        }
        return std::strtod(text, nullptr);
    }

    // the element type of `field` as a field of its own (a list's element is converted as one)
    ScriptProperty elementField(const ScriptProperty& field) {
        ScriptProperty element = field;
        element.type = field.elementType;
        return element;
    }

    // a reference path as the file keeps it: in the hint form of the asset's type (assets-relative for
    // assets, project-relative for scenes and prefabs), so a game finds it by path too
    std::string fileHint(Project* project, const std::string& projectRelative) {
        if (!project || projectRelative.empty()) return projectRelative;
        const AssetDatabase::Resolution found = project->getAssetDatabase().lookupPath(projectRelative);
        const AssetPathBase base = found.found() ? AssetTypes::info(found.type).base : AssetPathBase::Assets;
        return project->projectPathToAssetHint(projectRelative, base);
    }

    AssetType dataFieldAssetType(const ScriptProperty& field) {
        return assetTypeFromName(field.assetType);
    }

    // a reference given as JSON -> {GUID, project-relative path}. strict: it names an asset of the type
    bool referenceFromJson(Project* project, const Json& value, AssetType expected, bool prefab, bool strict, Guid& guid,
                           std::string& path, std::string& error) {
        guid = Guid();
        path.clear();
        std::string guidText, hint;
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) return true;
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (Guid::isValidText(text)) guidText = text;
            else hint = text;
        } else if (value.is_object()) {
            if (value.contains("guid") && value["guid"].is_string()) guidText = value["guid"].get<std::string>();
            if (value.contains("path") && value["path"].is_string()) hint = value["path"].get<std::string>();
        } else {
            error = "expected a path, a GUID, {guid, path} or null";
            return false;
        }
        if (!project) {
            guid = Guid::isValidText(guidText) ? Guid::parse(guidText) : Guid();
            path = hint;
            return true;
        }
        const AssetDatabase& database = project->getAssetDatabase();
        if (Guid::isValidText(guidText)) {
            guid = Guid::parse(guidText);
            const fs::path found = database.getPath(guid);
            if (!found.empty()) {
                path = found.generic_string();
            } else if (strict) {
                error = "no asset has the GUID " + guidText;
                return false;
            }
        }
        if (path.empty() && !hint.empty()) {
            // project-relative when the file is there, else relative to the type's root
            std::error_code ec;
            const AssetPathBase base = prefab ? AssetPathBase::Project
                                     : expected == AssetType::Unknown ? AssetPathBase::Assets : AssetTypes::info(expected).base;
            if (fs::exists(project->getProjectPath() / FileUtils::pathFromUtf8(hint), ec)) path = fs::path(hint).lexically_normal().generic_string();
            else path = project->assetHintToProjectPath(hint, base);
            const Guid known = database.getAssetGuid(fs::path(path));
            if (!known.isNull()) guid = known;
            else if (strict) {
                error = "'" + hint + "' is no asset of the project";
                return false;
            }
        }
        if (strict && !path.empty()) {
            const AssetDatabase::Resolution found = database.lookupPath(path);
            const AssetType type = prefab ? AssetType::Prefab : expected;
            if (type != AssetType::Unknown && found.found() && found.type != type) {
                error = "'" + path + "' is a " + assetTypeName(found.type) + ", not a " + assetTypeName(type);
                return false;
            }
        }
        return true;
    }

    bool dataNumberArray(const Json& value, size_t size, float* out, std::string& error) {
        static const char* const axes[] = {"x", "y", "z", "w"};
        static const char* const channels[] = {"r", "g", "b", "a"};
        if (value.is_array()) {
            if (value.size() < size - (size == 4 ? 1 : 0) || value.size() > size) {
                error = "expected an array of " + std::to_string(size) + " numbers";
                return false;
            }
            for (size_t i = 0; i < value.size(); i++) {
                if (!value[i].is_number()) {
                    error = "expected numbers";
                    return false;
                }
                out[i] = value[i].get<float>();
            }
            return true;
        }
        if (value.is_object()) {
            const char* const* names = value.contains("r") ? channels : axes;
            for (size_t i = 0; i < size; i++) {
                if (value.contains(names[i]) && value[names[i]].is_number()) out[i] = value[names[i]].get<float>();
                else if (!(i == 3 && size == 4)) {
                    error = std::string("expected a number at ") + names[i];
                    return false;
                }
            }
            return true;
        }
        error = "expected an array of " + std::to_string(size) + " numbers";
        return false;
    }

}

bool DataAssets::hasAnnotation(const std::string& code) {
    size_t start = 0;
    // a UTF-8 byte order mark opens some files
    if (code.compare(0, 3, "\xEF\xBB\xBF") == 0) start = 3;
    while (start < code.size()) {
        size_t end = code.find('\n', start);
        if (end == std::string::npos) end = code.size();
        const std::string line = trimmedLine(code.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        if (line.compare(0, 2, "--") != 0) return false;
        const std::string annotation = DataAsset::ANNOTATION;
        if (line.compare(0, annotation.size(), annotation) == 0 &&
            (line.size() == annotation.size() || std::isspace(static_cast<unsigned char>(line[annotation.size()])))) {
            return true;
        }
    }
    return false;
}

std::vector<DataAssets::Class> DataAssets::listClasses(const fs::path& luaDir) {
    std::vector<Class> classes;
    std::error_code ec;
    if (luaDir.empty() || !fs::is_directory(luaDir, ec)) return classes;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(luaDir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (AssetTypes::isSkippedDirectory(name)) it.disable_recursion_pending();
            continue;
        }
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".lua") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    ClassCache& cache = classCache();
    std::scoped_lock lock(cache.mutex);
    for (const fs::path& file : files) {
        const std::string key = file.generic_string();
        const fs::file_time_type time = fs::last_write_time(file, ec);
        const uintmax_t size = fs::file_size(file, ec);
        ClassFileEntry& entry = cache.files[key];
        if (entry.time != time || entry.size != size) {
            entry.time = time;
            entry.size = size;
            entry.dataClass = fileHasAnnotation(file);
        }
        if (!entry.dataClass) continue;
        Class dataClass;
        dataClass.name = file.stem().string();
        dataClass.luaPath = file.lexically_relative(luaDir).generic_string();
        dataClass.file = file;
        const bool taken = std::any_of(classes.begin(), classes.end(), [&](const Class& other) { return other.name == dataClass.name; });
        if (!taken) classes.push_back(std::move(dataClass));
    }
    std::sort(classes.begin(), classes.end(), [](const Class& a, const Class& b) { return a.name < b.name; });
    return classes;
}

std::vector<std::string> DataAssets::classNamesThrottled(const fs::path& luaDir) {
    static std::mutex mutex;
    static fs::path lastDir;
    static std::chrono::steady_clock::time_point lastLook;
    static std::vector<std::string> names;
    std::scoped_lock lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if (luaDir != lastDir || now - lastLook > std::chrono::seconds(1)) {
        names.clear();
        for (const Class& dataClass : listClasses(luaDir)) names.push_back(dataClass.name);
        lastDir = luaDir;
        lastLook = now;
    }
    return names;
}

bool DataAssets::findClass(const fs::path& luaDir, const std::string& name, Class& out) {
    for (Class& dataClass : listClasses(luaDir)) {
        if (dataClass.name == name) {
            out = std::move(dataClass);
            return true;
        }
    }
    return false;
}

bool DataAssets::readSchema(const Class& dataClass, std::vector<ScriptProperty>& schema, std::string& error) {
    std::string code;
    if (!ProjectChecks::readFile(dataClass.file, code)) {
        error = "cannot read " + dataClass.luaPath;
        return false;
    }
    ScriptEntry entry;
    entry.type = ScriptType::LUA;
    entry.className = dataClass.name;
    const ProjectUtils::LuaPropertiesResult result = ProjectUtils::readLuaScriptProperties(entry, code, dataClass.luaPath);
    if (result.status != ProjectUtils::LuaPropertiesResult::Ok) {
        error = result.message;
        return false;
    }
    if (!result.returnedTable) {
        error = dataClass.luaPath + " does not return a table";
        return false;
    }
    for (const ScriptProperty& field : entry.properties) {
        const bool entity = field.type == ScriptPropertyType::EntityReference ||
                            (field.type == ScriptPropertyType::List && field.elementType == ScriptPropertyType::EntityReference);
        if (entity) {
            // an unknown type name reads as an entity reference: say which it was
            error = "property '" + field.name + "': " +
                    (field.ptrTypeName.empty() ? std::string("a data class cannot reference a GameObject (an asset outlives the scenes): use a prefab or a Data Asset")
                                               : "type '" + field.ptrTypeName + "' is neither a property type nor a data class");
            return false;
        }
    }
    schema = std::move(entry.properties);
    return true;
}

bool DataAssets::parse(const std::string& text, Document& document, std::string* error) {
    const Json json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return dataAssetFail(error, "not JSON");
    if (!json.contains("version") || !json["version"].is_number_integer()) return dataAssetFail(error, "no version");
    document.version = json["version"].get<int>();
    if (document.version > DataAsset::VERSION) {
        return dataAssetFail(error, "made by a newer editor (version " + std::to_string(document.version) + ")");
    }
    if (!json.contains("class") || !json["class"].is_string() || json["class"].get<std::string>().empty()) {
        return dataAssetFail(error, "no class");
    }
    document.className = json["class"].get<std::string>();
    document.values = json.contains("values") && json["values"].is_object() ? json["values"] : Json::object();
    return true;
}

std::string DataAssets::write(const Document& document) {
    const Json json = {{"version", DataAsset::VERSION}, {"class", document.className}, {"values", document.values}};
    return json.dump(2) + "\n";
}

bool DataAssets::read(const fs::path& file, Document& document, std::string* error) {
    std::string text;
    if (!ProjectChecks::readFile(file, text)) return dataAssetFail(error, "cannot read " + FileUtils::pathToGenericUtf8(file));
    return parse(text, document, error);
}

std::string DataAssets::classOfFile(const fs::path& file) {
    Document document;
    return read(file, document, nullptr) ? document.className : std::string();
}

DataAssets::Document DataAssets::create(Project* project, const std::string& className, const std::vector<ScriptProperty>& schema) {
    Document document;
    document.className = className;
    for (const ScriptProperty& field : schema) document.values[field.name] = toJson(project, field, field.defaultValue);
    return document;
}

ScriptPropertyValue DataAssets::valueOf(Project* project, const ScriptProperty& field, const Document& document) {
    const auto found = document.values.find(field.name);
    if (found == document.values.end()) return field.defaultValue;
    ScriptPropertyValue value;
    std::string error;
    return fromJson(project, field, *found, false, value, error) ? value : field.defaultValue;
}

Json DataAssets::toJson(Project* project, const ScriptProperty& field, const ScriptPropertyValue& value) {
    if (field.type == ScriptPropertyType::List) {
        Json list = Json::array();
        if (const ScriptPropertyList* items = std::get_if<ScriptPropertyList>(&value)) {
            const ScriptProperty element = elementField(field);
            for (const ScriptPropertyElement& item : items->items) list.push_back(toJson(project, element, scriptPropertyValueOf(item)));
        }
        return list;
    }
    if (const bool* b = std::get_if<bool>(&value)) return *b;
    if (const int* i = std::get_if<int>(&value)) return *i;
    if (const float* f = std::get_if<float>(&value)) return floatJson(*f);
    if (const std::string* s = std::get_if<std::string>(&value)) return *s;
    if (const Vector2* v = std::get_if<Vector2>(&value)) return Json::array({floatJson(v->x), floatJson(v->y)});
    if (const Vector3* v = std::get_if<Vector3>(&value)) return Json::array({floatJson(v->x), floatJson(v->y), floatJson(v->z)});
    if (const Vector4* v = std::get_if<Vector4>(&value)) return Json::array({floatJson(v->x), floatJson(v->y), floatJson(v->z), floatJson(v->w)});
    if (const PrefabAsset* prefab = std::get_if<PrefabAsset>(&value)) {
        if (prefab->isEmpty()) return nullptr;
        Json ref = Json::object();
        if (!prefab->getGuid().empty()) ref["guid"] = prefab->getGuid();
        if (!prefab->getPath().empty()) ref["path"] = prefab->getPath();
        return ref;
    }
    if (const AssetRef* asset = std::get_if<AssetRef>(&value)) {
        if (!asset->isSet()) return nullptr;
        Json ref = Json::object();
        if (!asset->guid.isNull()) ref["guid"] = asset->guid.str();
        if (!asset->path.empty()) ref["path"] = fileHint(project, asset->path);
        return ref;
    }
    return toJson(project, field, scriptPropertyValueOf(scriptPropertyElementDefault(field.type)));
}

bool DataAssets::elementFromJson(Project* project, const ScriptProperty& field, const Json& value, bool strict,
                                 ScriptPropertyElement& out, std::string& error) {
    ScriptPropertyValue single;
    if (!fromJson(project, elementField(field), value, strict, single, error)) return false;
    out = scriptPropertyElementOf(single);
    return true;
}

bool DataAssets::fromJson(Project* project, const ScriptProperty& field, const Json& value, bool strict, ScriptPropertyValue& out,
                          std::string& error) {
    float numbers[4] = {0, 0, 0, 1};
    switch (field.type) {
        case ScriptPropertyType::Bool:
            if (!value.is_boolean()) {
                error = "expected true or false";
                return false;
            }
            out = value.get<bool>();
            return true;
        case ScriptPropertyType::Int:
            if (!value.is_number() || (value.is_number_float() && std::floor(value.get<double>()) != value.get<double>())) {
                error = "expected an integer";
                return false;
            }
            out = value.is_number_float() ? static_cast<int>(value.get<double>()) : value.get<int>();
            return true;
        case ScriptPropertyType::Float:
            if (!value.is_number()) {
                error = "expected a number";
                return false;
            }
            out = value.get<float>();
            return true;
        case ScriptPropertyType::String:
            if (!value.is_string()) {
                error = "expected a string";
                return false;
            }
            out = value.get<std::string>();
            return true;
        case ScriptPropertyType::Vector2:
            if (!dataNumberArray(value, 2, numbers, error)) return false;
            out = Vector2(numbers[0], numbers[1]);
            return true;
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3:
            if (!dataNumberArray(value, 3, numbers, error)) return false;
            out = Vector3(numbers[0], numbers[1], numbers[2]);
            return true;
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4:
            if (!dataNumberArray(value, 4, numbers, error)) return false;
            out = Vector4(numbers[0], numbers[1], numbers[2], numbers[3]);
            return true;
        case ScriptPropertyType::Prefab: {
            Guid guid;
            std::string path;
            if (!referenceFromJson(project, value, AssetType::Prefab, true, strict, guid, path, error)) return false;
            out = PrefabAsset(guid.isNull() ? std::string() : guid.str(), path);
            return true;
        }
        case ScriptPropertyType::Asset: {
            Guid guid;
            std::string path;
            if (!referenceFromJson(project, value, dataFieldAssetType(field), false, strict, guid, path, error)) return false;
            if (strict && field.assetType == DataAsset::ASSET_TYPE && !field.ptrTypeName.empty() && !path.empty() && project) {
                const std::string className = classOfFile(project->getProjectPath() / FileUtils::pathFromUtf8(path));
                if (className != field.ptrTypeName) {
                    error = "'" + path + "' is " + (className.empty() ? std::string("no Data Asset") : "a " + className) + ", not a " + field.ptrTypeName;
                    return false;
                }
            }
            out = AssetRef(path, guid);
            return true;
        }
        case ScriptPropertyType::List: {
            if (!value.is_array()) {
                error = "expected an array";
                return false;
            }
            ScriptPropertyList list;
            for (size_t i = 0; i < value.size(); i++) {
                ScriptPropertyElement element;
                std::string why;
                if (!elementFromJson(project, field, value[i], strict, element, why)) {
                    error = "[" + std::to_string(i) + "]: " + why;
                    return false;
                }
                list.items.push_back(std::move(element));
            }
            out = std::move(list);
            return true;
        }
        case ScriptPropertyType::EntityReference:
            break;
    }
    error = "a data class cannot reference a GameObject";
    return false;
}

std::vector<std::string> DataAssets::problems(Project* project, const Document& document) {
    std::vector<std::string> found;
    if (!project) return found;
    Class dataClass;
    if (!findClass(project->getLuaPath(), document.className, dataClass)) {
        found.push_back("class '" + document.className + "' is no data class of the project (a Lua file whose first lines hold ---@dataasset)");
        return found;
    }
    std::vector<ScriptProperty> schema;
    std::string error;
    if (!readSchema(dataClass, schema, error)) {
        found.push_back("class '" + document.className + "' (" + dataClass.luaPath + "): " + error);
        return found;
    }
    for (auto it = document.values.begin(); it != document.values.end(); ++it) {
        const auto field = std::find_if(schema.begin(), schema.end(), [&](const ScriptProperty& f) { return f.name == it.key(); });
        if (field == schema.end()) {
            found.push_back("values." + it.key() + ": " + document.className + " has no such field (renamed or removed?): the value stays in the file");
            continue;
        }
        ScriptPropertyValue value;
        std::string why;
        if (!fromJson(project, *field, it.value(), true, value, why)) found.push_back("values." + it.key() + ": " + why);
    }
    return found;
}

bool DataAssets::commit(Project* project, const fs::path& file, const Document& document, const std::string& undoName,
                        const std::string& mergeKey, std::string* error) {
    if (!project) return dataAssetFail(error, "no project is open");
    const std::string text = write(document);
    std::string current;
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && ProjectChecks::readFile(file, current) && current == text) return true;

    auto* command = new WriteFilesCmd({{file, text}}, [project](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
    });
    command->setName(undoName);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey("data-asset\n" + FileUtils::pathToGenericUtf8(file) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        return dataAssetFail(error, "writing " + FileUtils::pathToGenericUtf8(file) + " failed" + (failure->empty() ? "" : ": " + *failure));
    }
    return true;
}

DataAsset::Index DataAssets::buildIndex(Project* project) {
    DataAsset::Index index;
    if (!project) return index;
    for (const Class& dataClass : listClasses(project->getLuaPath())) {
        index.classes[dataClass.name].module = dataClass.luaPath;
    }
    std::vector<std::string> files;
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type == AssetType::DataAsset) files.push_back(info.path);
    });
    std::sort(files.begin(), files.end());
    for (const std::string& relative : files) {
        const fs::path absolute = project->getProjectPath() / FileUtils::pathFromUtf8(relative);
        const auto found = index.classes.find(classOfFile(absolute));
        if (found == index.classes.end()) continue;
        found->second.assets.push_back(project->normalizeToAssetsRelative(absolute).generic_string());
    }
    return index;
}

std::string DataAssets::newFileName(const std::string& className) {
    return "New " + className + DataAsset::EXTENSION;
}
