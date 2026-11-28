// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Localization.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "io/FileData.h"
#include "io/UserSettings.h"
#include "localization/MessageFormat.h"
#include "localization/StringTableRuntime.h"
#include "util/TitleArtLanguage.h"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <set>

using namespace jasperengine;

FunctionSubscribe<void(const std::string&)> Localization::onLanguageChanged;

#if JASPERENGINE_L10N_CAPTURE
// the capture code is in this binary (D8): tools/l10n/check_capture_symbols.py looks for this name
// in a release engine (absent) and the editor's (present)
extern "C" JASPERENGINE_API const char jasperengine_l10n_capture[] = "missing-translation capture";
#endif

namespace {

    constexpr const char* kRememberKey = "jasper.language";
    constexpr const char* kPseudoAccented = "en-XA";
    constexpr const char* kPseudoRtl = "ar-XB";
#if JASPERENGINE_L10N_CAPTURE
    constexpr size_t kRingSize = 1024;
#endif

    struct LoadedNamespace {
        LocalizationNamespace info;
        // the chain's languages that were asked for: a table, or null when the file did not read
        std::map<std::string, std::shared_ptr<const l10n::StringTable>> tables;
        // the shown language is the source's (no "untranslated" capture), set per language change
        bool sourceShown = true;
    };

    struct State {
        std::mutex mutex;
        LocalizationManifest manifest;
        std::map<std::string, LoadedNamespace, std::less<>> namespaces;
        // the Default Table: what a key without a namespace reads (D-15), null for none
        LoadedNamespace* defaultNamespace = nullptr;
        std::string language;               // resolved
        std::vector<std::string> chain;     // the languages before the source text
        bool pseudo = false;
        std::map<std::string, Localization::Reader> readers;
        std::set<std::string> warned;
        int titleListener = -1;
        int localesListener = -1;
        int nextId = 1;
        std::map<int, std::function<void(const std::string&)>> listeners;
        std::map<int, std::function<void(Localization::ScanReport&)>> scanners;
#if JASPERENGINE_L10N_CAPTURE
        std::vector<Localization::Miss> misses;
        std::array<uint64_t, kRingSize> ring{};
        size_t ringNext = 0;
#endif
    };

    std::atomic<bool> active{false};
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> editorMode{false};

    State& state() {
        static State* s = new State();
        return *s;
    }

    const std::string& emptyString() {
        static const std::string empty;
        return empty;
    }

    bool isPseudo(const std::string& code) {
        return code == kPseudoAccented || code == kPseudoRtl;
    }

    bool development() {
        return editorMode.load() || Engine::isDevelopmentBuild();
    }

    std::string readFile(const std::string& path, bool& ok) {
        ok = false;
        FileData* file = FileData::newFile(path.c_str());
        if (!file) return {};
        std::string text;
        const unsigned int length = file->length();
        text.resize(length);
        ok = length == 0 || file->read(reinterpret_cast<unsigned char*>(text.data()), length) == length;
        delete file;
        return text;
    }

    std::string extensionOf(const std::string& path) {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
        return path.substr(dot);
    }

    std::shared_ptr<const l10n::StringTable> loadTable(State& s, const std::string& path, const std::string& language) {
        const std::string extension = extensionOf(path);
        std::string error;
        std::shared_ptr<const l10n::StringTable> table;
        const auto reader = s.readers.find(extension);
        if (reader != s.readers.end()) {
            table = reader->second(path, language, &error);
        } else {
            bool ok = false;
            const std::string text = readFile(path, ok);
            if (!ok) {
                error = "cannot be read";
            } else if (extension == ".strbin") {
                table = l10n::StringTable::fromStrbin(text, &error);
            } else if (extension == ".stringtable") {
                table = l10n::StringTable::fromStringTableJson(text, nullptr, nullptr, &error);
            } else if (extension == ".translation") {
                table = l10n::StringTable::fromTranslationJson(text, nullptr, &error);
            } else {
                error = "is not a string table";
            }
        }
        if (!table) Log::warn("[Localization] %s (%s) %s", path.c_str(), language.c_str(), error.c_str());
        return table;
    }

