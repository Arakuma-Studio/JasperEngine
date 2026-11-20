// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_string_table (docs/specs/game-ui-localization.md R33, group `localization`): lists the
// project's String Tables with their coverage and this Play's misses, reads entries, applies ops
// as one undo step (refused whole when one fails), creates a table, imports and exports CSV/XLIFF.

#include "McpTools.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "Project.h"
#include "io/AtomicFile.h"
#include "localization/Localization.h"
#include "localization/MessageFormat.h"
#include "localization/StringFiles.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableDocument.h"
#include "localization/StringTableEditCmd.h"
#include "localization/StringTableInterchange.h"
#include "util/ProjectChecks.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using l10n::StringTableSet;

std::string relativePath(Project* project, const fs::path& path) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, project->getProjectPath(), ec);
    return ec ? path.generic_string() : relative.generic_string();
}

std::map<std::string, uint32_t> missesByNamespace() {
    std::map<std::string, uint32_t> out;
    for (const Localization::Miss& miss : Localization::getMisses()) out[miss.key.substr(0, miss.key.find('.'))] += miss.count;
    return out;
}

Json coverageOf(const StringTableSet& set) {
    Json languages = Json::object();
    for (const std::string& code : set.languages()) {
        languages[code] = Json{{"coverage", std::round(set.coverage(code) * 1000.0) / 1000.0}, {"missing", set.missing(code)},
                               {"needs_review", set.stale(code)}};
    }
    return languages;
}

void listTables(const CallPtr& call, Project* project) {
    const l10n::ProjectTables tables = l10n::loadProjectTables(project->getProjectPath());
    const std::map<std::string, uint32_t> misses = missesByNamespace();
    Json list = Json::array();
    std::string text;
    for (const StringTableSet& set : tables.sets) {
        const auto miss = misses.find(set.table.ns);
        list.push_back(Json{{"path", set.ownerRelative}, {"namespace", set.table.ns}, {"source_language", set.table.sourceLanguage},
                            {"entries", set.table.entries.size()}, {"languages", coverageOf(set)},
                            {"play_misses", miss == misses.end() ? 0u : miss->second}});
        text += set.ownerRelative + " (" + set.table.ns + ", " + std::to_string(set.table.entries.size()) + " entries)\n";
    }
    Json out{{"tables", list}};
    if (!tables.errors.empty()) out["errors"] = tables.errors;
    if (tables.sets.empty()) {
        out["next"] = "No String Tables yet: edit_string_table {path: \"assets/l10n/ui.stringtable\", create: {}, ops: [{op: \"set\", key: \"menu.play\", text: \"Play\"}]}";
        text = "No String Tables.";
    }
    call->ok(text, out);
}

Json entryJson(const StringTableSet& set, const l10n::TableEntry& entry, const std::string& language) {
    Json row{{"key", entry.key}, {"text", entry.text}};
    if (!entry.context.empty()) row["context"] = entry.context;
    if (entry.maxChars > 0) row["max_chars"] = entry.maxChars;
    auto overLength = [&](const std::string& text) {
        size_t points = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) points++;
        return entry.maxChars > 0 && points > static_cast<size_t>(entry.maxChars);
    };
    if (overLength(entry.text)) row["over_length"] = true;
    Json translations = Json::object();
    for (const auto& [code, doc] : set.translations) {
        if (!language.empty() && code != language) continue;
        const auto found = doc.entries.find(entry.key);
        if (found == doc.entries.end() || found->second.text.empty()) {
            translations[code] = Json{{"status", "missing"}};
            continue;
        }
        const bool review = found->second.status == "needs_review" || found->second.sourceHash != l10n::sourceHash(entry.text);
        Json t{{"text", found->second.text}, {"status", review ? "needs_review" : "translated"}};
        if (overLength(found->second.text)) t["over_length"] = true;
        translations[code] = std::move(t);
    }
    if (!translations.empty()) row["translations"] = std::move(translations);
    return row;
}

