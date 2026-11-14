// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTables.h"

#include "Project.h"
#include "titleart/localisation/TitleLanguageNames.h"
#include "titleart/localisation/TitleStrings.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::string stLower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool stHasLanguage(const std::vector<std::string>& list, const std::string& code) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& l) { return stLower(l) == stLower(code); });
    }

    // a cell's value: an ICU plural or the text
    bool stSetCell(StringTableEntry& entry, const std::string& language, const std::string& cell) {
        if (cell.empty()) return false;
        const StringTableValue value = StringTableValue::fromMessageFormat(cell);
        const StringTableValue* current = entry.find(language);
        if (current && *current == value) return false;
        entry.set(language, value);
        return true;
    }

}

bool stringtables::read(const fs::path& file, StringTable& out, std::string& error, std::vector<std::string>* problems) {
    std::string text;
    if (!ProjectChecks::readFile(file, text)) {
        error = "cannot be read";
        return false;
    }
    return StringTable::parse(text, out, &error, problems);
}

std::vector<stringtables::TableFile> stringtables::readAll(const fs::path& projectRoot, const fs::path& assetsRoot) {
    std::vector<TableFile> out;
    for (const fs::path& file : ProjectChecks::findProjectFiles(assetsRoot, {StringTable::EXTENSION})) {
        TableFile table;
        table.path = file;
        table.relative = ProjectChecks::projectRelative(projectRoot, file);
        table.name = StringTable::nameOfPath(file.filename().string());
        read(file, table.table, table.error, &table.problems);
        out.push_back(std::move(table));
    }
    return out;
}

std::map<std::string, std::vector<std::string>> stringtables::duplicateKeys(const std::vector<TableFile>& tables) {
    std::map<std::string, std::vector<std::string>> owners;
    for (const TableFile& table : tables) {
        for (const StringTableEntry& entry : table.table.entries) owners[entry.key].push_back(table.name);
    }
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [key, names] : owners) {
        if (names.size() > 1) out[key] = names;
    }
    return out;
}

std::vector<ProjectChecks::Finding> stringtables::check(const fs::path& projectRoot, const fs::path& assetsRoot) {
    std::vector<ProjectChecks::Finding> findings;
    const std::vector<TableFile> tables = readAll(projectRoot, assetsRoot);
    auto add = [&](const std::string& path, const std::string& severity, const std::string& message, const std::string& suggestion) {
        ProjectChecks::Finding f;
        f.tool = "strings";
        f.path = path;
        f.severity = severity;
        f.message = message;
        f.suggestion = suggestion;
        findings.push_back(std::move(f));
    };
    std::map<std::string, std::string> pathOf;
    for (const TableFile& table : tables) {
        pathOf[table.name] = table.relative;
        if (!table.error.empty()) add(table.relative, "error", "the string table does not load: " + table.error, "");
        for (const std::string& problem : table.problems) add(table.relative, "warning", problem, "");
    }
    for (const auto& [key, names] : duplicateKeys(tables)) {
        std::string list;
        for (size_t i = 0; i < names.size(); i++) list += (i ? ", " : "") + names[i];
        add(pathOf[names.back()], "error", "the key '" + key + "' is defined in " + list + ": Localization.get(\"" + key + "\") finds " + names.front() + "'s",
            "rename one, or look it up as \"" + names.back() + ":" + key + "\"");
    }
    return findings;
}

