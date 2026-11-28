// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTableDocument.h"

#include "dialogue/DialogueProgram.h"
#include "graph/Graph.h"
#include "localization/MessageFormat.h"
#include "localization/PluralRules.h"
#include "titleart/TitleTranslations.h"
#include "util/AssetDatabase.h"
#include "util/ProjectChecks.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::l10n {

namespace mf = jasperengine::l10n;
// the documents' JSON by its own name: a unity batch can hold another Json (gametest::Json)
namespace l10ndoc = jasperengine::editor::l10n;
namespace fs = std::filesystem;

namespace {

    bool setError(ParseError* error, const std::string& code, const std::string& message) {
        if (error) *error = {code, message};
        return false;
    }

    // the top-level checks both formats share
    bool readHeader(const std::string& text, const char* format, l10ndoc::Json& root, ParseError* error) {
        root = l10ndoc::Json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return setError(error, "not_json", "not a JSON object");
        if (!root.contains("format") || !root["format"].is_string() || root["format"].get<std::string>() != format) {
            return setError(error, "wrong_format", std::string("not a ") + format + " file");
        }
        if (!root.contains("version") || !root["version"].is_number_integer()) return setError(error, "bad_member", "`version` is missing");
        const int64_t version = root["version"].get<int64_t>();
        if (version > STRINGTABLE_VERSION) {
            return setError(error, "version_newer", "this file is version " + std::to_string(version) +
                                                   ", written by a newer editor; this editor reads up to version " +
                                                   std::to_string(STRINGTABLE_VERSION) + ": open it with an editor that reads version " +
                                                   std::to_string(version));
        }
        return true;
    }

    std::string str(const l10ndoc::Json& object, const char* key) {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string dump(const l10ndoc::Json& root) {
        return root.dump(2) + "\n";
    }

    std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string relativeTo(const fs::path& projectPath, const fs::path& path) {
        std::error_code ec;
        const fs::path relative = fs::relative(path, projectPath, ec);
        return ec ? path.generic_string() : relative.generic_string();
    }

    bool fail(std::string& error, size_t index, const std::string& op, const std::string& message) {
        error = "op " + std::to_string(index) + " (" + op + "): " + message;
        return false;
    }

    // a translation language of the set, matched case-insensitively
    TranslationDoc* findTranslation(StringTableSet& set, const std::string& language) {
        for (auto& [code, doc] : set.translations) {
            if (lower(code) == lower(language)) return &doc;
        }
        return nullptr;
    }

    bool isSource(const StringTableSet& set, const std::string& language) {
        return language.empty() || lower(language) == lower(set.table.sourceLanguage);
    }

    bool checkMessage(const std::string& text, std::string& problem) {
        mf::MessageError error;
        if (mf::MessageFormat::validate(text, &error)) return true;
        problem = "the text does not parse at column " + std::to_string(mf::MessageFormat::column(text, error.offset)) +
                  ": " + error.message;
        return false;
    }

}

const TableEntry* StringTableDoc::find(const std::string& key) const {
    for (const TableEntry& entry : entries) if (entry.key == key) return &entry;
    return nullptr;
}

TableEntry* StringTableDoc::find(const std::string& key) {
    for (TableEntry& entry : entries) if (entry.key == key) return &entry;
    return nullptr;
}

bool parseStringTable(const std::string& text, StringTableDoc& out, ParseError* error) {
    out = StringTableDoc();
    l10ndoc::Json root;
    if (!readHeader(text, STRINGTABLE_FORMAT, root, error)) return false;
    out.guid = str(root, "guid");
    out.ns = str(root, "namespace");
    out.sourceLanguage = str(root, "source_language");
    if (out.sourceLanguage.empty()) out.sourceLanguage = "en";
    if (root.contains("entries") && !root["entries"].is_array()) return setError(error, "bad_member", "`entries` is not a list");
    for (const l10ndoc::Json& row : root.value("entries", l10ndoc::Json::array())) {
        if (!row.is_object() || !row.contains("key") || !row["key"].is_string()) return setError(error, "bad_member", "an entry has no `key`");
        TableEntry entry;
        for (auto it = row.begin(); it != row.end(); ++it) {
            if (it.key() == "key" && it->is_string()) entry.key = it->get<std::string>();
            else if (it.key() == "text" && it->is_string()) entry.text = it->get<std::string>();
            else if (it.key() == "context" && it->is_string()) entry.context = it->get<std::string>();
            else if (it.key() == "max_chars" && it->is_number_integer()) entry.maxChars = static_cast<int>(it->get<int64_t>());
            else entry.extra[it.key()] = *it;
        }
        out.entries.push_back(std::move(entry));
    }
    static const std::set<std::string> known = {"format", "version", "guid", "namespace", "source_language", "entries"};
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!known.count(it.key())) out.extra[it.key()] = *it;
    }
    return true;
}