    // the table of one language of a namespace, loaded on first use; null when it has none
    const l10n::StringTable* tableOf(State& s, LoadedNamespace& ns, const std::string& language) {
        auto it = ns.tables.find(language);
        if (it != ns.tables.end()) return it->second.get();
        const auto file = ns.info.files.find(language);
        std::shared_ptr<const l10n::StringTable> table;
        if (file != ns.info.files.end()) table = loadTable(s, file->second, language);
        return ns.tables.emplace(language, std::move(table)).first->second.get();
    }

    // R8: the language, its truncations that are available, the default
    std::vector<std::string> chainOf(const std::string& language, const LocalizationManifest& manifest) {
        std::vector<std::string> chain;
        auto add = [&chain](const std::string& code) {
            if (!code.empty() && std::find(chain.begin(), chain.end(), code) == chain.end()) chain.push_back(code);
        };
        add(language);
        std::string tag = language;
        for (size_t dash = tag.find_last_of('-'); dash != std::string::npos; dash = tag.find_last_of('-')) {
            tag.resize(dash);
            if (std::find(manifest.languages.begin(), manifest.languages.end(), tag) != manifest.languages.end()) add(tag);
        }
        add(manifest.defaultLanguage);
        return chain;
    }

    std::vector<std::string> availableLanguages(const LocalizationManifest& manifest) {
        std::vector<std::string> available = manifest.languages;
        if (std::find(available.begin(), available.end(), manifest.defaultLanguage) == available.end()) {
            available.insert(available.begin(), manifest.defaultLanguage);
        }
        return available;
    }

    // R7; a pseudo-locale is taken as it is, in development builds only
    std::string resolveLanguage(const LocalizationManifest& manifest) {
        const std::vector<std::string> available = availableLanguages(manifest);
        std::vector<std::string> candidates;
        candidates.push_back(TitleArtLanguage::global());
        if (manifest.rememberLanguage && UserSettings::hasKey(kRememberKey)) candidates.push_back(UserSettings::getStringForKey(kRememberKey, ""));
        for (const std::string& locale : System::instance().getPreferredLocales()) candidates.push_back(TitleArtLanguage::canonicalize(locale));
        candidates.push_back(manifest.defaultLanguage);
        for (const std::string& candidate : candidates) {
            if (candidate.empty()) continue;
            if (isPseudo(candidate)) {
                if (development()) return candidate;
                continue;
            }
            const std::string match = TitleArtLanguage::match(candidate, available);
            if (!match.empty()) return match;
        }
        return manifest.defaultLanguage;
    }

    // the chain's tables stay; the others go (R26)
    void applyLanguage(State& s, const std::string& language) {
        generation++;
        s.language = language;
        s.pseudo = isPseudo(language);
        s.chain = chainOf(s.pseudo ? s.manifest.defaultLanguage : language, s.manifest);
        const std::string& shown = s.pseudo ? s.manifest.defaultLanguage : language;
        for (auto& [name, ns] : s.namespaces) {
            ns.sourceShown = !TitleArtLanguage::match(shown, {ns.info.sourceLanguage}).empty();
            for (auto it = ns.tables.begin(); it != ns.tables.end();) {
                const bool keep = it->first == ns.info.sourceLanguage || std::find(s.chain.begin(), s.chain.end(), it->first) != s.chain.end();
                it = keep ? std::next(it) : ns.tables.erase(it);
            }
        }
    }

    void notify(const std::string& language) {
        State& s = state();
        const auto listeners = s.listeners;
        for (const auto& entry : listeners) entry.second(language);
        Localization::onLanguageChanged.call(language);
    }

