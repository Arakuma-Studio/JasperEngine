// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTableInterchange.h"

#include "localization/MessageFormat.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::l10n {

namespace fs = std::filesystem;

std::vector<StringsNote> importStrings(StringTableSet& set, const StringsFile& file, bool addLanguages, int& changed) {
    std::vector<StringsNote> notes;
    notes.insert(notes.end(), file.notes.begin(), file.notes.end());
    for (const StringRow& row : file.rows) {
        if (!row.file.empty() && fs::path(row.file).lexically_normal() != fs::path(set.ownerRelative).lexically_normal()) continue;
        const size_t dot = row.key.find('.');
        const std::string ns = dot == std::string::npos ? std::string() : row.key.substr(0, dot);
        if (row.file.empty() && ns != set.table.ns) {
            notes.push_back({"unknown_key", row.key, "", "not this table's namespace; a title's row: import it with edit_title import_strings", row.line});
            continue;
        }
        const std::string key = dot == std::string::npos ? row.key : row.key.substr(dot + 1);
        const TableEntry* entry = set.table.find(key);
        if (!entry) {
            notes.push_back({"unknown_key", row.key, "", "no such entry: set it in the source language first", row.line});
            continue;
        }
        const bool sourceChanged = !row.source.empty() && row.source != entry->text;
        if (sourceChanged) {
            notes.push_back({"source_changed", row.key, "", "the source text differs: imported as needs_review", row.line});
        }
        for (const auto& [code, text] : row.targets) {
            if (text.empty()) continue;
            if (!set.translations.count(code)) {
                if (!addLanguages) {
                    notes.push_back({"unknown_language", row.key, code, "the table has no " + code + ": pass add_languages: true", row.line});
                    continue;
                }
                std::string error;
                if (!applyOps(set, Json::array({Json{{"op", "add_language"}, {"language", code}}}), error)) continue;
            }
            jasperengine::l10n::MessageError problem;
            if (!jasperengine::l10n::MessageFormat::validate(text, &problem)) {
                notes.push_back({"bad_message", row.key, code, problem.message, row.line});
                continue;
            }
            TranslationEntry& translation = set.translations[code].entries[key];
            const auto state = row.states.find(code);
            const std::string status = sourceChanged || (state != row.states.end() && state->second == "needs_review") ? "needs_review" : "translated";
            if (translation.text == text && translation.status == status) continue;
            translation.text = text;
            translation.status = status;
            translation.sourceHash = sourceHash(entry->text);
            changed++;
        }
    }
    return notes;
}

StringsExportDoc exportDocOf(const StringTableSet& set, const std::vector<std::string>& languages) {
    StringsExportDoc doc;
    doc.file = set.ownerRelative;
    doc.sourceLanguage = set.table.sourceLanguage;
    for (const auto& entry : set.translations) {
        if (languages.empty() || std::find(languages.begin(), languages.end(), entry.first) != languages.end()) doc.languages.push_back(entry.first);
    }
    for (const TableEntry& entry : set.table.entries) {
        StringsExportUnit unit;
        unit.key = set.table.ns + "." + entry.key;
        unit.context = entry.context;
        unit.source = entry.text;
        unit.maxChars = entry.maxChars;
        for (const auto& [code, translation] : set.translations) {
            const auto found = translation.entries.find(entry.key);
            if (found == translation.entries.end() || found->second.text.empty()) continue;
            unit.targets[code] = {found->second.text, found->second.status == "needs_review" || found->second.sourceHash != sourceHash(entry.text)};
        }
        doc.units.push_back(std::move(unit));
    }
    return doc;
}


namespace {

    // a key from a text: "Options" -> "text.options", "New Game!" -> "text.new_game"
    std::string keyFromText(const std::string& text, const StringTableSet& set) {
        std::string slug;
        for (unsigned char c : text) {
            if (std::isalnum(c)) slug += static_cast<char>(std::tolower(c));
            else if (!slug.empty() && slug.back() != '_') slug += '_';
            if (slug.size() >= 40) break;
        }
        while (!slug.empty() && slug.back() == '_') slug.pop_back();
        if (slug.empty()) slug = "text";
        std::string key = "text." + slug;
        for (int n = 2; set.table.find(key); n++) key = "text." + slug + "_" + std::to_string(n);
        return key;
    }

}

bool extractText(StringTableSet& set, std::string& html, const std::string& text, std::string& key, std::string& error) {
    std::string needle = text;
    while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.back()))) needle.pop_back();
    while (!needle.empty() && std::isspace(static_cast<unsigned char>(needle.front()))) needle.erase(0, 1);
    if (needle.empty()) {
        error = "no text to extract";
        return false;
    }
    // the element holding the text: `>text<`, its start tag the last `<` before it
    size_t at = std::string::npos;
    for (size_t from = html.find(needle); from != std::string::npos; from = html.find(needle, from + 1)) {
        size_t left = from;
        while (left > 0 && std::isspace(static_cast<unsigned char>(html[left - 1]))) left--;
        size_t right = from + needle.size();
        while (right < html.size() && std::isspace(static_cast<unsigned char>(html[right]))) right++;
        if (left > 0 && html[left - 1] == '>' && right < html.size() && html[right] == '<') {
            at = left - 1;
            break;
        }
    }
    const size_t open = at == std::string::npos ? std::string::npos : html.rfind('<', at);
    if (open == std::string::npos || html.compare(open, 2, "</") == 0 || html.compare(open, 2, "<!") == 0) {
        error = "\"" + needle + "\" is not the whole text of an element";
        return false;
    }
    if (html.substr(open, at - open).find("data-l10n-id") != std::string::npos) {
        error = "the element already has a data-l10n-id";
        return false;
    }
    const std::string local = keyFromText(needle, set);
    if (!applyOps(set, Json::array({{{"op", "set"}, {"key", local}, {"text", needle}}}), error)) return false;
    key = set.table.ns + "." + local;
    const size_t insertAt = at > open && html[at - 1] == '/' ? at - 1 : at;
    html.insert(insertAt, " data-l10n-id=\"" + key + "\"");
    return true;
}

}  // namespace jasperengine::editor::l10n
