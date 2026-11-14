// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Data Assets (docs/specs/S95-lua-runtime-gaps.md REQ-24): edit_data_asset lists the project's data
// classes and their assets, makes a .dataasset of a class, reads one, and sets its values as one
// undo step (every value checked first: nothing is written when one is wrong).
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "dataasset/DataAssets.h"
#include "util/AssetDatabase.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

// where the call's `path` is: a .dataasset inside the assets folder
bool dataAssetFile(Project* project, const std::string& text, fs::path& file, std::string& error) {
    std::string named = text;
    if (!Util::isDataAssetFile(named)) named += DataAsset::EXTENSION;
    std::error_code ec;
    fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(named);
    if (!fs::exists(path, ec) && !project->isInsideAssetsPath(path)) path = project->resolveAssetPath(FileUtils::pathFromUtf8(named));
    file = path.lexically_normal();
    if (!project->isInsideAssetsPath(file)) {
        error = "`path` must be a .dataasset inside the assets folder (\"data/sword.dataasset\").";
        return false;
    }
    return true;
}

std::string dataFieldTypeName(const ScriptProperty& field) {
    std::string name;
    switch (field.type == ScriptPropertyType::List ? field.elementType : field.type) {
        case ScriptPropertyType::Bool: name = "bool"; break;
        case ScriptPropertyType::Int: name = "int"; break;
        case ScriptPropertyType::Float: name = "float"; break;
        case ScriptPropertyType::String: name = "string"; break;
        case ScriptPropertyType::Vector2: name = "vector2"; break;
        case ScriptPropertyType::Vector3: name = "vector3"; break;
        case ScriptPropertyType::Vector4: name = "vector4"; break;
        case ScriptPropertyType::Color3: name = "color3"; break;
        case ScriptPropertyType::Color4: name = "color4"; break;
        case ScriptPropertyType::Prefab: name = "prefab"; break;
        case ScriptPropertyType::Asset:
            name = !field.ptrTypeName.empty() ? field.ptrTypeName : field.assetType.empty() ? "asset" : "asset:" + field.assetType;
            break;
        case ScriptPropertyType::EntityReference: name = "entity"; break;
        case ScriptPropertyType::List: break;
    }
    return field.type == ScriptPropertyType::List ? "list:" + name : name;
}

Json fieldsJson(const std::vector<ScriptProperty>& schema) {
    Json fields = Json::array();
    for (const ScriptProperty& field : schema) fields.push_back({{"name", field.name}, {"type", dataFieldTypeName(field)}});
    return fields;
}

// a class and its schema; false with the call failed
bool classSchema(const CallPtr& call, Project* project, const std::string& className, DataAssets::Class& dataClass,
                 std::vector<ScriptProperty>& schema) {
    if (!DataAssets::findClass(project->getLuaPath(), className, dataClass)) {
        std::string known;
        for (const DataAssets::Class& c : DataAssets::listClasses(project->getLuaPath())) known += (known.empty() ? "" : ", ") + c.name;
        call->fail("No data class '" + className + "'" + (known.empty() ? " (the project has none)" : " (the project's: " + known + ")") +
                   ". A data class is a Lua module in the Lua folder whose first lines hold ---@dataasset; its `properties` are the fields "
                   "(make one with create_script, then come back).");
        return false;
    }
    std::string error;
    if (!DataAssets::readSchema(dataClass, schema, error)) {
        call->fail("Data class '" + className + "' (" + dataClass.luaPath + "): " + error + ". Fix its properties table.");
        return false;
    }
    return true;
}

// the values given, checked against the schema, into the document; false with every error listed
bool applyDataValues(Project* project, const std::vector<ScriptProperty>& schema, const Json& values, DataAssets::Document& document,
                 std::vector<std::string>& errors) {
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto field = std::find_if(schema.begin(), schema.end(), [&](const ScriptProperty& f) { return f.name == it.key(); });
        if (field == schema.end()) {
            std::string names;
            for (const ScriptProperty& f : schema) names += (names.empty() ? "" : ", ") + f.name;
            errors.push_back(it.key() + ": " + document.className + " has no such field (its fields: " + names + ")");
            continue;
        }
        if (it.value().is_null()) {
            document.values[field->name] = DataAssets::toJson(project, *field, field->defaultValue);
            continue;
        }
        ScriptPropertyValue value;
        std::string why;
        if (!DataAssets::fromJson(project, *field, it.value(), true, value, why)) {
            errors.push_back(it.key() + " (" + dataFieldTypeName(*field) + "): " + why);
            continue;
        }
        document.values[field->name] = DataAssets::toJson(project, *field, value);
    }
    return errors.empty();
}