std::vector<PluralCategory> stringtables::requiredForms(const std::string& language) {
    // a thousand selections a language: once (the window asks per cell and frame)
    static std::mutex mutex;
    static std::map<std::string, std::vector<PluralCategory>> known;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = known.find(stLower(language));
    if (found != known.end()) return found->second;
    std::vector<PluralCategory>& out = known[stLower(language)];
    auto add = [&](double count) {
        const PluralCategory category = PluralRules::select(language, count);
        if (std::find(out.begin(), out.end(), category) == out.end()) out.push_back(category);
    };
    for (int n = 0; n <= 1000; n++) add(n);
    for (double n : {0.5, 1.5, 2.5}) add(n);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> stringtables::missingKeys(const StringTable& table, const std::string& language) {
    std::vector<std::string> out;
    const std::vector<PluralCategory> used = requiredForms(language);
    for (const StringTableEntry& entry : table.entries) {
        const StringTableValue* value = entry.find(language);
        bool missing = !value || value->empty();
        if (!missing && value->plural) {
            for (PluralCategory category : used) {
                if (!value->has(category)) missing = true;
            }
        }
        if (missing) out.push_back(entry.key);
    }
    return out;
}

std::vector<std::string> stringtables::editLanguages(const Project* project, const StringTable& table) {
    if (project && !project->getLocalizationSettings().languages.empty()) return project->getLocalizationSettings().languages;
    std::vector<std::string> out;
    const std::string fallback = project ? project->getLocalizationSettings().effectiveDefault() : std::string("en");
    out.push_back(fallback);
    for (const std::string& code : table.languages()) {
        if (!stHasLanguage(out, code)) out.push_back(code);
    }
    return out;
}

std::string stringtables::exportCsv(const StringTable& table, const std::vector<std::string>& languages) {
    std::string out = "\xEF\xBB\xBF";
    std::vector<std::string> header = {"key", "context"};
    for (const std::string& code : languages) header.push_back(titleLanguageLabel(code));
    for (size_t i = 0; i < header.size(); i++) out += (i ? "," : "") + titleStringsCsvField(header[i]);
    out += "\r\n";
    for (const StringTableEntry& entry : table.entries) {
        std::vector<std::string> row = {entry.key, entry.comment};
        for (const std::string& code : languages) {
            const StringTableValue* value = entry.find(code);
            row.push_back(value ? value->toMessageFormat() : std::string());
        }
        for (size_t i = 0; i < row.size(); i++) out += (i ? "," : "") + titleStringsCsvField(row[i]);
        out += "\r\n";
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> stringtables::exportXliff(const StringTable& table, const std::string& original,
                                                                           const std::string& sourceLanguage, const std::vector<std::string>& targets) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& code : targets) {
        if (stLower(code) == stLower(sourceLanguage)) continue;
        std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xliff version=\"1.2\" xmlns=\"urn:oasis:names:tc:xliff:document:1.2\">\n";
        x += "  <file original=\"" + titleStringsXmlEscape(original, true) + "\" source-language=\"" + titleStringsXmlEscape(sourceLanguage, true) +
             "\" target-language=\"" + titleStringsXmlEscape(code, true) + "\" datatype=\"plaintext\">\n    <body>\n";
        for (const StringTableEntry& entry : table.entries) {
            const StringTableValue* source = entry.find(sourceLanguage);
            const StringTableValue* target = entry.find(code);
            x += "      <trans-unit id=\"" + titleStringsXmlEscape(entry.key, true) + "\" resname=\"" + titleStringsXmlEscape(entry.key, true) +
                 "\" xml:space=\"preserve\">\n        <source>" + titleStringsXmlEscape(source ? source->toMessageFormat() : std::string(), false) + "</source>";
            if (target && !target->empty()) x += "<target state=\"translated\">" + titleStringsXmlEscape(target->toMessageFormat(), false) + "</target>";
            x += "\n";
            if (!entry.comment.empty()) x += "        <note>" + titleStringsXmlEscape(entry.comment, false) + "</note>\n";
            x += "      </trans-unit>\n";
        }
        x += "    </body>\n  </file>\n</xliff>\n";
        out.emplace_back(code, std::move(x));
    }
    return out;
}

bool stringtables::importText(StringTable& table, const std::string& text, const std::string& format, const std::string& sourceLanguage,
                              const std::vector<std::string>& allowed, ImportReport& report, std::string& error) {
    report = ImportReport();
    TitleStringsFile file;
    if (!parseTitleStrings(text, format, file, error)) return false;
    for (const TitleStringsNote& note : file.notes) {
        report.notes.push_back((note.line ? "line " + std::to_string(note.line) + ": " : std::string()) + note.message);
    }
    std::set<std::string> refused;
    auto accept = [&](const std::string& code) {
        if (allowed.empty() || stHasLanguage(allowed, code)) {
            if (!stHasLanguage(report.languages, code)) report.languages.push_back(code);
            return true;
        }
        if (refused.insert(code).second) report.notes.push_back("'" + code + "' is not one of the project's languages (Project Settings > Localization): its column is left out");
        return false;
    };
    for (const TitleStringRow& row : file.rows) {
        const std::string why = StringTable::keyProblem(row.key);
        if (!why.empty()) {
            report.notes.push_back((row.line ? "line " + std::to_string(row.line) + ": " : std::string()) + why + " (left out)");
            continue;
        }
        StringTableEntry* entry = table.find(row.key);
        bool changed = false;
        const bool isNew = !entry;
        if (isNew) {
            table.entries.push_back(StringTableEntry());
            entry = &table.entries.back();
            entry->key = row.key;
            report.added++;
        }
        if (!row.context.empty() && entry->comment.empty()) {
            entry->comment = row.context;
            changed = true;
        }
        const std::string source = !row.sourceLanguage.empty() ? row.sourceLanguage : sourceLanguage;
        if (!source.empty() && !row.source.empty() && accept(source)) changed = stSetCell(*entry, TitleArtLanguage::canonicalize(source), row.source) || changed;
        for (const auto& [code, cell] : row.targets) {
            if (accept(code)) changed = stSetCell(*entry, TitleArtLanguage::canonicalize(code), cell) || changed;
        }
        if (changed && !isNew) report.changed++;
    }
    return true;
}

bool stringtables::write(Project* project, const fs::path& file, const StringTable& table, const std::string& stepName, std::string& error) {
    return ProjectFileWrites::writeUndoable(project, {{file, table.write()}}, error, nullptr, nullptr, {}, false, stepName);
}

fs::path stringtables::createNew(Project* project, const fs::path& directory, std::string& error) {
    fs::path file;
    std::error_code ec;
    for (int n = 1;; n++) {
        file = directory / (n == 1 ? std::string("New String Table") + StringTable::EXTENSION
                                   : "New String Table " + std::to_string(n) + StringTable::EXTENSION);
        if (!fs::exists(file, ec)) break;
    }
    if (!write(project, file, StringTable(), "Create String Table", error)) return {};
    return file;
}

bool stringtables::writeExport(Project* project, const std::vector<std::pair<fs::path, std::string>>& files, std::string& error) {
    std::vector<WriteFilesCmd::Item> inside;
    for (const auto& [path, text] : files) {
        const fs::path relative = project ? path.lexically_normal().lexically_relative(project->getProjectPath().lexically_normal()) : fs::path();
        if (!relative.empty() && *relative.begin() != "..") {
            inside.push_back({path, text});
        } else if (!editor::FileUtils::writeFileReplacing(path, text)) {
            error = "cannot write " + editor::FileUtils::pathToUtf8(path);
            return false;
        }
    }
    return inside.empty() || ProjectFileWrites::writeUndoable(project, std::move(inside), error, nullptr, nullptr, {}, false, "Export String Table");
}