    // re-resolves; true when the language changed
    bool refresh() {
        if (!active.load()) return false;
        State& s = state();
        std::string language;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            language = resolveLanguage(s.manifest);
            if (language == s.language) return false;
            applyLanguage(s, language);
        }
        Log::print("[Localization] language %s", language.c_str());
        notify(language);
        return true;
    }

    void hookLanguageEvents(State& s) {
        if (s.titleListener < 0) s.titleListener = TitleArtLanguage::addListener([](const std::string&) { refresh(); });
        if (s.localesListener < 0) s.localesListener = TitleArtLanguage::addLocalesListener([]() { refresh(); });
    }

    // ---- pseudo-locales (R19)

    void appendCodePoint(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    void accentLiteral(std::string_view literal, std::string& out) {
        static const uint16_t lower[26] = {0xE1, 0x180, 0xE7, 0xF0, 0xE9, 0x192, 0x11D, 0x125, 0xED, 0x135, 0x137, 0x13A, 0x1E3F,
                                           0xF1, 0xF3, 0xFE, 0x1EB, 0x155, 0x161, 0x163, 0xFA, 0x1E7D, 0x175, 0x1E8B, 0xFD, 0x17E};
        static const uint16_t upper[26] = {0xC1, 0x181, 0xC7, 0xD0, 0xC9, 0x191, 0x11C, 0x124, 0xCD, 0x134, 0x136, 0x139, 0x1E3E,
                                           0xD1, 0xD3, 0xDE, 0x1EA, 0x154, 0x160, 0x162, 0xDA, 0x1E7C, 0x174, 0x1E8A, 0xDD, 0x17D};
        for (char c : literal) {
            if (c >= 'a' && c <= 'z') appendCodePoint(out, lower[c - 'a']);
            else if (c >= 'A' && c <= 'Z') appendCodePoint(out, upper[c - 'A']);
            else out += c;
        }
    }

    // the code points of a literal run in reverse order
    void reverseLiteral(std::string_view literal, std::string& out) {
        std::vector<std::string_view> points;
        for (size_t k = 0; k < literal.size();) {
            size_t length = 1;
            const unsigned char c = static_cast<unsigned char>(literal[k]);
            if (c >= 0xF0) length = 4;
            else if (c >= 0xE0) length = 3;
            else if (c >= 0xC0) length = 2;
            length = std::min(length, literal.size() - k);
            points.push_back(literal.substr(k, length));
            k += length;
        }
        for (auto it = points.rbegin(); it != points.rend(); ++it) out.append(*it);
    }

    size_t codePoints(std::string_view text) {
        size_t count = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) count++;
        return count;
    }

    // ---- capture (D8)

#if JASPERENGINE_L10N_CAPTURE
    uint64_t hashText(std::string_view text) {
        return l10n::StringTable::hashKey(text, 0x5bd1e995ull);
    }
