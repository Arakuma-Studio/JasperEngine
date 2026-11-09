// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtLanguage.h"

#include "util/PlatformRules.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <map>

using namespace jasperengine;

namespace {

    struct GlobalState{
        std::string language;
        std::string projectDefault;
        uint64_t generation = 0;
        int nextListener = 1;
        std::map<int, TitleArtLanguage::Listener> listeners;
    };

    GlobalState& state(){
        static GlobalState s;
        return s;
    }

    std::string lower(std::string s){
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool alpha(const std::string& s){
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c){ return std::isalpha((unsigned char)c) != 0; });
    }

    // language, script (inferred for Chinese) and region of a canonical tag
    struct Parts{
        std::string language, script, region;
    };

    bool digits(const std::string& s){
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c){ return std::isdigit((unsigned char)c) != 0; });
    }

    // the subtags of a canonical tag (LocaleRules' rules, without normalizing again)
    Parts partsOf(const std::string& tag){
        Parts p;
        size_t start = 0, index = 0;
        while (start <= tag.size()){
            size_t end = tag.find('-', start);
            if (end == std::string::npos) end = tag.size();
            const std::string part = tag.substr(start, end - start);
            if (index == 0) p.language = lower(part);
            else if (index == 1 && part.size() == 4 && alpha(part)) p.script = lower(part);
            else if (p.region.empty() && index <= 2 && ((part.size() == 2 && alpha(part)) || (part.size() == 3 && digits(part)))) p.region = part;
            else if (index >= 1) break;
            index++;
            start = end + 1;
        }
        for (char& c : p.region) c = (char)std::toupper((unsigned char)c);
        if (p.language == "zh" && p.script.empty()){
            p.script = (p.region == "TW" || p.region == "HK" || p.region == "MO") ? "hant" : "hans";
        }
        return p;
    }

}

std::vector<std::string> TitleArtLanguageTable::available() const{
    std::vector<std::string> out;
    if (!source.empty()) out.push_back(source);
    for (const auto& set : sets) out.push_back(set.first);
    return out;
}

std::string TitleArtLanguageTable::setFile(const std::string& code) const{
    for (const auto& set : sets){
        if (lower(set.first) == lower(code)) return set.second;
    }
    return "";
}

std::string TitleArtLanguage::canonicalize(const std::string& tag){
    std::string out = LocaleRules::normalize(tag);
    if (out.empty()) return out;
    const size_t dash = out.find('-');
    const std::string language = out.substr(0, dash);
    const char* replacement = language == "iw" ? "he" : language == "in" ? "id" : language == "ji" ? "yi" : nullptr;
    if (replacement) out = replacement + (dash == std::string::npos ? std::string() : out.substr(dash));
    return out;
}

bool TitleArtLanguage::isValid(const std::string& tag, std::string* error){
    std::vector<std::string> parts;
    std::string part;
    bool ok = !tag.empty();
    for (size_t i = 0; i <= tag.size() && ok; i++){
        if (i == tag.size() || tag[i] == '-' || tag[i] == '_'){
            parts.push_back(part);
            part.clear();
        }else{
            part += tag[i];
        }
    }
    for (size_t i = 0; i < parts.size() && ok; i++){
        const std::string& p = parts[i];
        const bool alnum = !p.empty() && std::all_of(p.begin(), p.end(), [](char c){ return std::isalnum((unsigned char)c) != 0; });
        if (i == 0) ok = alpha(p) && (p.size() == 2 || p.size() == 3 || (p.size() >= 4 && p.size() <= 8));
        else ok = alnum && p.size() <= 8;
    }
    if (!ok && error){
        *error = "'" + tag + "' is not a language tag (BCP 47, for example \"de\", \"pt-BR\", \"zh-Hant\")";
    }
    return ok;
}

namespace {

    // `available` canonicalised once: renderers ask with the same list every time (R48)
    struct AvailableCache{
        std::vector<std::string> source;
        std::vector<std::string> lowered;
        std::vector<Parts> parts;
    };

    const AvailableCache& prepared(const std::vector<std::string>& available){
        thread_local AvailableCache cache;
        if (cache.source != available){
            cache.source = available;
            cache.lowered.clear();
            cache.parts.clear();
            for (const std::string& a : available){
                const std::string c = TitleArtLanguage::canonicalize(a);
                cache.lowered.push_back(lower(c));
                cache.parts.push_back(partsOf(c));
            }
        }
        return cache;
    }

}