std::string joinedDataErrors(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) text += (text.empty() ? "" : "; ") + line;
    return text;
}

// what get and a write report: the class, every field's value (stored or default), values of no field, problems
Json describeDataAsset(Project* project, const fs::path& file, const DataAssets::Document& document, const std::vector<ScriptProperty>& schema) {
    Json values = Json::object();
    Json unknown = Json::object();
    for (const ScriptProperty& field : schema) values[field.name] = DataAssets::toJson(project, field, DataAssets::valueOf(project, field, document));
    for (auto it = document.values.begin(); it != document.values.end(); ++it) {
        if (!values.contains(it.key())) unknown[it.key()] = it.value();
    }
    Json data = {{"path", project->normalizeToProjectRelative(file).generic_string()},
                 {"guid", project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(file)).str()},
                 {"class", document.className}, {"fields", fieldsJson(schema)}, {"values", values},
                 {"problems", DataAssets::problems(project, document)}};
    if (!unknown.empty()) data["unknown_values"] = unknown;
    return data;
}

void listAction(const CallPtr& call, Project* project) {
    const std::string only = call->args().value("class", std::string());
    const DataAsset::Index index = DataAssets::buildIndex(project);
    Json classes = Json::array();
    for (const DataAssets::Class& dataClass : DataAssets::listClasses(project->getLuaPath())) {
        if (!only.empty() && dataClass.name != only) continue;
        Json item = {{"class", dataClass.name}, {"script", project->normalizeToProjectRelative(dataClass.file).generic_string()}};
        std::vector<ScriptProperty> schema;
        std::string error;
        if (DataAssets::readSchema(dataClass, schema, error)) item["fields"] = fieldsJson(schema);
        else item["error"] = error;
        const auto found = index.classes.find(dataClass.name);
        item["assets"] = found == index.classes.end() ? Json::array() : Json(found->second.assets);
        classes.push_back(std::move(item));
    }
    if (!only.empty() && classes.empty()) {
        call->fail("No data class '" + only + "'. Call edit_data_asset action: list without class to see them.");
        return;
    }
    std::string text = std::to_string(classes.size()) + " data class(es)";
    for (const Json& item : classes) text += (text.back() == ')' ? ": " : ", ") + item["class"].get<std::string>() + " (" + std::to_string(item["assets"].size()) + " asset(s))";
    Json data = {{"classes", classes}};
    if (classes.empty()) {
        data["next"] = "A data class is a Lua module in the Lua folder whose first lines hold ---@dataasset and whose properties table lists its "
                       "fields: make one with create_script, then edit_data_asset action: create.";
        text += ". " + data["next"].get<std::string>();
    }
    call->ok(text + ".", data);
}