std::string saveStringTable(const StringTableDoc& doc) {
    l10ndoc::Json root;
    root["format"] = STRINGTABLE_FORMAT;
    root["version"] = STRINGTABLE_VERSION;
    root["guid"] = doc.guid;
    root["namespace"] = doc.ns;
    root["source_language"] = doc.sourceLanguage;
    l10ndoc::Json entries = l10ndoc::Json::array();
    for (const TableEntry& entry : doc.entries) {
        l10ndoc::Json row;
        row["key"] = entry.key;
        row["text"] = entry.text;
        if (!entry.context.empty()) row["context"] = entry.context;
        if (entry.maxChars > 0) row["max_chars"] = entry.maxChars;
        for (auto it = entry.extra.begin(); it != entry.extra.end(); ++it) row[it.key()] = *it;
        entries.push_back(std::move(row));
    }
    root["entries"] = std::move(entries);
    for (auto it = doc.extra.begin(); it != doc.extra.end(); ++it) root[it.key()] = *it;
    return dump(root);
}

bool parseTranslation(const std::string& text, TranslationDoc& out, ParseError* error) {
    out = TranslationDoc();
    l10ndoc::Json root;
    if (!readHeader(text, TRANSLATION_FORMAT, root, error)) return false;
    out.guid = str(root, "guid");
    out.language = str(root, "language");
    if (root.contains("owner") && root["owner"].is_object()) {
        for (auto it = root["owner"].begin(); it != root["owner"].end(); ++it) {
            if (it.key() == "guid" && it->is_string()) out.ownerGuid = it->get<std::string>();
            else if (it.key() == "path" && it->is_string()) out.ownerPath = it->get<std::string>();
            else out.ownerExtra[it.key()] = *it;
        }
    }
    if (root.contains("entries") && !root["entries"].is_object()) return setError(error, "bad_member", "`entries` is not an object");
    const l10ndoc::Json entries = root.value("entries", l10ndoc::Json::object());
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it->is_object()) return setError(error, "bad_member", "the entry `" + it.key() + "` is not an object");
        TranslationEntry entry;
        for (auto member = it->begin(); member != it->end(); ++member) {
            if (member.key() == "text" && member->is_string()) entry.text = member->get<std::string>();
            else if (member.key() == "status" && member->is_string()) entry.status = member->get<std::string>();
            else if (member.key() == "source_hash" && member->is_string()) entry.sourceHash = member->get<std::string>();
            else entry.extra[member.key()] = *member;
        }
        out.entries[it.key()] = std::move(entry);
    }
    static const std::set<std::string> known = {"format", "version", "guid", "owner", "language", "entries"};
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!known.count(it.key())) out.extra[it.key()] = *it;
    }
    return true;
}