#endif

    void remember(State& s, std::string_view text) {
#if JASPERENGINE_L10N_CAPTURE
        s.ring[s.ringNext] = hashText(text);
        s.ringNext = (s.ringNext + 1) % kRingSize;
#else
        (void)s;
        (void)text;
#endif
    }

    void capture(State& s, std::string_view key, const char* kind, std::string_view where) {
#if JASPERENGINE_L10N_CAPTURE
        for (Localization::Miss& miss : s.misses) {
            if (miss.key == key && miss.language == s.language && miss.kind == kind) {
                miss.count++;
                if (miss.where.empty()) miss.where = std::string(where);
                return;
            }
        }
        if (s.misses.size() < 4096) s.misses.push_back({std::string(key), s.language, kind, std::string(where), 1});
#else
        (void)s;
        (void)key;
        (void)kind;
        (void)where;
#endif
    }

    struct Found {
        std::string_view text;
        std::string_view language;     // the chain's or the namespace's: alive while the lock is held
        bool plain = false;
    };

    // the text of a global key along the chain, then the source; false for a key no table has
    bool lookup(State& s, std::string_view key, Found& found, std::string_view where) {
        if (key.empty()) return false;
        // "<namespace>.<key>"; a first segment that names no namespace makes the whole key the
        // Default Table's (game-ui-localization.md R1, D-15)
        const size_t dot = key.find('.');
        auto nsIt = s.namespaces.end();
        if (dot != std::string_view::npos && dot != 0 && dot + 1 != key.size()) nsIt = s.namespaces.find(key.substr(0, dot));
        if (nsIt == s.namespaces.end() && !s.defaultNamespace) return false;
        LoadedNamespace& ns = nsIt != s.namespaces.end() ? nsIt->second : *s.defaultNamespace;
        const std::string_view local = nsIt != s.namespaces.end() ? key.substr(dot + 1) : key;
        bool review = false;
        for (const std::string& language : s.chain) {
            if (language == ns.info.sourceLanguage) break;
            const l10n::StringTable* table = tableOf(s, ns, language);
            if (table && table->find(local, found.text, &review, &found.plain)) {
                found.language = language;
                if (review) capture(s, key, "stale", where);
                return true;
            }
        }
        const l10n::StringTable* source = tableOf(s, ns, ns.info.sourceLanguage);
        if (!source || !source->find(local, found.text, nullptr, &found.plain)) return false;
        found.language = ns.info.sourceLanguage;
        if (!ns.sourceShown) capture(s, key, "untranslated", where);
        return true;
    }

    bool resolveReference(void* context, std::string_view key, std::string& text, std::string& language) {
        State& s = *static_cast<State*>(context);
        Found found;
        if (!lookup(s, key, found, {})) return false;
        text.assign(found.text);
        language.assign(found.language);
        if (s.pseudo) {
            // the referenced noun is shown transformed like the rest of the message
            std::string transformed;
            (s.language == kPseudoRtl ? reverseLiteral : accentLiteral)(text, transformed);
            text = std::move(transformed);
        }
        return true;
    }

    void formatFound(State& s, const Found& found, const LocalizationArgs& args, std::string& out) {
        l10n::FormatOptions options;
        options.language = found.language;
        options.args = &args;
        options.showMissing = development();
        options.resolveKey = resolveReference;
        options.resolveContext = &s;
        if (!s.pseudo) {
            if (found.plain) out.append(found.text);
            else if (!l10n::MessageFormat::format(found.text, options, out)) {
                Log::warn("[Localization] a message does not parse, shown as written: %.*s", static_cast<int>(found.text.size()), found.text.data());
            }
            return;
        }
        const bool rtl = s.language == kPseudoRtl;
        options.literal = rtl ? reverseLiteral : accentLiteral;
        std::string body;
        l10n::MessageFormat::format(found.text, options, body);
        if (rtl) {
            out += body;
            return;
        }
        const size_t filler = (codePoints(body) * 4 + 9) / 10;
        out += "\xE2\x9F\xA6";      // ⟦
        out += body;
        for (size_t k = 0; k < filler; k++) out += "\xC2\xB7";     // ·
        out += "\xE2\x9F\xA7";      // ⟧
    }

    void warnUnknown(State& s, std::string_view key) {
        std::string id = std::string(key) + "|" + s.language;
        if (s.warned.insert(id).second) {
            Log::warn("[Localization] no table has the key \"%.*s\" (%s)", static_cast<int>(key.size()), key.data(), s.language.c_str());
        }
    }

}

// ---- the manifest

bool LocalizationManifest::fromJson(std::string_view json, LocalizationManifest& out, std::string* error) {
    const nlohmann::json doc = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("format", std::string()) != "jasperengine.l10n-manifest") {
        if (error) *error = "not a jasperengine.l10n-manifest";
        return false;
    }
    if (doc.value("version", 0) > 1) {
        if (error) *error = "manifest version " + std::to_string(doc.value("version", 0)) + " needs a newer engine";
        return false;
    }
    out = LocalizationManifest();
    out.defaultLanguage = doc.value("default_language", std::string("en"));
    out.rememberLanguage = doc.value("remember_language", true);
    out.defaultTable = doc.value("default_table", std::string());
    if (doc.contains("languages") && doc["languages"].is_array()) {
        for (const auto& code : doc["languages"]) if (code.is_string()) out.languages.push_back(code.get<std::string>());
    }
    if (doc.contains("names") && doc["names"].is_object()) {
        for (const auto& [code, name] : doc["names"].items()) if (name.is_string()) out.names[code] = name.get<std::string>();
    }
    if (doc.contains("fonts") && doc["fonts"].is_object()) {
        for (const auto& [code, list] : doc["fonts"].items()) {
            if (!list.is_array()) continue;
            for (const auto& path : list) if (path.is_string()) out.fonts[code].push_back(path.get<std::string>());
        }
    }
    if (doc.contains("namespaces") && doc["namespaces"].is_array()) {
        for (const auto& row : doc["namespaces"]) {
            if (!row.is_object()) continue;
            LocalizationNamespace ns;
            ns.name = row.value("namespace", std::string());
            ns.sourceLanguage = row.value("source_language", out.defaultLanguage);
            if (row.contains("files") && row["files"].is_object()) {
                for (const auto& [code, path] : row["files"].items()) if (path.is_string()) ns.files[code] = path.get<std::string>();
            }
            if (!ns.name.empty()) out.namespaces.push_back(std::move(ns));
        }
    }
    return true;
}