std::string TitleArtLanguage::match(const std::string& candidate, const std::vector<std::string>& available){
    const std::string wanted = canonicalize(candidate);
    if (wanted.empty()) return "";
    const AvailableCache& av = prepared(available);
    const std::string wantedLower = lower(wanted);
    for (size_t i = 0; i < available.size(); i++){
        if (av.lowered[i] == wantedLower) return available[i];
    }
    const Parts want = partsOf(wanted);
    // (2) same language and script, region ignored: a tag without region first
    std::string first;
    for (size_t i = 0; i < available.size(); i++){
        const Parts& have = av.parts[i];
        if (have.language != want.language || have.script != want.script) continue;
        if (have.region.empty()) return available[i];
        if (first.empty()) first = available[i];
    }
    if (!first.empty()) return first;
    // (3) same language, either script unknown: the bare language first
    for (size_t i = 0; i < available.size(); i++){
        const Parts& have = av.parts[i];
        if (have.language != want.language || (!have.script.empty() && !want.script.empty())) continue;
        if (av.lowered[i] == want.language) return available[i];
        if (first.empty()) first = available[i];
    }
    return first;
}

std::string TitleArtLanguage::resolve(const std::vector<std::string>& candidates, const std::vector<std::string>& available){
    for (const std::string& c : candidates){
        if (c.empty()) continue;
        std::string found = match(c, available);
        if (!found.empty()) return found;
    }
    return "";
}

std::vector<std::string> TitleArtLanguage::candidates(const std::string& component, const std::vector<std::string>& preferred,
                                                      const std::string& source){
    std::vector<std::string> out;
    const auto add = [&](const std::string& c){ if (!c.empty()) out.push_back(c); };
    add(component);
    add(state().language);
    for (const std::string& p : preferred) add(p);
    add(state().projectDefault);
    add(source);
    return out;
}

bool TitleArtLanguage::setGlobal(const std::string& code, std::string* error){
    if (!code.empty() && !isValid(code, error)) return false;
    const std::string value = canonicalize(code);
    GlobalState& s = state();
    if (value == s.language) return true;
    s.language = value;
    s.generation++;
    // a listener may add or remove listeners: call a copy
    const std::map<int, Listener> listeners = s.listeners;
    for (const auto& entry : listeners) entry.second(value);
    return true;
}

const std::string& TitleArtLanguage::global(){
    return state().language;
}

uint64_t TitleArtLanguage::generation(){
    return state().generation;
}

void TitleArtLanguage::localesChanged(){
    state().generation++;
}

int TitleArtLanguage::addListener(Listener listener){
    GlobalState& s = state();
    const int id = s.nextListener++;
    s.listeners[id] = std::move(listener);
    return id;
}

void TitleArtLanguage::removeListener(int id){
    state().listeners.erase(id);
}

void TitleArtLanguage::setProjectDefault(const std::string& code){
    const std::string value = canonicalize(code);
    if (value == state().projectDefault) return;
    state().projectDefault = value;
    state().generation++;
}

const std::string& TitleArtLanguage::projectDefault(){
    return state().projectDefault;
}

bool TitleArtLanguage::parseTable(const std::string& sidecarText, TitleArtLanguageTable& out){
    out = TitleArtLanguageTable();
    const nlohmann::ordered_json json = nlohmann::ordered_json::parse(sidecarText, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return false;
    auto table = json.find("languages");
    if (table == json.end() || !table->is_object()) return false;
    auto text = [&](const char* key){
        auto it = table->find(key);
        return it != table->end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    out.source = text("source");
    out.defaultLanguage = text("default");
    auto sets = table->find("sets");
    if (out.source.empty() || sets == table->end() || !sets->is_object()) return false;
    for (auto it = sets->begin(); it != sets->end(); ++it){
        if (it->is_string()) out.sets.emplace_back(it.key(), it->get<std::string>());
        else if (it->is_null()) out.sets.emplace_back(it.key(), std::string());
    }
    if (out.defaultLanguage.empty()) out.defaultLanguage = out.source;
    out.present = true;
    return true;
}

void TitleArtLanguage::resetForTesting(){
    state() = GlobalState();
}
