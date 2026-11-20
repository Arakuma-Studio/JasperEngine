// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTableCook.h"

#include "Project.h"
#include "io/AtomicFile.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableRuntime.h"
#include "titleart/localisation/TitleLanguageNames.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace rt = jasperengine::l10n;

namespace {

    std::string lowerCase(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool listed(const std::vector<std::string>& list, const std::string& code) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& l) { return lowerCase(l) == lowerCase(code); });
    }

    std::string endonym(const std::string& code) {
        for (const TitleLanguageName& name : titleLanguageNames()) {
            if (lowerCase(name.code) == lowerCase(code)) return name.native;
        }
        const std::string language = code.substr(0, code.find('-'));
        for (const TitleLanguageName& name : titleLanguageNames()) {
            if (lowerCase(name.code) == lowerCase(language)) return name.native;
        }
        return code;
    }

    void addNames(LocalizationManifest& manifest) {
        for (const std::string& code : manifest.languages) manifest.names[code] = endonym(code);
    }

}

editor::l10n::ProjectTables editor::l10n::loadProjectTables(const fs::path& projectPath) {
    ProjectTables out;
    for (const fs::path& owner : findOwners(projectPath)) {
        StringTableSet set;
        std::string error;
        if (loadSet(projectPath, owner, set, error)) out.sets.push_back(std::move(set));
        else out.errors.push_back(error);
    }
    return out;
}

std::vector<std::string> editor::l10n::shippedLanguages(const LocalizationSettings& settings, const ProjectTables& tables) {
    std::vector<std::string> out;
    const std::string defaultLanguage = settings.effectiveDefault();
    out.push_back(defaultLanguage);
    if (!settings.languages.empty()) {
        for (const std::string& code : settings.languages) if (!listed(out, code)) out.push_back(code);
        return out;
    }
    for (const StringTableSet& set : tables.sets) {
        for (const std::string& code : set.languages()) if (!listed(out, code)) out.push_back(code);
    }
    return out;
}

LocalizationManifest editor::l10n::editorManifest(const LocalizationSettings& settings, const ProjectTables& tables) {
    LocalizationManifest manifest;
    manifest.defaultLanguage = settings.effectiveDefault();
    manifest.rememberChoice = settings.rememberChoice;
    manifest.languages = shippedLanguages(settings, tables);
    manifest.fonts = settings.fonts;
    addNames(manifest);
    for (const StringTableSet& set : tables.sets) {
        LocalizationNamespace ns;
        ns.name = set.table.ns;
        ns.sourceLanguage = set.table.sourceLanguage;
        ns.files[set.table.sourceLanguage] = set.owner.generic_string();
        for (const auto& [code, doc] : set.translations) ns.files[code] = translationPathFor(set.owner, code).generic_string();
        manifest.namespaces.push_back(std::move(ns));
    }
    return manifest;
}