std::string saveTranslation(const TranslationDoc& doc) {
    l10ndoc::Json root;
    root["format"] = TRANSLATION_FORMAT;
    root["version"] = STRINGTABLE_VERSION;
    root["guid"] = doc.guid;
    l10ndoc::Json owner;
    owner["guid"] = doc.ownerGuid;
    owner["path"] = doc.ownerPath;
    for (auto it = doc.ownerExtra.begin(); it != doc.ownerExtra.end(); ++it) owner[it.key()] = *it;
    root["owner"] = std::move(owner);
    root["language"] = doc.language;
    l10ndoc::Json entries = l10ndoc::Json::object();
    for (const auto& [key, entry] : doc.entries) {
        l10ndoc::Json row;
        row["text"] = entry.text;
        row["status"] = entry.status;
        row["source_hash"] = entry.sourceHash;
        for (auto it = entry.extra.begin(); it != entry.extra.end(); ++it) row[it.key()] = *it;
        entries[key] = std::move(row);
    }
    root["entries"] = std::move(entries);
    for (auto it = doc.extra.begin(); it != doc.extra.end(); ++it) root[it.key()] = *it;
    return dump(root);
}

bool isValidNamespace(const std::string& ns) {
    if (ns.empty() || ns.size() > 48) return false;
    return std::all_of(ns.begin(), ns.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool isValidKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    return std::all_of(key.begin(), key.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'; });
}

std::string namespaceFromStem(const std::string& stem) {
    std::string out;
    for (char c : stem) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) out += l;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 48) out.resize(48);
    return out.empty() ? std::string("strings") : out;
}

std::string sourceHash(const std::string& text) {
    return titleart::translationSourceHash(text);
}

std::string pluralSkeleton(const std::string& language, const std::string& argument) {
    std::string out = "{" + argument + ", plural,";
    std::string categories = mf::PluralRules::categories(language);
    size_t start = 0;
    while (start < categories.size()) {
        size_t end = categories.find(' ', start);
        if (end == std::string::npos) end = categories.size();
        out += " " + categories.substr(start, end - start) + " {# }";
        start = end + 1;
    }
    return out + "}";
}

// ---- sets

std::vector<std::string> StringTableSet::languages() const {
    std::vector<std::string> out = {table.sourceLanguage};
    for (const auto& entry : translations) out.push_back(entry.first);
    return out;
}

std::map<fs::path, std::string> StringTableSet::files() const {
    std::map<fs::path, std::string> out;
    if (!dialogue) out[owner] = saveStringTable(table);
    for (const auto& [language, doc] : translations) out[translationPathFor(owner, language)] = saveTranslation(doc);
    return out;
}

double StringTableSet::coverage(const std::string& language) const {
    if (lower(language) == lower(table.sourceLanguage) || table.entries.empty()) return 1.0;
    return 1.0 - static_cast<double>(missing(language)) / static_cast<double>(table.entries.size());
}

size_t StringTableSet::missing(const std::string& language) const {
    const auto it = translations.find(language);
    if (it == translations.end()) return lower(language) == lower(table.sourceLanguage) ? 0 : table.entries.size();
    size_t count = 0;
    for (const TableEntry& entry : table.entries) {
        const auto found = it->second.entries.find(entry.key);
        if (found == it->second.entries.end() || found->second.text.empty()) count++;
    }
    return count;
}

size_t StringTableSet::stale(const std::string& language) const {
    const auto it = translations.find(language);
    if (it == translations.end()) return 0;
    size_t count = 0;
    for (const TableEntry& entry : table.entries) {
        const auto found = it->second.entries.find(entry.key);
        if (found != it->second.entries.end() && !found->second.text.empty() &&
                (found->second.status == "needs_review" || found->second.sourceHash != sourceHash(entry.text))) count++;
    }
    return count;
}

fs::path translationPathFor(const fs::path& owner, const std::string& language) {
    return owner.parent_path() / (owner.stem().string() + "." + language + TRANSLATION_EXTENSION);
}