void editDataAssetTool(CallPtr call) {
    Project* project = call->ctx().project();
    if (!project || project->getProjectPath().empty()) {
        call->fail("No project is open.");
        return;
    }
    const Json& args = call->args();
    const std::string action = args.value("action", std::string());
    if (action == "list") {
        listAction(call, project);
        return;
    }
    if (!args.contains("path") || !args["path"].is_string()) {
        call->fail("action: " + action + " needs `path` (a .dataasset in the assets folder).");
        return;
    }
    fs::path file;
    std::string error;
    if (!dataAssetFile(project, args["path"].get<std::string>(), file, error)) {
        call->fail(error);
        return;
    }
    const std::string rel = project->normalizeToProjectRelative(file).generic_string();
    std::error_code ec;
    const bool exists = fs::exists(file, ec);
    const Json values = args.contains("values") && args["values"].is_object() ? args["values"] : Json::object();

    if (action == "create") {
        if (exists) {
            call->fail(rel + " exists: change it with action: set.");
            return;
        }
        const std::string className = args.value("class", std::string());
        if (className.empty()) {
            call->fail("action: create needs `class` (a data class; action: list shows them).");
            return;
        }
        DataAssets::Class dataClass;
        std::vector<ScriptProperty> schema;
        if (!classSchema(call, project, className, dataClass, schema)) return;
        DataAssets::Document document = DataAssets::create(project, className, schema);
        std::vector<std::string> errors;
        if (!applyDataValues(project, schema, values, document, errors)) {
            call->fail("Nothing was written: " + joinedDataErrors(errors) + ".", {{"errors", errors}});
            return;
        }
        fs::create_directories(file.parent_path(), ec);
        if (!DataAssets::commit(project, file, document, "Create " + className + " " + file.stem().string(), "", &error)) {
            call->fail(error);
            return;
        }
        // a new file has no entry until the database next looks, and the reply names its GUID
        project->refreshAssetDatabase(true);
        Json data = describeDataAsset(project, file, document, schema);
        data["written"] = true;
        data["next"] = "Scripts reach it through a property typed \"" + className + "\" (set_script_properties: this path), or "
                       "DataAsset.load(\"" + project->normalizeToAssetsRelative(file).generic_string() + "\") in Lua.";
        call->ok("Created " + rel + " (" + className + ", " + std::to_string(schema.size()) + " field(s)). One undo step. " +
                 data["next"].get<std::string>(), data);
        return;
    }

    if (!exists) {
        call->fail(rel + " does not exist: make it with action: create and its `class`.");
        return;
    }
    DataAssets::Document document;
    if (!DataAssets::read(file, document, &error)) {
        call->fail(rel + ": " + error + ".");
        return;
    }
    DataAssets::Class dataClass;
    std::vector<ScriptProperty> schema;
    if (!classSchema(call, project, document.className, dataClass, schema)) return;

    if (action == "get") {
        const Json data = describeDataAsset(project, file, document, schema);
        const size_t problems = data["problems"].size();
        call->ok(rel + ": " + document.className + ", " + std::to_string(schema.size()) + " field(s), " + std::to_string(problems) +
                 " problem(s)" + (problems ? ": " + data["problems"][0].get<std::string>() : std::string()) + ".", data);
        return;
    }
    if (action != "set") {
        call->fail("action must be create, get, set or list.");
        return;
    }
    if (values.empty()) {
        call->fail("action: set needs `values` ({field: value}; null resets a field to its default).");
        return;
    }
    DataAssets::Document changed = document;
    std::vector<std::string> errors;
    if (!applyDataValues(project, schema, values, changed, errors)) {
        call->fail("Nothing was written: " + joinedDataErrors(errors) + ".", {{"errors", errors}});
        return;
    }
    const bool written = changed.values != document.values;
    if (written && !DataAssets::commit(project, file, changed, "Set " + document.className + " " + file.stem().string(), "", &error)) {
        call->fail(error);
        return;
    }
    Json data = describeDataAsset(project, file, changed, schema);
    data["written"] = written;
    call->ok(std::string(written ? "Changed " : "Unchanged ") + rel + " (" + std::to_string(values.size()) + " value(s))." +
             (written ? " One undo step." : ""), data);
}

} // namespace

Json dataAssetInfo(Project* project, const fs::path& file) {
    DataAssets::Document document;
    std::string error;
    if (!DataAssets::read(file, document, &error)) return {{"class", nullptr}, {"problems", Json::array({error})}};
    DataAssets::Class dataClass;
    std::vector<ScriptProperty> schema;
    if (!DataAssets::findClass(project->getLuaPath(), document.className, dataClass) || !DataAssets::readSchema(dataClass, schema, error)) {
        return {{"class", document.className}, {"values", document.values}, {"problems", DataAssets::problems(project, document)}};
    }
    Json data = describeDataAsset(project, file, document, schema);
    data.erase("path");
    data.erase("guid");
    return data;
}

void registerDataAssetTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "edit_data_asset";
    tool.title = "Edit Data Asset";
    tool.description =
        "Data Assets (Unity's ScriptableObject): list the data classes (Lua modules marked ---@dataasset) and their assets, "
        "create a .dataasset of a class, get its values, set some (checked first, one undo step). Edit them with this tool, "
        "not by writing the JSON. Fields: jasper://docs/lua-api.";
    tool.inputSchema = object({
        {"action", stringEnum("", {"list", "create", "get", "set"})},
        {"path", assetPath("A .dataasset in the assets folder.")},
        {"class", string("create: its data class; list: only this one.")},
        {"values", anyObject("{field: value}; null resets a field. References: a path or GUID.")}
    }, {"action"});
    tool.activity = ToolActivity::Edit;
    tool.allowedWhilePlaying = true;
    tool.writesFiles = [](const Json& args) {
        const std::string action = args.value("action", std::string());
        return action == "create" || action == "set";
    };
    tool.handler = editDataAssetTool;
    tool.group = "scripting";
    registry.addTool(std::move(tool));
}

}
