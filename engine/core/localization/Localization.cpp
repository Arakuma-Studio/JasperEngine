// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Localization.h"

#include "localization/StringTable.h"

#include "Log.h"
#include "System.h"
#include "io/Data.h"
#include "io/UserSettings.h"
#include "util/TitleArtLanguage.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>

using namespace jasperengine;

namespace {

    struct LocLoadedTable{
        std::string name;
        StringTable table;
    };

    // a key's value in the current language, resolved once per generation
    struct LocResolved{
        const StringTableValue* value = nullptr;
        std::string language;
    };

    struct LocState{
        LocalizationConfig config;
        bool loaded = false;
        bool testTables = false;
        std::vector<LocLoadedTable> tables;
        // "key" (the first table that has it) and "Table:key" -> (table, entry)
        std::unordered_map<std::string, std::pair<size_t, size_t>> index;
        std::vector<std::vector<LocResolved>> resolved;   // [table][entry]
        uint64_t revision = 0;
        uint64_t resolvedGeneration = ~uint64_t(0);
        std::string language;
        std::set<std::string> warned;
        int rememberListener = 0;
    };

    LocState& locState(){
        static LocState s;
        return s;
    }

    void locBuildIndex(LocState& s){
        s.index.clear();
        for (size_t t = 0; t < s.tables.size(); t++){
            const LocLoadedTable& table = s.tables[t];
            for (size_t e = 0; e < table.table.entries.size(); e++){
                const std::string& key = table.table.entries[e].key;
                s.index.emplace(key, std::make_pair(t, e));
                s.index.emplace(table.name + ":" + key, std::make_pair(t, e));
            }
        }
        s.resolvedGeneration = ~uint64_t(0);
    }

    void locEnsureLoaded(LocState& s){
        if (s.loaded) return;
        s.loaded = true;
        s.tables.clear();
        for (const std::string& path : s.config.tables){
            Data data;
            if (data.open(path.c_str()) != FILEDATA_OK){
                Log::error("Localization: cannot read the string table %s", path.c_str());
                continue;
            }
            LocLoadedTable loaded;
            loaded.name = StringTable::nameOfPath(path);
            std::string error;
            std::vector<std::string> problems;
            if (!StringTable::parse(std::string(reinterpret_cast<const char*>(data.getMemPtr()), data.length()), loaded.table, &error, &problems)){
                Log::error("Localization: %s: %s", path.c_str(), error.c_str());
                continue;
            }
            for (const std::string& problem : problems) Log::warn("Localization: %s: %s", path.c_str(), problem.c_str());
            s.tables.push_back(std::move(loaded));
        }
        locBuildIndex(s);
    }

    std::vector<std::string> locAvailableLanguages(LocState& s){
        if (!s.config.languages.empty()) return s.config.languages;
        locEnsureLoaded(s);
        std::vector<std::string> out;
        for (const LocLoadedTable& table : s.tables){
            for (const std::string& code : table.table.languages()){
                if (std::find(out.begin(), out.end(), code) == out.end()) out.push_back(code);
            }
        }
        return out;
    }

    std::string locProjectDefault(){
        return TitleArtLanguage::projectDefault().empty() ? std::string("en") : TitleArtLanguage::projectDefault();
    }

    // "zh-Hant-TW" -> "zh-Hant", "zh"; "" after the language subtag
    std::string locParentTag(const std::string& tag){
        const size_t dash = tag.rfind('-');
        return dash == std::string::npos ? std::string() : tag.substr(0, dash);
    }

    // the fallback chain: the language, its parents, the project default
    void locResolveValue(const StringTableEntry& entry, const std::string& language, LocResolved& out){
        out = LocResolved();
        for (std::string tag = language; !tag.empty(); tag = locParentTag(tag)){
            if (const StringTableValue* value = entry.find(tag)){
                out.value = value;
                out.language = tag;
                return;
            }
        }
        const std::string fallback = locProjectDefault();
        if (const StringTableValue* value = entry.find(fallback)){
            out.value = value;
            out.language = fallback;
        }
    }

