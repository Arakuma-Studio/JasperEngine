// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "script/DataAsset.h"
#include "script/ScriptProperty.h"

#include "json.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Data Assets as the editor makes and edits them (docs/specs/S95-lua-runtime-gaps.md REQ-20 to
    // REQ-24): the data classes of the project (Lua modules marked ---@dataasset), their schemas (their
    // `properties`, read as a script's), the .dataasset files and their values. The Inspector, the
    // Create menu, the MCP edit_data_asset tool, editor Play and the export use it. Every change is
    // one undo step of the project history (WriteFilesCmd); the file holds no GUID, its .meta does.
    class DataAssets {
    public:
        struct Class {
            std::string name;               // the file's name without .lua, as a script's class
            std::string luaPath;            // relative to the Lua directory, generic ("data/ItemData.lua")
            std::filesystem::path file;     // absolute
        };

        // The data classes below luaDir, by name. Any thread: files are read again only when their
        // time or size changed. Two classes of one name: the first path wins
        static std::vector<Class> listClasses(const std::filesystem::path& luaDir);
        static bool findClass(const std::filesystem::path& luaDir, const std::string& name, Class& out);
        // the class names, looked for at most once a second (the main menu, built every frame)
        static std::vector<std::string> classNamesThrottled(const std::filesystem::path& luaDir);
        // the code marks a data class: ---@dataasset among the comment lines that open it
        static bool hasAnnotation(const std::string& code);

        // A class's schema: its `properties` read as a script's, refused when a field references a
        // GameObject or names an unknown type. False with the reason
        static bool readSchema(const Class& dataClass, std::vector<ScriptProperty>& schema, std::string& error);

        struct Document {
            int version = DataAsset::VERSION;
            std::string className;
            nlohmann::json values = nlohmann::json::object();   // as the file holds them: unknown keys kept
        };
        // false + why: not JSON, no class, a newer version
        static bool parse(const std::string& text, Document& document, std::string* error);
        // JSON, keys sorted, 2-space indent, LF
        static std::string write(const Document& document);
        static bool read(const std::filesystem::path& file, Document& document, std::string* error);
        // the class the file names ("" when it does not read)
        static std::string classOfFile(const std::filesystem::path& file);

        // A new document of the class with every field at its default (Unity writes them all)
        static Document create(Project* project, const std::string& className, const std::vector<ScriptProperty>& schema);

        // The value a field has in the document: the stored one when it converts, else its default
        static ScriptPropertyValue valueOf(Project* project, const ScriptProperty& field, const Document& document);
        // A value as the file stores it (references {guid, path}, the path in its type's hint form)
        static nlohmann::json toJson(Project* project, const ScriptProperty& field, const ScriptPropertyValue& value);
        // A value given as JSON (the file's form; MCP's too: vectors as arrays or {x, y, z[, w]},
        // references as a path, a GUID, {guid, path} or null). strict: a reference must name an asset
        // of its type (and class); otherwise it is kept as written, missing or not. False + why
        static bool fromJson(Project* project, const ScriptProperty& field, const nlohmann::json& value, bool strict,
                             ScriptPropertyValue& out, std::string& error);
        // the file's value of an element type (a list's elements)
        static bool elementFromJson(Project* project, const ScriptProperty& field, const nlohmann::json& value, bool strict,
                                    ScriptPropertyElement& out, std::string& error);

        // What a look at the document finds: its class missing or broken, values of no field (a
        // renamed field: they stay in the file), values of the wrong type, references to no asset
        static std::vector<std::string> problems(Project* project, const Document& document);

        // The document written as one undo step named `undoName`; a non-empty merge key merges it
        // with the step before of the same key (a drag). False + error
        static bool commit(Project* project, const std::filesystem::path& file, const Document& document, const std::string& undoName,
                           const std::string& mergeKey, std::string* error);

        // The project's classes and their assets: editor Play's index (DataAsset::setIndexProvider)
        // and the export's je-dataassets.json
        static DataAsset::Index buildIndex(Project* project);

        // "Ammo" -> "New Ammo.dataasset" (Assets > Create > Data Asset > <Class>)
        static std::string newFileName(const std::string& className);
    };

}