void getEntries(const CallPtr& call, const StringTableSet& set, const Json& get) {
    std::set<std::string> keys;
    if (get.contains("keys") && get["keys"].is_array()) {
        for (const Json& key : get["keys"]) if (key.is_string()) keys.insert(key.get<std::string>());
    }
    const std::string language = get.contains("language") && get["language"].is_string() ? TitleArtLanguage::canonicalize(get["language"].get<std::string>()) : std::string();
    const bool missing = get.value("missing", false);
    const int limit = std::clamp(get.value("limit", 200), 1, 1000);
    Json entries = Json::array();
    size_t matched = 0;
    for (const l10n::TableEntry& entry : set.table.entries) {
        if (!keys.empty() && !keys.count(entry.key)) continue;
        if (missing) {
            bool lacks = false;
            for (const auto& [code, doc] : set.translations) {
                if (!language.empty() && code != language) continue;
                const auto found = doc.entries.find(entry.key);
                if (found == doc.entries.end() || found->second.text.empty()) lacks = true;
            }
            if (!lacks) continue;
        }
        matched++;
        if (entries.size() < static_cast<size_t>(limit)) entries.push_back(entryJson(set, entry, language));
    }
    Json out{{"path", set.ownerRelative}, {"namespace", set.table.ns}, {"source_language", set.table.sourceLanguage},
             {"languages", coverageOf(set)}, {"entries", entries}, {"matched", matched}};
    if (matched > entries.size()) out["next"] = "More entries match: raise get.limit or filter with get.keys.";
    call->ok(set.ownerRelative + ": " + std::to_string(entries.size()) + " of " + std::to_string(matched) + " entries", out);
}

// keys the project's HTML names in this table's namespace that the table lacks
Json missingReferences(Project* project, const StringTableSet& set) {
    Json ops = Json::array();
    for (const auto& [key, reference] : l10n::referencedKeys(project->getProjectPath())) {
        const size_t dot = key.find('.');
        if (key.substr(0, dot) != set.table.ns || set.table.find(key.substr(dot + 1))) continue;
        ops.push_back(Json{{"op", "set"}, {"key", key.substr(dot + 1)}, {"text", "..."}, {"referenced_by", reference.file + ":" + std::to_string(reference.line)}});
        if (ops.size() >= 20) break;
    }
    return ops;
}