std::string LocalizationManifest::toJson() const {
    nlohmann::ordered_json doc;
    doc["format"] = "jasperengine.l10n-manifest";
    doc["version"] = 1;
    doc["default_language"] = defaultLanguage;
    doc["remember_language"] = rememberLanguage;
    if (!defaultTable.empty()) doc["default_table"] = defaultTable;
    doc["languages"] = languages;
    doc["names"] = nlohmann::ordered_json::object();
    for (const auto& [code, name] : names) doc["names"][code] = name;
    doc["fonts"] = nlohmann::ordered_json::object();
    for (const auto& [code, list] : fonts) doc["fonts"][code] = list;
    doc["namespaces"] = nlohmann::ordered_json::array();
    for (const LocalizationNamespace& ns : namespaces) {
        nlohmann::ordered_json row;
        row["namespace"] = ns.name;
        row["source_language"] = ns.sourceLanguage;
        row["files"] = nlohmann::ordered_json::object();
        for (const auto& [code, path] : ns.files) row["files"][code] = path;
        doc["namespaces"].push_back(std::move(row));
    }
    return doc.dump(2) + "\n";
}

// ---- Localization

bool Localization::isActive() {
    return active.load();
}

std::string Localization::get(std::string_view key, const LocalizationArgs& args, std::string_view fallback) {
    std::string out;
    if (translate(key, args, out)) return out;
    if (!fallback.empty()) return std::string(fallback);
    if (!development()) return {};
    return "\xE2\x9F\xA6" + std::string(key) + "\xE2\x9F\xA7";
}

bool Localization::translate(std::string_view key, const LocalizationArgs& args, std::string& out, std::string_view where) {
    if (!active.load()) return false;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Found found;
    if (!lookup(s, key, found, where)) {
        capture(s, key, "unknown_key", where);
        warnUnknown(s, key);
        return false;
    }
    const size_t start = out.size();
    formatFound(s, found, args, out);
    remember(s, std::string_view(out).substr(start));
    return true;
}

std::string_view Localization::getView(std::string_view key) {
    if (!active.load()) return {};
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Found found;
    if (s.pseudo || !lookup(s, key, found, {}) || !found.plain) return {};
    remember(s, found.text);
    return found.text;
}

bool Localization::has(std::string_view key) {
    if (!active.load()) return false;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Found found;
    return lookup(s, key, found, {});
}

std::string Localization::format(std::string_view pattern, const LocalizationArgs& args, std::string_view language) {
    l10n::FormatOptions options;
    std::string current;
    if (language.empty() && active.load()) {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        current = s.pseudo ? s.manifest.defaultLanguage : s.language;
    }
    options.language = language.empty() ? std::string_view(current.empty() ? std::string_view("en") : std::string_view(current)) : language;
    options.args = &args;
    options.showMissing = development();
    std::string out;
    l10n::MessageFormat::format(pattern, options, out);
    return out;
}

std::string Localization::pseudoLocalize(std::string_view text, const std::string& code, std::string_view language) {
    l10n::FormatOptions options;
    options.language = language;
    options.showMissing = true;
    const bool rtl = code == kPseudoRtl;
    options.literal = rtl ? reverseLiteral : accentLiteral;
    std::string body;
    l10n::MessageFormat::format(text, options, body);
    if (rtl) return body;
    std::string out = "\xE2\x9F\xA6" + body;
    for (size_t k = 0, filler = (codePoints(body) * 4 + 9) / 10; k < filler; k++) out += "\xC2\xB7";
    return out + "\xE2\x9F\xA7";
}