editor::l10n::CookResult editor::l10n::cookLocalization(const fs::path& projectPath, const LocalizationSettings& settings, const fs::path& assetsOut,
                                                        uint64_t seed) {
    CookResult result;
    const ProjectTables tables = loadProjectTables(projectPath);
    for (const std::string& error : tables.errors) result.warnings.push_back("W-L10N-BAD-TABLE: " + error + " (left out)");
    if (tables.sets.empty()) return result;     // R27: no manifest, no l10n/ folder

    LocalizationManifest manifest;
    manifest.defaultLanguage = settings.effectiveDefault();
    manifest.rememberChoice = settings.rememberChoice;
    manifest.languages = shippedLanguages(settings, tables);
    addNames(manifest);
    // the fonts of shipped languages, as the game finds them (assets-relative)
    for (const auto& [code, list] : settings.fonts) if (listed(manifest.languages, code)) manifest.fonts[code] = list;

    std::set<std::string> knownKeys;
    std::map<std::string, size_t> untranslated;
    std::map<std::string, size_t> stale;
    for (const StringTableSet& set : tables.sets) {
        LocalizationNamespace ns;
        ns.name = set.table.ns;
        ns.sourceLanguage = set.table.sourceLanguage;
        std::vector<rt::StringTable::Entry> source;
        for (const TableEntry& entry : set.table.entries) {
            knownKeys.insert(set.table.ns + "." + entry.key);
            source.push_back({entry.key, entry.text, false});
            rt::MessageError problem;
            if (!rt::MessageFormat::validate(entry.text, &problem)) {
                result.warnings.push_back("W-L10N-BAD-MESSAGE: " + set.ownerRelative + " " + entry.key + " (" + set.table.sourceLanguage +
                                          "): " + problem.message + "; the game shows it as written");
            }
        }
        std::vector<std::pair<std::string, std::vector<rt::StringTable::Entry>>> languages;
        languages.emplace_back(set.table.sourceLanguage, std::move(source));
        for (const auto& [code, doc] : set.translations) {
            if (!listed(manifest.languages, code)) {
                result.warnings.push_back("W-L10N-LANGUAGE-DROPPED: " + translationPathFor(set.owner, code).filename().string() +
                                          " is not in localization.languages and is left out");
                continue;
            }
            std::vector<rt::StringTable::Entry> entries;
            for (const TableEntry& entry : set.table.entries) {
                const auto found = doc.entries.find(entry.key);
                if (found == doc.entries.end() || found->second.text.empty()) {
                    untranslated[code]++;
                    continue;
                }
                const bool review = found->second.status == "needs_review" || found->second.sourceHash != sourceHash(entry.text);
                if (review) stale[code]++;
                rt::MessageError problem;
                if (!rt::MessageFormat::validate(found->second.text, &problem)) {
                    result.warnings.push_back("W-L10N-BAD-MESSAGE: " + set.ownerRelative + " " + entry.key + " (" + code + "): " +
                                              problem.message + "; the game shows it as written");
                }
                entries.push_back({entry.key, found->second.text, review});
            }
            languages.emplace_back(code, std::move(entries));
        }
        // a shipped language this table has no translation of: every key falls back
        for (const std::string& code : manifest.languages) {
            if (lowerCase(code) == lowerCase(set.table.sourceLanguage) || set.translations.count(code)) continue;
            if (TitleArtLanguage::match(code, {set.table.sourceLanguage}).empty()) untranslated[code] += set.table.entries.size();
        }
        for (const auto& [code, entries] : languages) {
            std::string bytes;
            std::string error;
            if (!rt::StringTable::cook(code, entries, bytes, &error, seed)) {
                result.ok = false;
                result.error = "Cannot export the String Table " + set.ownerRelative + " (" + code + "): " + error;
                return result;
            }
            const std::string relative = "l10n/" + set.table.ns + "." + code + ".strbin";
            const io::WriteResult written = io::writeFileAtomic(assetsOut / relative, bytes);
            if (!written.ok()) {
                result.ok = false;
                result.error = "Cannot write " + relative + ": " + written.message;
                return result;
            }
            result.written.push_back(relative);
            result.bytes += bytes.size();
            ns.files[code] = relative;
        }
        manifest.namespaces.push_back(std::move(ns));
    }
    for (const auto& [code, count] : untranslated) {
        if (count) result.warnings.push_back("W-L10N-UNTRANSLATED: " + code + ": " + std::to_string(count) + " entries show their fallback text");
    }
    for (const auto& [code, count] : stale) {
        if (count) result.warnings.push_back("W-L10N-STALE: " + code + ": " + std::to_string(count) + " translations need review (their source changed)");
    }
    for (const auto& [key, reference] : referencedKeys(projectPath)) {
        if (knownKeys.count(key)) continue;
        // a title's key belongs to its document (S87), not to a String Table
        const std::string ns = key.substr(0, key.find('.'));
        if (std::none_of(tables.sets.begin(), tables.sets.end(), [&](const StringTableSet& s) { return s.table.ns == ns; }) &&
                !namespaceOwner(projectPath, ns).empty()) continue;
        result.warnings.push_back("W-L10N-UNKNOWN-KEY: " + reference.file + ":" + std::to_string(reference.line) + " names \"" + key +
                                  "\", which no String Table has");
    }
    const std::string text = manifest.toJson();
    const io::WriteResult written = io::writeFileAtomic(assetsOut / "l10n" / "manifest.json", text);
    if (!written.ok()) {
        result.ok = false;
        result.error = "Cannot write l10n/manifest.json: " + written.message;
        return result;
    }
    result.written.push_back("l10n/manifest.json");
    result.bytes += text.size();
    return result;
}

void editor::l10n::refreshEditorLocalization(Project* project) {
    if (!project) return;
    Localization::setEditorMode(true);
    const ProjectTables tables = loadProjectTables(project->getProjectPath());
    if (tables.sets.empty() && !Localization::isActive()) return;
    Localization::setManifest(tables.sets.empty() ? LocalizationManifest() : editorManifest(project->getLocalizationSettings(), tables));
}