std::map<std::string, fs::path> translationSiblings(const fs::path& owner) {
    std::map<std::string, fs::path> out;
    std::error_code ec;
    const std::string prefix = owner.stem().string() + ".";
    for (fs::directory_iterator it(owner.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        if (path.extension() != TRANSLATION_EXTENSION) continue;
        const std::string stem = path.stem().string();
        if (stem.rfind(prefix, 0) != 0) continue;
        const std::string language = stem.substr(prefix.size());
        if (language.empty() || language.find('.') != std::string::npos || !TitleArtLanguage::isValid(language)) continue;
        out[TitleArtLanguage::canonicalize(language)] = path;
    }
    return out;
}

bool loadSet(const fs::path& projectPath, const fs::path& owner, StringTableSet& out, std::string& error) {
    out = StringTableSet();
    out.owner = owner;
    out.ownerRelative = relativeTo(projectPath, owner);
    std::string text;
    if (!ProjectChecks::readFile(owner, text)) {
        error = out.ownerRelative + " cannot be read";
        return false;
    }
    ParseError parse;
    if (owner.extension() == dialogue::FILE_EXTENSION) {
        // a dialogue's entries are its lines', variants' and options' texts, keyed by their ids
        graph::LoadReport report;
        const graph::Graph file = graph::load(text, report);
        if (!report.ok || file.kind != dialogue::KIND_ID) {
            error = out.ownerRelative + " is not a dialogue" + (report.error.empty() ? std::string() : ": " + report.error);
            return false;
        }
        out.dialogue = true;
        if (auto guid = file.passthrough.find("guid"); guid != file.passthrough.end() && guid->is_string()) out.table.guid = guid->get<std::string>();
        out.table.ns = dialogue::namespaceOf(file, owner.string());
        if (auto language = file.metadata.find("source_language"); language != file.metadata.end() && language->is_string() && !language->get<std::string>().empty()) {
            out.table.sourceLanguage = language->get<std::string>();
        }
        for (const dialogue::SourceEntry& entry : dialogue::sourceEntries(file)) out.table.entries.push_back({entry.key, entry.text, "", 0, l10ndoc::Json::object()});
    } else if (!parseStringTable(text, out.table, &parse)) {
        error = out.ownerRelative + ": " + parse.message;
        return false;
    }
    for (const auto& [language, path] : translationSiblings(owner)) {
        std::string body;
        TranslationDoc doc;
        if (!ProjectChecks::readFile(path, body) || !parseTranslation(body, doc, &parse)) {
            error = relativeTo(projectPath, path) + ": " + (body.empty() ? std::string("cannot be read") : parse.message);
            return false;
        }
        if (doc.language.empty()) doc.language = language;
        out.translations[language] = std::move(doc);
    }
    return true;
}

std::vector<fs::path> findTables(const fs::path& projectPath) {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path assets = projectPath / "assets";
    for (fs::recursive_directory_iterator it(fs::exists(assets, ec) ? assets : projectPath, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == ".jasperengine" || name == "build" || (!name.empty() && name[0] == '.'))) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() == STRINGTABLE_EXTENSION && it->is_regular_file(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fs::path> findOwners(const fs::path& projectPath) {
    std::vector<fs::path> out = findTables(projectPath);
    std::error_code ec;
    const fs::path assets = projectPath / "assets";
    for (fs::recursive_directory_iterator it(fs::exists(assets, ec) ? assets : projectPath, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == ".jasperengine" || name == "build" || (!name.empty() && name[0] == '.'))) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() == dialogue::FILE_EXTENSION && it->is_regular_file(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string namespaceOwner(const fs::path& projectPath, const std::string& ns, const fs::path& except) {
    for (const fs::path& owner : findOwners(projectPath)) {
        if (!except.empty() && owner == except) continue;
        if (owner.extension() == dialogue::FILE_EXTENSION) {
            std::string text;
            if (!ProjectChecks::readFile(owner, text)) continue;
            graph::LoadReport report;
            const graph::Graph file = graph::load(text, report);
            if (report.ok && file.kind == dialogue::KIND_ID && dialogue::namespaceOf(file, owner.string()) == ns) return relativeTo(projectPath, owner);
            continue;
        }
        std::string text;
        StringTableDoc doc;
        if (ProjectChecks::readFile(owner, text) && parseStringTable(text, doc) && doc.ns == ns) return relativeTo(projectPath, owner);
    }
    // a Title Art document's string_namespace (S87 D3: one key space)
    std::error_code ec;
    const fs::path assets = projectPath / "assets";
    for (fs::recursive_directory_iterator it(fs::exists(assets, ec) ? assets : projectPath, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".titleart" || !it->is_regular_file(ec)) continue;
        std::string text;
        if (!ProjectChecks::readFile(it->path(), text)) continue;
        const l10ndoc::Json root = l10ndoc::Json::parse(text, nullptr, false);
        if (root.is_object() && str(root, "string_namespace") == ns) return relativeTo(projectPath, it->path());
    }
    return {};
}

bool createTable(const fs::path& projectPath, const fs::path& owner, const std::string& nsIn, const std::string& sourceLanguage,
                       StringTableSet& out, std::string& error) {
    const std::string ns = nsIn.empty() ? namespaceFromStem(owner.stem().string()) : nsIn;
    if (!isValidNamespace(ns)) {
        error = "the namespace \"" + ns + "\" is not [a-z0-9_]{1,48}";
        return false;
    }
    if (!sourceLanguage.empty() && !TitleArtLanguage::isValid(sourceLanguage, &error)) return false;
    std::error_code ec;
    if (fs::exists(owner, ec)) {
        error = relativeTo(projectPath, owner) + " exists";
        return false;
    }
    const std::string taken = namespaceOwner(projectPath, ns);
    if (!taken.empty()) {
        error = "l10n-namespace-taken: the namespace \"" + ns + "\" belongs to " + taken + "; pick another namespace";
        return false;
    }
    out = StringTableSet();
    out.owner = owner;
    out.ownerRelative = relativeTo(projectPath, owner);
    out.table.guid = AssetDatabase::generateGuid();
    out.table.ns = ns;
    out.table.sourceLanguage = sourceLanguage.empty() ? "en" : TitleArtLanguage::canonicalize(sourceLanguage);
    return true;
}

bool applyOps(StringTableSet& set, const l10ndoc::Json& ops, std::string& error, std::vector<std::string>* notes) {
    if (!ops.is_array()) {
        error = "ops is a list of {op, ...}";
        return false;
    }
    StringTableSet work = set;
    size_t index = 0;
    for (const l10ndoc::Json& op : ops) {
        const std::string name = op.is_object() ? str(op, "op") : std::string();
        const std::string key = op.is_object() ? str(op, "key") : std::string();
        const std::string language = op.is_object() && !str(op, "language").empty() ? TitleArtLanguage::canonicalize(str(op, "language")) : std::string();
        // a dialogue's source texts are its nodes' params (dialogue-graphs.md R3, D5)
        if (work.dialogue && (name == "remove" || name == "rename" || (name == "set" && isSource(work, language)))) {
            return fail(error, index, name, "a dialogue's source texts and keys are its nodes': edit the text with edit_graph setParam "
                                            "(line text, variants, options); translations take a language");
        }
        if (name == "set") {
            if (!op.contains("text") || !op["text"].is_string()) return fail(error, index, name, "needs text");
            const std::string text = op["text"].get<std::string>();
            std::string problem;
            if (!text.empty() && !checkMessage(text, problem)) return fail(error, index, name, problem + " (ICU: {name}, {n, plural, one {...} other {...}})");
            if (isSource(work, language)) {
                if (!isValidKey(key)) return fail(error, index, name, "the key \"" + key + "\" is not [a-z0-9_.-]{1,64}");
                TableEntry* entry = work.table.find(key);
                if (!entry) {
                    work.table.entries.push_back({key, text, "", 0, l10ndoc::Json::object()});
                    entry = &work.table.entries.back();
                }
                if (entry->text != text) {
                    entry->text = text;
                    const std::string hash = sourceHash(text);
                    for (auto& [code, doc] : work.translations) {
                        auto found = doc.entries.find(key);
                        if (found != doc.entries.end() && found->second.sourceHash != hash) found->second.status = "needs_review";
                    }
                }
                if (op.contains("context") && op["context"].is_string()) entry->context = op["context"].get<std::string>();
                if (op.contains("max_chars") && op["max_chars"].is_number_integer()) entry->maxChars = std::max(0, op["max_chars"].get<int>());
            } else {
                if (!TitleArtLanguage::isValid(language, &problem)) return fail(error, index, name, problem);
                const TableEntry* entry = work.table.find(key);
                if (!entry) return fail(error, index, name, "no entry \"" + key + "\": set it in the source language first");
                TranslationDoc* doc = findTranslation(work, language);
                if (!doc) return fail(error, index, name, "the table has no " + language + " translation: add_language first");
                if (op.contains("context") || op.contains("max_chars")) return fail(error, index, name, "context and max_chars belong to the source language");
                if (text.empty()) {
                    doc->entries.erase(key);
                } else {
                    TranslationEntry& translation = doc->entries[key];
                    translation.text = text;
                    translation.status = "translated";
                    translation.sourceHash = sourceHash(entry->text);
                }
            }
        } else if (name == "remove") {
            auto it = std::find_if(work.table.entries.begin(), work.table.entries.end(), [&](const TableEntry& e) { return e.key == key; });
            if (it == work.table.entries.end()) return fail(error, index, name, "no entry \"" + key + "\"");
            work.table.entries.erase(it);
            for (auto& [code, doc] : work.translations) doc.entries.erase(key);
        } else if (name == "rename") {
            const std::string to = str(op, "to");
            TableEntry* entry = work.table.find(key);
            if (!entry) return fail(error, index, name, "no entry \"" + key + "\"");
            if (!isValidKey(to)) return fail(error, index, name, "the key \"" + to + "\" is not [a-z0-9_.-]{1,64}");
            if (work.table.find(to)) return fail(error, index, name, "\"" + to + "\" exists: remove it first or pick another key");
            entry->key = to;
            for (auto& [code, doc] : work.translations) {
                auto found = doc.entries.find(key);
                if (found == doc.entries.end()) continue;
                TranslationEntry moved = std::move(found->second);
                doc.entries.erase(found);
                doc.entries[to] = std::move(moved);
            }
        } else if (name == "add_language") {
            std::string problem;
            if (!TitleArtLanguage::isValid(language, &problem)) return fail(error, index, name, language.empty() ? "needs language" : problem);
            if (isSource(work, language)) return fail(error, index, name, language + " is the source language");
            if (findTranslation(work, language)) return fail(error, index, name, "the table has " + language + " already");
            TranslationDoc doc;
            doc.guid = AssetDatabase::generateGuid();
            doc.ownerGuid = work.table.guid;
            doc.ownerPath = work.ownerRelative;
            doc.language = language;
            work.translations[language] = std::move(doc);
        } else if (name == "remove_language") {
            auto it = std::find_if(work.translations.begin(), work.translations.end(),
                                   [&](const auto& entry) { return lower(entry.first) == lower(language); });
            if (it == work.translations.end()) return fail(error, index, name, "the table has no " + language + " translation");
            work.translations.erase(it);
        } else if (name == "mark_reviewed") {
            TranslationDoc* doc = findTranslation(work, language);
            if (!doc) return fail(error, index, name, "the table has no " + language + " translation");
            int marked = 0;
            for (auto& [entryKey, translation] : doc->entries) {
                if (!key.empty() && entryKey != key) continue;
                const TableEntry* entry = work.table.find(entryKey);
                if (!entry) continue;
                translation.status = "translated";
                translation.sourceHash = sourceHash(entry->text);
                marked++;
            }
            if (!key.empty() && marked == 0) return fail(error, index, name, "no " + language + " translation of \"" + key + "\"");
        } else {
            return fail(error, index, name.empty() ? std::string("?") : name,
                        "op is set, remove, rename, add_language, remove_language or mark_reviewed");
        }
        index++;
    }
    set = std::move(work);
    (void)notes;
    return true;
}

// ---- references

namespace {

    void scanFile(const fs::path& projectPath, const fs::path& path, const std::string& kind,
                  const std::function<void(const std::string& key, int line)>& visit) {
        std::string text;
        if (!ProjectChecks::readFile(path, text)) return;
        // "ns.key", or a key alone for the Default Table (D-15)
        static const std::regex html(R"re(data-l10n-(?:id|attr-[a-z-]+)\s*=\s*["']([a-z0-9_][a-z0-9_.-]*)["'])re");
        static const std::regex scene(R"re(localizationKey:\s*["']?([a-z0-9_][a-z0-9_.-]*))re");
        static const std::regex script(R"re(["']([a-z0-9_]+\.[a-z0-9_.-]+)["'])re");
        // the Lua calls that take a key: Localization.get/has, setLocalized, localizationKey =
        static const std::regex scriptCall(
            R"re((?:Localization\s*\.\s*(?:get|has)\s*\(|:\s*setLocalized\s*\(|\.\s*localizationKey\s*=)\s*["']([a-z0-9_][a-z0-9_.-]*)["'])re");
        const std::regex& pattern = kind == "html" ? html : kind == "scene" ? scene : kind == "script-call" ? scriptCall : script;
        int line = 1;
        size_t lineStart = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            const size_t at = static_cast<size_t>(it->position(0));
            line += static_cast<int>(std::count(text.begin() + static_cast<long>(lineStart), text.begin() + static_cast<long>(at), '\n'));
            lineStart = at;
            visit((*it)[1].str(), line);
        }
        (void)projectPath;
    }

    template<typename Visit>
    void eachReferencingFile(const fs::path& projectPath, Visit visit) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(projectPath, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (it->is_directory(ec)) {
                if (name == "build" || (!name.empty() && name[0] == '.')) it.disable_recursion_pending();
                continue;
            }
            const std::string extension = lower(it->path().extension().string());
            if (extension == ".html" || extension == ".htm" || extension == ".rml") visit(it->path(), std::string("html"));
            else if (extension == ".scene" || extension == ".prefab") visit(it->path(), std::string("scene"));
            else if (extension == ".lua") visit(it->path(), std::string("script"));
        }
    }

}

std::vector<KeyReference> findReferences(const fs::path& projectPath, const std::string& key) {
    std::vector<KeyReference> out;
    eachReferencingFile(projectPath, [&](const fs::path& path, const std::string& kind) {
        scanFile(projectPath, path, kind, [&](const std::string& found, int line) {
            if (found == key) out.push_back({relativeTo(projectPath, path), line, kind});
        });
    });
    return out;
}

std::map<std::string, KeyReference> referencedKeys(const fs::path& projectPath, bool scriptCalls) {
    std::map<std::string, KeyReference> out;
    eachReferencingFile(projectPath, [&](const fs::path& path, const std::string& kind) {
        // quoted strings in Lua are not all keys: only those the key-taking calls are given
        if (kind == "script" && !scriptCalls) return;
        scanFile(projectPath, path, kind == "script" ? std::string("script-call") : kind, [&](const std::string& found, int line) {
            out.emplace(found, KeyReference{relativeTo(projectPath, path), line, kind});
        });
    });
    return out;
}

}  // namespace jasperengine::editor::l10n