const std::string& Localization::getLanguage() {
    if (!active.load()) return emptyString();
    return state().language;
}

const std::vector<std::string>& Localization::getLanguages() {
    static const std::vector<std::string> none;
    if (!active.load()) return none;
    return state().manifest.languages;
}

bool Localization::setLanguage(const std::string& code, std::string* error) {
    if (!code.empty() && !TitleArtLanguage::isValid(code, error)) return false;
    // the remembered choice first: the global change below re-resolves with it; "" forgets it, so
    // the game follows the device again
    if (active.load() && state().manifest.rememberLanguage) {
        if (code.empty()) UserSettings::removeKey(kRememberKey);
        else UserSettings::setStringForKey(kRememberKey, TitleArtLanguage::canonicalize(code));
    }
    if (!TitleArtLanguage::setGlobal(code, error)) return false;
    // the same global value can still resolve differently (the remembered choice changed)
    refresh();
    return true;
}

std::string Localization::getLanguageName(const std::string& code) {
    if (active.load()) {
        const auto& names = state().manifest.names;
        const auto it = names.find(code);
        if (it != names.end()) return it->second;
    }
    if (code == kPseudoAccented) return "Pseudo (en-XA)";
    if (code == kPseudoRtl) return "Pseudo RTL (ar-XB)";
    return code;
}

bool Localization::isRightToLeft(std::string_view code) {
    std::string tag = code.empty() ? getLanguage() : TitleArtLanguage::canonicalize(std::string(code));
    if (tag == kPseudoRtl) return true;
    static const char* const rtlScripts[] = {"Arab", "Hebr", "Thaa", "Syrc", "Nkoo", "Adlm"};
    static const char* const rtlLanguages[] = {"ar", "fa", "he", "ur", "ps", "sd", "ug", "yi", "dv", "ckb", "syr", "nqo", "arc", "ks", "mzn", "glk", "lrc"};
    const size_t dash = tag.find('-');
    const std::string language = tag.substr(0, dash);
    if (dash != std::string::npos) {
        const std::string rest = tag.substr(dash + 1);
        const std::string script = rest.size() >= 4 && (rest.size() == 4 || rest[4] == '-') ? rest.substr(0, 4) : std::string();
        if (!script.empty() && std::isalpha(static_cast<unsigned char>(script[0])) && std::isalpha(static_cast<unsigned char>(script[3]))) {
            for (const char* s : rtlScripts) if (script == s) return true;
            return false;
        }
    }
    for (const char* l : rtlLanguages) if (language == l) return true;
    return false;
}

std::vector<std::string> Localization::getLanguageFonts(const std::string& code) {
    if (!active.load()) return {};
    const auto& fonts = state().manifest.fonts;
    std::vector<std::string> keys;
    for (const auto& entry : fonts) keys.push_back(entry.first);
    const std::string match = TitleArtLanguage::match(code, keys);
    return match.empty() ? std::vector<std::string>() : fonts.at(match);
}

uint64_t Localization::getGeneration() {
    return active.load() ? generation.load() : 0;
}

std::vector<Localization::Miss> Localization::getMisses() {
#if JASPERENGINE_L10N_CAPTURE
    if (!active.load() || !jasperengine_l10n_capture[0]) return {};
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.misses;
#else
    return {};
#endif
}

void Localization::clearMisses() {
#if JASPERENGINE_L10N_CAPTURE
    if (!active.load()) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.misses.clear();
#endif
}

bool Localization::wasProduced(std::string_view text) {
#if JASPERENGINE_L10N_CAPTURE
    if (!active.load()) return false;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const uint64_t hash = hashText(text);
    return std::find(s.ring.begin(), s.ring.end(), hash) != s.ring.end();
#else
    (void)text;
    return false;
#endif
}

Localization::ScanReport Localization::scan() {
    ScanReport report;
#if JASPERENGINE_L10N_CAPTURE
    if (!active.load()) return report;
    State& s = state();
    const auto scanners = s.scanners;
    for (const auto& entry : scanners) entry.second(report);
    report.missing = getMisses();
#endif
    return report;
}