    void locRefresh(LocState& s){
        locEnsureLoaded(s);
        const uint64_t now = Localization::generation();
        if (s.resolvedGeneration == now) return;
        s.resolvedGeneration = now;
        const std::vector<std::string> preferred = System::hasInstance() ? System::instance().getPreferredLocales() : std::vector<std::string>();
        s.language = TitleArtLanguage::resolve(TitleArtLanguage::candidates("", preferred, ""), locAvailableLanguages(s));
        if (s.language.empty()) s.language = locProjectDefault();
        s.resolved.assign(s.tables.size(), {});
        for (size_t t = 0; t < s.tables.size(); t++){
            const std::vector<StringTableEntry>& entries = s.tables[t].table.entries;
            s.resolved[t].resize(entries.size());
            for (size_t e = 0; e < entries.size(); e++) locResolveValue(entries[e], s.language, s.resolved[t][e]);
        }
    }

    void locWarnOnce(LocState& s, const std::string& key, const std::string& message){
        if (!s.warned.insert(key).second) return;
        Log::warn("Localization: %s", message.c_str());
    }

    LocalizationArg locArgOfJson(const nlohmann::json& value){
        if (value.is_number()) return LocalizationArg::ofNumber(value.get<double>());
        if (value.is_string()) return LocalizationArg::ofText(value.get<std::string>());
        if (value.is_boolean()) return LocalizationArg::ofText(value.get<bool>() ? "true" : "false");
        return LocalizationArg::ofText(value.is_null() ? std::string() : value.dump());
    }

    nlohmann::json locJsonOfArg(const LocalizationArg& arg){
        return arg.isNumber ? nlohmann::json(arg.number) : nlohmann::json(arg.text);
    }

}

LocalizationArg LocalizationArg::ofText(const std::string& text){
    LocalizationArg arg;
    arg.text = text;
    return arg;
}

LocalizationArg LocalizationArg::ofNumber(double number){
    LocalizationArg arg;
    arg.isNumber = true;
    arg.number = number;
    char buffer[64];
    if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15){
        std::snprintf(buffer, sizeof(buffer), "%lld", (long long)number);
    }else{
        // Lua's own %.14g
        std::snprintf(buffer, sizeof(buffer), "%.14g", number);
    }
    arg.text = buffer;
    return arg;
}

const LocalizationArg* LocalizationArgs::find(const std::string& name) const{
    for (const auto& entry : named){
        if (entry.first == name) return &entry.second;
    }
    return nullptr;
}

bool LocalizationArgs::count(double& out) const{
    if (const LocalizationArg* arg = find("count"); arg && arg->isNumber){
        out = arg->number;
        return true;
    }
    if (!positional.empty() && positional.front().isNumber){
        out = positional.front().number;
        return true;
    }
    return false;
}

LocalizationArgs LocalizationArgs::fromJson(const std::string& text){
    LocalizationArgs args;
    if (text.empty()) return args;
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) return args;
    if (json.is_object()){
        for (auto it = json.begin(); it != json.end(); ++it) args.named.emplace_back(it.key(), locArgOfJson(*it));
    }else if (json.is_array()){
        for (const nlohmann::json& item : json) args.positional.push_back(locArgOfJson(item));
    }else if (!json.is_null()){
        args.positional.push_back(locArgOfJson(json));
    }
    return args;
}

