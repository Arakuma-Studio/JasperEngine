// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// String Tables (docs/specs/S95-lua-runtime-gaps.md REQ-30): edit_string_table makes, reads and
// changes a .stringtable (each write one undo step), reports the keys each language misses and the
// keys two tables define, and moves the strings through CSV or XLIFF (S87's interchange).
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpRefs.h"

#include "Project.h"
#include "localization/StringTables.h"
#include "util/FileUtils.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

constexpr const char* STRING_TABLE_ACTIONS = "create, get, set, remove, import, export";

std::string locLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// where the call's `path` is: inside the assets folder, with its extension
bool locTableFile(Project* project, const std::string& text, fs::path& file, std::string& error) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    if (text.empty()) {
        error = "`path` is a .stringtable in the assets folder (\"Localization/UI.stringtable\").";
        return false;
    }
    std::string named = text;
    if (!Util::isStringTableFile(named)) named += StringTable::EXTENSION;
    std::error_code ec;
    fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(named);
    if (!fs::exists(path, ec) && !project->isInsideAssetsPath(path)) path = project->resolveAssetPath(FileUtils::pathFromUtf8(named));
    file = path.lexically_normal();
    if (!project->isInsideAssetsPath(file)) {
        error = "`path` must be a .stringtable inside the assets folder (\"Localization/UI.stringtable\").";
        return false;
    }
    return true;
}

Json locValueJson(const StringTableValue& value) {
    if (!value.plural) return value.text;
    Json forms = Json::object();
    for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++) {
        if (value.has((PluralCategory)c)) forms[PluralRules::name((PluralCategory)c)] = value.forms[(size_t)c];
    }
    return forms;
}

bool locReadValue(const Json& json, StringTableValue& out, std::string& error) {
    if (json.is_string()) {
        out = StringTableValue::ofText(json.get<std::string>());
        return true;
    }
    if (!json.is_object() || json.empty()) {
        error = "a value is a string or plural forms {\"one\": ..., \"other\": ...}";
        return false;
    }
    out = StringTableValue();
    for (auto it = json.begin(); it != json.end(); ++it) {
        PluralCategory category;
        if (!PluralRules::fromName(it.key(), category) || !it->is_string()) {
            error = "plural forms are zero, one, two, few, many, other, each a string ('" + it.key() + "' is not)";
            return false;
        }
        out.setForm(category, it->get<std::string>());
    }
    return true;
}

// the languages a value may be in: the project's when it lists any
std::string locLanguageRefusal(Project* project, const std::string& code) {
    std::string why;
    if (!TitleArtLanguage::isValid(code, &why)) return "'" + code + "': " + why;
    const std::vector<std::string>& languages = project->getLocalizationSettings().languages;
    if (languages.empty()) return "";
    const std::string wanted = locLower(TitleArtLanguage::canonicalize(code));
    for (const std::string& l : languages) {
        if (locLower(l) == wanted) return "";
    }
    return "'" + code + "' is not one of the project's languages: add it to Project Settings > Localization > Languages "
           "(set_project_settings localization.languages) first";
}

Json locMissingJson(Project* project, const StringTable& table, std::string& next) {
    Json missing = Json::object();
    for (const std::string& code : stringtables::editLanguages(project, table)) {
        const std::vector<std::string> keys = stringtables::missingKeys(table, code);
        missing[code] = keys;
        if (!keys.empty()) {
            next += (next.empty() ? "" : " ") + code + " misses " + std::to_string(keys.size()) + " key(s) (" + keys.front() + (keys.size() > 1 ? ", ..." : "") + ").";
        }
    }
    return missing;
}

// the keys of this table that another table defines too: Localization.get(key) finds the first
Json locDuplicatesJson(Project* project, const std::string& name) {
    Json out = Json::array();
    for (const auto& [key, names] : stringtables::duplicateKeys(stringtables::readAll(project->getProjectPath(), project->getAssetsPath()))) {
        if (std::find(names.begin(), names.end(), name) != names.end()) out.push_back({{"key", key}, {"tables", names}});
    }
    return out;
}