int Localization::addScanner(std::function<void(ScanReport&)> scanner) {
    State& s = state();
    const int id = s.nextId++;
    s.scanners[id] = std::move(scanner);
    return id;
}

void Localization::removeScanner(int id) {
    state().scanners.erase(id);
}

int Localization::addListener(std::function<void(const std::string&)> listener) {
    State& s = state();
    const int id = s.nextId++;
    s.listeners[id] = std::move(listener);
    return id;
}

void Localization::removeListener(int id) {
    state().listeners.erase(id);
}

bool Localization::loadManifest(const std::string& path) {
    if (path.empty()) {
        setManifest(LocalizationManifest());
        return true;
    }
    bool ok = false;
    const std::string text = readFile(path, ok);
    LocalizationManifest manifest;
    std::string error;
    if (!ok || !LocalizationManifest::fromJson(text, manifest, &error)) {
        Log::error("[Localization] %s: %s", path.c_str(), ok ? error.c_str() : "cannot be read");
        return false;
    }
    setManifest(manifest);
    return true;
}

void Localization::setManifest(const LocalizationManifest& manifest) {
    if (manifest.namespaces.empty() && !active.load()) return;
    State& s = state();
    std::string language;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const std::string before = s.language;
        s.manifest = manifest;
        s.namespaces.clear();
        for (const LocalizationNamespace& ns : manifest.namespaces) s.namespaces[ns.name].info = ns;
        s.defaultNamespace = nullptr;
        if (!manifest.defaultTable.empty()) {
            const auto it = s.namespaces.find(manifest.defaultTable);
            if (it != s.namespaces.end()) s.defaultNamespace = &it->second;
            else Log::warn("[Localization] Default Table '%s' is no String Table's namespace", manifest.defaultTable.c_str());
        } else if (s.namespaces.size() == 1) {
            s.defaultNamespace = &s.namespaces.begin()->second;
        }
        active.store(!manifest.namespaces.empty());
        if (!active.load()) {
            s.language.clear();
            s.chain.clear();
            return;
        }
        TitleArtLanguage::setProjectDefault(manifest.defaultLanguage);
        language = resolveLanguage(s.manifest);
        applyLanguage(s, language);
        changed = !before.empty() && before != language;
    }
    hookLanguageEvents(s);
    Log::print("[Localization] language %s", language.c_str());
    // a reload re-translates what is shown; only a different language is an event for scripts
    const auto listeners = s.listeners;
    for (const auto& entry : listeners) entry.second(language);
    if (changed) onLanguageChanged.call(language);
}

const LocalizationManifest& Localization::getManifest() {
    static const LocalizationManifest none;
    if (!active.load()) return none;
    return state().manifest;
}

void Localization::registerReader(const std::string& extension, Reader reader) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.readers[extension] = std::move(reader);
}

void Localization::setEditorMode(bool editor) {
    editorMode.store(editor);
}

void Localization::reloadNamespace(const std::string& ns) {
    if (!active.load()) return;
    State& s = state();
    std::string language;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto it = s.namespaces.find(ns);
        if (it == s.namespaces.end()) return;
        it->second.tables.clear();
        language = s.language;
        generation++;
    }
    const auto listeners = s.listeners;
    for (const auto& entry : listeners) entry.second(language);
}

void Localization::resetForTesting() {
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.manifest = LocalizationManifest();
        s.namespaces.clear();
        s.defaultNamespace = nullptr;
        s.language.clear();
        s.chain.clear();
        s.pseudo = false;
        s.readers.clear();
        s.warned.clear();
        editorMode.store(false);
#if JASPERENGINE_L10N_CAPTURE
        s.misses.clear();
        s.ring.fill(0);
        s.ringNext = 0;
#endif
    }
    if (s.titleListener >= 0) TitleArtLanguage::removeListener(s.titleListener);
    if (s.localesListener >= 0) TitleArtLanguage::removeLocalesListener(s.localesListener);
    s.titleListener = -1;
    s.localesListener = -1;
    s.listeners.clear();
    s.scanners.clear();
    onLanguageChanged.clear();
    active.store(false);
}