std::string LocalizationArgs::toJson() const{
    if (empty()) return "";
    if (named.empty()){
        nlohmann::json list = nlohmann::json::array();
        for (const LocalizationArg& arg : positional) list.push_back(locJsonOfArg(arg));
        return list.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    // named and positional together: the positional ones under their index ("0", "1")
    nlohmann::ordered_json object = nlohmann::ordered_json::object();
    for (size_t i = 0; i < positional.size(); i++) object[std::to_string(i)] = locJsonOfArg(positional[i]);
    for (const auto& entry : named) object[entry.first] = locJsonOfArg(entry.second);
    return object.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
}

void Localization::configure(const LocalizationConfig& config){
    LocState& s = locState();
    if (s.config == config && !s.testTables) return;
    s.config = config;
    reloadTables();
}

const LocalizationConfig& Localization::getConfig(){
    return locState().config;
}

void Localization::reloadTables(){
    LocState& s = locState();
    s.loaded = false;
    s.testTables = false;
    s.tables.clear();
    s.index.clear();
    s.resolved.clear();
    s.warned.clear();
    s.revision++;
}

void Localization::start(){
    LocState& s = locState();
    stop();
    if (!s.config.rememberLanguage || !System::hasInstance()) return;
    if (UserSettings::hasKey(REMEMBERED_LANGUAGE_KEY)){
        const std::string stored = UserSettings::getStringForKey(REMEMBERED_LANGUAGE_KEY);
        std::string error;
        if (!TitleArtLanguage::setGlobal(stored, &error)) Log::warn("Localization: the remembered language '%s' is ignored: %s", stored.c_str(), error.c_str());
    }
    s.rememberListener = TitleArtLanguage::addListener([](const std::string& code){
        if (code.empty()) UserSettings::removeKey(REMEMBERED_LANGUAGE_KEY);
        else UserSettings::setStringForKey(REMEMBERED_LANGUAGE_KEY, code);
    });
}

void Localization::stop(){
    LocState& s = locState();
    if (s.rememberListener == 0) return;
    TitleArtLanguage::removeListener(s.rememberListener);
    s.rememberListener = 0;
}

std::string Localization::getLanguage(){
    LocState& s = locState();
    locRefresh(s);
    return s.language;
}

bool Localization::setLanguage(const std::string& code, std::string* error){
    return TitleArtLanguage::setGlobal(code, error);
}

std::vector<std::string> Localization::getLanguages(){
    return locAvailableLanguages(locState());
}

bool Localization::has(const std::string& key){
    LocState& s = locState();
    locEnsureLoaded(s);
    return s.index.count(key) > 0;
}

std::string Localization::get(const std::string& key, const LocalizationArgs& args){
    LocState& s = locState();
    locRefresh(s);
    const auto it = s.index.find(key);
    if (it == s.index.end()){
        locWarnOnce(s, key, "no string table has the key '" + key + "': it shows as the key");
        return key;
    }
    const LocResolved& resolved = s.resolved[it->second.first][it->second.second];
    if (!resolved.value){
        locWarnOnce(s, key, "'" + key + "' has no " + s.language + " value, nor one in the default language " + locProjectDefault() + ": it shows as the key");
        return key;
    }
    if (!resolved.value->plural) return args.empty() ? resolved.value->text : format(resolved.value->text, args);
    double count = 0.0;
    const PluralCategory category = args.count(count) ? PluralRules::select(resolved.language, count) : PluralCategory::OTHER;
    return format(resolved.value->form(category), args);
}

uint64_t Localization::generation(){
    return TitleArtLanguage::generation() + locState().revision;
}

std::string Localization::format(const std::string& pattern, const LocalizationArgs& args){
    if (pattern.find_first_of("{}") == std::string::npos) return pattern;
    std::string out;
    out.reserve(pattern.size() + 16);
    for (size_t i = 0; i < pattern.size(); i++){
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c){
            out += c;
            i++;
            continue;
        }
        if (c != '{'){
            out += c;
            continue;
        }
        const size_t close = pattern.find('}', i + 1);
        const std::string name = close == std::string::npos ? std::string() : pattern.substr(i + 1, close - i - 1);
        const LocalizationArg* arg = nullptr;
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](char d){ return std::isdigit((unsigned char)d) != 0; })){
            const size_t index = (size_t)std::strtoul(name.c_str(), nullptr, 10);
            if (index < args.positional.size()) arg = &args.positional[index];
            if (!arg) arg = args.find(name);
        }else if (!name.empty()){
            arg = args.find(name);
        }
        if (!arg){
            out += c;
            continue;
        }
        out += arg->text;
        i = close;
    }
    return out;
}

void Localization::setTablesForTesting(const std::vector<std::pair<std::string, StringTable>>& tables){
    LocState& s = locState();
    reloadTables();
    s.loaded = true;
    s.testTables = true;
    for (const auto& table : tables) s.tables.push_back({table.first, table.second});
    locBuildIndex(s);
}

void Localization::resetForTesting(){
    stop();
    locState() = LocState();
}