Json locTableJson(const StringTable& table, const Json& keys) {
    Json entries = Json::array();
    for (const StringTableEntry& entry : table.entries) {
        if (keys.is_array() && !keys.empty() && std::find(keys.begin(), keys.end(), Json(entry.key)) == keys.end()) continue;
        Json values = Json::object();
        for (const auto& value : entry.values) values[value.first] = locValueJson(value.second);
        Json item = {{"key", entry.key}, {"values", values}};
        if (!entry.comment.empty()) item["comment"] = entry.comment;
        entries.push_back(std::move(item));
    }
    return entries;
}

// a reply about the table as written (or read)
void locReply(const CallPtr& call, Project* project, const fs::path& file, const StringTable& table, const std::string& summary, Json data) {
    const std::string name = StringTable::nameOfPath(file.filename().string());
    std::string next;
    data["path"] = w4::projectRelative(project, file);
    data["name"] = name;
    data["keys"] = table.entries.size();
    data["languages"] = stringtables::editLanguages(project, table);
    data["missing"] = locMissingJson(project, table, next);
    const Json duplicates = locDuplicatesJson(project, name);
    if (!duplicates.empty()) {
        data["duplicates"] = duplicates;
        next += (next.empty() ? "" : " ") + std::string("Keys another table defines too: rename them, or look them up as \"") + name + ":key\".";
    }
    if (!next.empty()) data["next"] = next;
    call->ok(summary + (next.empty() ? std::string(" Nothing missing.") : " " + next), data);
}

bool locWrite(const CallPtr& call, Project* project, const fs::path& file, const StringTable& table, const std::string& step) {
    std::string error;
    if (!stringtables::write(project, file, table, step, error)) {
        call->fail(w4::projectRelative(project, file) + ": " + error);
        return false;
    }
    return true;
}

void locSet(const CallPtr& call, Project* project, const fs::path& file, StringTable table, const Json& entries, const std::string& rel, bool created) {
    if (!entries.is_object()) {
        call->fail("`entries` is {key: {language: text or plural forms or null, comment?: text}}.");
        return;
    }
    std::vector<std::string> errors;
    int changedKeys = 0;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const std::string key = it.key();
        const std::string why = StringTable::keyProblem(key);
        if (!why.empty()) {
            errors.push_back(why);
            continue;
        }
        if (!it->is_object()) {
            errors.push_back("'" + key + "': give {language: value}");
            continue;
        }
        StringTableEntry* entry = table.find(key);
        if (!entry) {
            table.entries.push_back(StringTableEntry());
            entry = &table.entries.back();
            entry->key = key;
        }
        const StringTableEntry before = *entry;
        for (auto value = it->begin(); value != it->end(); ++value) {
            if (value.key() == "comment") {
                if (value->is_string()) entry->comment = value->get<std::string>();
                else if (value->is_null()) entry->comment.clear();
                else errors.push_back("'" + key + "' comment is a string");
                continue;
            }
            const std::string refusal = locLanguageRefusal(project, value.key());
            if (!refusal.empty()) {
                errors.push_back("'" + key + "': " + refusal);
                continue;
            }
            const std::string code = TitleArtLanguage::canonicalize(value.key());
            if (value->is_null()) {
                entry->erase(code);
                continue;
            }
            StringTableValue parsed;
            std::string error;
            if (!locReadValue(*value, parsed, error)) {
                errors.push_back("'" + key + "' " + code + ": " + error);
                continue;
            }
            entry->set(code, parsed);
        }
        if (!(before == *entry)) changedKeys++;
    }
    if (!errors.empty()) {
        call->fail("Nothing was written: " + errors.front() + (errors.size() > 1 ? " (+" + std::to_string(errors.size() - 1) + " more)" : "") + ".");
        return;
    }
    if ((changedKeys > 0 || created) && !locWrite(call, project, file, table, (created ? "Create String Table " : "Edit String Table ") + StringTable::nameOfPath(rel))) return;
    Json data = Json::object();
    if (created) {
        // a new file has no entry until the database next looks, and the reply names its GUID
        project->refreshAssetDatabase(true);
        data["guid"] = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(file)).str();
    }
    locReply(call, project, file, table, std::string(created ? "Created " : changedKeys ? "Changed " : "Unchanged ") + rel + ": " +
             std::to_string(changedKeys) + " key(s) set" + (changedKeys || created ? ", one undo step." : "."), data);
}

void editStringTableTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string action = args.value("action", std::string("get"));
    fs::path file;
    std::string error;
    if (!locTableFile(project, args.value("path", std::string()), file, error)) {
        call->fail(error);
        return;
    }
    const std::string rel = w4::projectRelative(project, file);
    std::error_code ec;
    const bool exists = fs::is_regular_file(file, ec);
    if (action == "create") {
        if (exists) {
            call->fail(rel + " already exists: use action set to change it.");
            return;
        }
        locSet(call, project, file, StringTable(), args.value("entries", Json::object()), rel, true);
        return;
    }
    if (!exists) {
        call->fail(rel + " does not exist: make it with action create.");
        return;
    }
    StringTable table;
    std::vector<std::string> problems;
    if (!stringtables::read(file, table, error, &problems)) {
        call->fail(rel + " does not load: " + error + ". Fix its JSON, or remove it and create it again.");
        return;
    }
    if (action == "get") {
        Json data = {{"entries", locTableJson(table, args.value("keys", Json::array()))}};
        if (!problems.empty()) data["problems"] = problems;
        locReply(call, project, file, table, rel + ": " + std::to_string(table.entries.size()) + " key(s).", data);
    } else if (action == "set") {
        if (!args.contains("entries")) {
            call->fail("set needs `entries`: {key: {language: text or plural forms or null, comment?: text}}.");
            return;
        }
        locSet(call, project, file, table, args["entries"], rel, false);
    } else if (action == "remove") {
        const Json keys = args.value("keys", Json::array());
        if (!keys.is_array() || keys.empty()) {
            call->fail("remove needs `keys`: the keys to remove (with `language`, only that language's values).");
            return;
        }
        const std::string language = args.value("language", std::string());
        int removed = 0;
        for (const Json& key : keys) {
            if (!key.is_string()) continue;
            if (language.empty()) {
                removed += table.erase(key.get<std::string>()) ? 1 : 0;
            } else if (StringTableEntry* entry = table.find(key.get<std::string>())) {
                removed += entry->erase(language) ? 1 : 0;
            }
        }
        if (removed && !locWrite(call, project, file, table, "Remove String Table Keys")) return;
        locReply(call, project, file, table, "Removed " + std::to_string(removed) + (language.empty() ? " key(s)" : " " + language + " value(s)") +
                 " from " + rel + (removed ? ", one undo step." : "."), Json::object());
    } else if (action == "import") {
        fs::path source;
        const std::string given = args.value("file", std::string());
        if (given.empty() || !McpRefs::resolveProjectPath(project, given, source, error, true)) {
            call->fail("import needs `file`: a CSV or XLIFF file" + (error.empty() ? std::string(".") : " (" + error + ")."));
            return;
        }
        std::string text;
        if (!ProjectChecks::readFile(source, text)) {
            call->fail(given + " cannot be read.");
            return;
        }
        stringtables::ImportReport report;
        const LocalizationSettings& settings = project->getLocalizationSettings();
        if (!stringtables::importText(table, text, args.value("format", std::string()), settings.effectiveDefault(), settings.languages, report, error)) {
            call->fail("import: " + error + ". Nothing was written.");
            return;
        }
        if ((report.added || report.changed) && !locWrite(call, project, file, table, "Import Strings into " + StringTable::nameOfPath(rel))) return;
        Json data = {{"added", report.added}, {"changed", report.changed}, {"file_languages", report.languages}, {"notes", report.notes}};
        locReply(call, project, file, table, "Imported " + given + " into " + rel + ": " + std::to_string(report.added) + " new key(s), " +
                 std::to_string(report.changed) + " changed" + (report.added || report.changed ? ", one undo step." : "."), data);
    } else if (action == "export") {
        const std::string format = locLower(args.value("format", std::string("csv")));
        const std::vector<std::string> languages = stringtables::editLanguages(project, table);
        const std::string given = args.value("file", std::string());
        std::vector<std::pair<std::string, std::string>> texts;   // (language or "", text)
        if (format == "csv") {
            texts.push_back({"", stringtables::exportCsv(table, languages)});
        } else if (format == "xliff") {
            texts = stringtables::exportXliff(table, rel, project->getLocalizationSettings().effectiveDefault(), languages);
        } else {
            call->fail("export `format` is csv or xliff.");
            return;
        }
        Json data = {{"format", format}};
        if (given.empty()) {
            Json files = Json::array();
            for (const auto& [code, text] : texts) files.push_back(code.empty() ? Json{{"text", text}} : Json{{"language", code}, {"text", text}});
            data["files"] = files;
            data["next"] = "Nothing was written: pass `file` to write it (XLIFF: one <name>.<language>.xlf per language beside it).";
            call->ok("Export of " + rel + " as " + format + " (not written).", data);
            return;
        }
        fs::path target;
        if (!McpRefs::resolveProjectWritePath(project, given, target, error)) {
            call->fail("export `file`: " + error);
            return;
        }
        std::vector<std::pair<fs::path, std::string>> files;
        for (const auto& [code, text] : texts) {
            files.push_back({code.empty() ? target : target.parent_path() / (target.stem().string() + "." + code + ".xlf"), text});
        }
        if (!stringtables::writeExport(project, files, error)) {
            call->fail("export: " + error);
            return;
        }
        Json written = Json::array();
        for (const auto& item : files) written.push_back(w4::projectRelative(project, item.first));
        data["written"] = written;
        call->ok("Exported " + rel + " to " + std::to_string(files.size()) + " file(s).", data);
    } else {
        call->fail("`action` is one of " + std::string(STRING_TABLE_ACTIONS) + ".");
    }
}

} // namespace

void registerLocalizationTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "edit_string_table";
    tool.title = "Edit String Table";
    tool.description =
        "Unity's String Tables: a .stringtable's keys with a value per project language (Lua Localization.get, a Text's "
        "localizationKey, HTML data-loc). get lists entries and the keys each language misses; set writes {key: {language: "
        "text or plural forms or null, comment}} in one undo step; import/export CSV or XLIFF for translators. "
        "jasper://docs/localization";
    tool.inputSchema = object({
        {"action", stringEnum("What to do.", {"create", "get", "set", "remove", "import", "export"})},
        {"path", assetPath("A .stringtable in the assets folder.")},
        {"entries", anyObject("create/set: {key: {language: text, {one, other...} or null, comment?: text}}.")},
        {"keys", array(string("A key."), "get: only these; remove: these keys.")},
        {"language", string("remove: only this language's values.")},
        {"format", stringEnum("import/export.", {"csv", "xliff"})},
        {"file", string("import: the file to read; export: where to write (omit to get the text).")}
    }, {"action", "path"});
    tool.annotations.idempotent = true;
    tool.activity = ToolActivity::Edit;
    tool.allowedWhilePlaying = true;
    tool.writesFiles = [](const Json& args) { return args.value("action", std::string()) != "get"; };
    tool.handler = editStringTableTool;
    registry.addTool(std::move(tool));
}

}