// rows of a strings file into the set; notes say what was not imported
void editStringTable(CallPtr call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (!args.contains("path")) {
        if (args.contains("ops") || args.contains("get") || args.contains("create") || args.contains("import") || args.contains("export")) {
            call->fail("path is required with create, get, ops, import and export: a .stringtable (edit_string_table with no arguments lists them).");
            return;
        }
        listTables(call, project);
        return;
    }
    const std::string path = args.value("path", std::string());
    fs::path owner;
    std::string error;
    const bool writes = args.contains("ops") || args.contains("create") || args.contains("import");
    const bool resolved = writes ? McpRefs::resolveProjectWritePath(project, path, owner, error)
                                 : McpRefs::resolveProjectPath(project, path, owner, error, true);
    if (!resolved) {
        call->fail(error.empty() ? "path: a project .stringtable path, e.g. assets/l10n/ui.stringtable." : error);
        return;
    }
    // a dialogue owns its lines' texts (dialogue-graphs.md R26): its translations are edited here
    const bool dialogueOwner = owner.extension() == ".dialogue";
    if (owner.extension() != l10n::STRINGTABLE_EXTENSION && !dialogueOwner) {
        call->fail("path must be a .stringtable or a .dialogue (" + path + ").");
        return;
    }
    if (dialogueOwner && args.contains("create")) {
        call->fail("create makes a .stringtable; a dialogue is created with edit_graph {create: {kind: \"dialogue\"}}.");
        return;
    }
    StringTableSet before;
    StringTableSet set;
    const bool creating = args.contains("create");
    if (creating) {
        const Json& create = args["create"];
        if (!l10n::createTable(project->getProjectPath(), owner, create.value("namespace", std::string()),
                               create.value("source_language", project->getLocalizationSettings().effectiveDefault()), set, error)) {
            call->fail("create: " + error + ".");
            return;
        }
    } else {
        if (!l10n::loadSet(project->getProjectPath(), owner, set, error)) {
            call->fail(error + ".");
            return;
        }
        before = set;
    }
    if (args.contains("get") && !writes) {
        getEntries(call, set, args["get"]);
        return;
    }
    Json out{{"path", set.ownerRelative}, {"namespace", set.table.ns}};
    std::string text;
    if (args.contains("export")) {
        const Json& exportArgs = args["export"];
        const std::string format = exportArgs.value("format", std::string("csv"));
        std::vector<std::string> languages;
        for (const Json& code : exportArgs.value("languages", Json::array())) if (code.is_string()) languages.push_back(TitleArtLanguage::canonicalize(code.get<std::string>()));
        fs::path target;
        if (!McpRefs::resolveProjectWritePath(project, exportArgs.value("out", std::string()), target, error)) {
            call->fail("export.out: " + (error.empty() ? std::string("a project path to write, e.g. translations/ui.csv") : error));
            return;
        }
        const std::vector<StringsExportDoc> docs = {l10n::exportDocOf(set, languages)};
        Json written = Json::array();
        auto write = [&](const fs::path& file, const std::string& body) {
            std::error_code ec;
            fs::create_directories(file.parent_path(), ec);
            const io::WriteResult result = io::writeFileAtomic(file, body);
            if (!result.ok()) return false;
            written.push_back(relativePath(project, file));
            return true;
        };
        bool ok = true;
        if (format == "xliff") {
            for (const auto& [code, body] : exportStringsXliff(docs, languages)) {
                ok = ok && write(target.parent_path() / (target.stem().string() + "." + code + ".xlf"), body);
            }
        } else {
            ok = write(target, exportStringsCsv(docs, languages));
        }
        if (!ok) {
            call->fail("export: could not write " + target.generic_string() + ".");
            return;
        }
        out["written"] = written;
        text += "Exported " + std::to_string(written.size()) + " file(s). ";
    }
    int changes = 0;
    if (args.contains("import")) {
        const Json& importArgs = args["import"];
        fs::path file;
        if (!McpRefs::resolveProjectPath(project, importArgs.value("file", std::string()), file, error, true)) {
            call->fail("import.file: " + (error.empty() ? std::string("a project CSV or XLIFF file") : error));
            return;
        }
        std::string body;
        StringsFile strings;
        if (!ProjectChecks::readFile(file, body) || !parseStrings(body, "auto", strings, error)) {
            call->fail("import: " + (error.empty() ? relativePath(project, file) + " cannot be read" : error) + ".");
            return;
        }
        Json notes = Json::array();
        for (const StringsNote& note : l10n::importStrings(set, strings, importArgs.value("add_languages", false), changes)) {
            notes.push_back(Json{{"code", note.code}, {"key", note.key}, {"language", note.language}, {"message", note.message}, {"line", note.line}});
        }
        out["imported"] = changes;
        if (!notes.empty()) out["notes"] = notes;
        text += "Imported " + std::to_string(changes) + " translation(s). ";
    }
    if (args.contains("ops")) {
        if (!l10n::applyOps(set, args["ops"], error)) {
            call->fail(error + " Nothing was changed.");
            return;
        }
        changes += static_cast<int>(args["ops"].size());
    }
    if (creating || changes > 0) {
        const std::string label = creating ? "Create String Table " + set.table.ns
                                           : "Edit String Table " + set.table.ns + (changes > 1 ? " (" + std::to_string(changes) + " Changes)" : "");
        if (!l10n::commitSet(project, creating ? nullptr : &before, set, label, &error) && !error.empty()) {
            call->fail("Could not write the table: " + error + ".");
            return;
        }
        text += label + ". ";
        out["languages"] = coverageOf(set);
        out["entries"] = set.table.entries.size();
    }
    const Json missing = missingReferences(project, set);
    if (!missing.empty()) {
        out["missing_keys"] = missing;
        out["next"] = "HTML names keys this table lacks: add them with ops [{op: \"set\", key, text}] (missing_keys).";
    } else if (creating || changes > 0) {
        out["next"] = "Use the keys: data-l10n-id=\"" + set.table.ns + ".<key>\" in HTML, Localization.get(\"" + set.table.ns +
                      ".<key>\") in Lua; check the fit with assert_ui {languages}.";
    }
    if (text.empty()) text = set.ownerRelative + ": nothing to do (pass get, ops, import or export).";
    call->ok(text, out);
}

} // namespace

void registerLocalizationTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "edit_string_table";
    tool.title = "Edit String Table";
    tool.group = "localization";
    tool.description =
        "String Tables (.stringtable, keys ns.key). No path: lists tables, coverage per language and Play misses. get reads "
        "entries; ops (set, remove, rename, add_language, remove_language, mark_reviewed) apply as one undo step, refused whole "
        "if one fails; create makes a table; import/export CSV or XLIFF. Texts are ICU ({n, plural, one {# coin} other {# coins}}).";
    tool.inputSchema = object({
        {"path", string("")},
        {"create", object({{"namespace", string("")}, {"source_language", string("")}})},
        {"get", object({{"keys", array(string(""), "")}, {"language", string("")}, {"missing", boolean("")}, {"limit", integer("", 1, 1000)}})},
        {"ops", Json{{"type", "array"}}},
        {"import", object({{"file", string("")}, {"add_languages", boolean("")}}, {"file"})},
        {"export", object({{"format", stringEnum("", {"csv", "xliff"})}, {"languages", array(string(""), "")}, {"out", string("")}}, {"out"})},
    });
    tool.annotations.idempotent = false;
    tool.writesFiles = [](const Json& args) { return args.contains("ops") || args.contains("create") || args.contains("import") || args.contains("export"); };
    tool.allowedWhilePlaying = true;
    tool.activity = ToolActivity::Edit;
    tool.handler = editStringTable;
    registry.addTool(std::move(tool));
}

}
