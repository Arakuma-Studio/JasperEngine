// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTable.h"

#include "util/TitleArtLanguage.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>

using namespace jasperengine;

namespace {

    using StOrderedJson = nlohmann::ordered_json;

    std::string stLowerText(std::string s){
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool stSameLanguage(const std::string& a, const std::string& b){
        return a.size() == b.size() && stLowerText(a) == stLowerText(b);
    }

    // the text between a '{' at `open` and its matching '}' (nested braces kept); npos when unclosed
    size_t stMatchingBrace(const std::string& s, size_t open){
        int depth = 0;
        for (size_t i = open; i < s.size(); i++){
            if (s[i] == '{') depth++;
            else if (s[i] == '}' && --depth == 0) return i;
        }
        return std::string::npos;
    }

    void stSkipSpaces(const std::string& s, size_t& i){
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    }

    std::string stWord(const std::string& s, size_t& i){
        size_t start = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '=')) i++;
        return s.substr(start, i - start);
    }

    // ICU's '#' in a plural form is the number: our forms name it {count}
    std::string stHashToCount(const std::string& text){
        std::string out;
        int depth = 0;
        for (char c : text){
            if (c == '{') depth++;
            else if (c == '}') depth--;
            if (c == '#' && depth == 0) out += "{count}";
            else out += c;
        }
        return out;
    }

    bool stReadValue(const StOrderedJson& json, StringTableValue& out, std::string& why){
        if (json.is_string()){
            out = StringTableValue::ofText(json.get<std::string>());
            return true;
        }
        if (!json.is_object()){
            why = "a value is a string or an object of plural forms";
            return false;
        }
        out = StringTableValue();
        out.plural = true;
        for (auto it = json.begin(); it != json.end(); ++it){
            PluralCategory category;
            if (!PluralRules::fromName(it.key(), category)){
                why = "'" + it.key() + "' is not a plural form (zero, one, two, few, many, other)";
                return false;
            }
            if (!it->is_string()){
                why = "the plural form '" + it.key() + "' is not a string";
                return false;
            }
            out.setForm(category, it->get<std::string>());
        }
        if (out.present == 0){
            why = "a plural value has no forms";
            return false;
        }
        return true;
    }

    StOrderedJson stValueJson(const StringTableValue& value){
        if (!value.plural) return StOrderedJson(value.text);
        StOrderedJson forms = StOrderedJson::object();
        for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++){
            if (value.has((PluralCategory)c)) forms[PluralRules::name((PluralCategory)c)] = value.forms[(size_t)c];
        }
        return forms;
    }

}

StringTableValue StringTableValue::ofText(const std::string& text){
    StringTableValue value;
    value.text = text;
    return value;
}

void StringTableValue::setForm(PluralCategory category, const std::string& text){
    plural = true;
    forms[(size_t)category] = text;
    present = (uint8_t)(present | (1u << (int)category));
}

const std::string& StringTableValue::form(PluralCategory category) const{
    if (!plural) return text;
    if (has(category)) return forms[(size_t)category];
    if (has(PluralCategory::OTHER)) return forms[(size_t)PluralCategory::OTHER];
    for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++){
        if (has((PluralCategory)c)) return forms[(size_t)c];
    }
    return text;
}

bool StringTableValue::empty() const{
    if (!plural) return text.empty();
    for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++){
        if (has((PluralCategory)c) && !forms[(size_t)c].empty()) return false;
    }
    return true;
}

std::string StringTableValue::toMessageFormat() const{
    if (!plural) return text;
    std::string out = "{count, plural,";
    for (int c = 0; c < PLURAL_CATEGORY_COUNT; c++){
        if (has((PluralCategory)c)) out += std::string(" ") + PluralRules::name((PluralCategory)c) + " {" + forms[(size_t)c] + "}";
    }
    return out + "}";
}

StringTableValue StringTableValue::fromMessageFormat(const std::string& source){
    const StringTableValue plain = ofText(source);
    size_t start = 0, end = source.size();
    while (start < end && std::isspace((unsigned char)source[start])) start++;
    while (end > start && std::isspace((unsigned char)source[end - 1])) end--;
    if (end - start < 2 || source[start] != '{' || stMatchingBrace(source, start) != end - 1) return plain;
    const std::string body = source.substr(start + 1, end - start - 2);
    size_t i = 0;
    stSkipSpaces(body, i);
    const std::string argument = stWord(body, i);
    stSkipSpaces(body, i);
    if (argument.empty() || i >= body.size() || body[i] != ',') return plain;
    i++;
    stSkipSpaces(body, i);
    if (stWord(body, i) != "plural") return plain;
    stSkipSpaces(body, i);
    if (i >= body.size() || body[i] != ',') return plain;
    i++;
    StringTableValue value;
    while (true){
        stSkipSpaces(body, i);
        if (i >= body.size()) break;
        std::string selector = stWord(body, i);
        // ICU's exact matches of the categories' usual numbers
        if (selector == "=0") selector = "zero";
        else if (selector == "=1") selector = "one";
        else if (selector == "=2") selector = "two";
        PluralCategory category;
        stSkipSpaces(body, i);
        if (!PluralRules::fromName(selector, category) || i >= body.size() || body[i] != '{') return plain;
        const size_t close = stMatchingBrace(body, i);
        if (close == std::string::npos) return plain;
        std::string text = body.substr(i + 1, close - i - 1);
        if (argument == "count") text = stHashToCount(text);
        value.setForm(category, text);
        i = close + 1;
    }
    return value.present ? value : plain;
}

bool StringTableValue::operator==(const StringTableValue& other) const{
    if (plural != other.plural) return false;
    if (!plural) return text == other.text;
    return present == other.present && forms == other.forms;
}

const StringTableValue* StringTableEntry::find(const std::string& language) const{
    for (const auto& value : values){
        if (stSameLanguage(value.first, language)) return &value.second;
    }
    return nullptr;
}

void StringTableEntry::set(const std::string& language, const StringTableValue& value){
    for (auto& existing : values){
        if (stSameLanguage(existing.first, language)){
            existing.second = value;
            return;
        }
    }
    values.emplace_back(TitleArtLanguage::canonicalize(language), value);
}

bool StringTableEntry::erase(const std::string& language){
    const auto it = std::find_if(values.begin(), values.end(), [&](const auto& v){ return stSameLanguage(v.first, language); });
    if (it == values.end()) return false;
    values.erase(it);
    return true;
}

bool StringTableEntry::operator==(const StringTableEntry& other) const{
    return key == other.key && comment == other.comment && values == other.values;
}

const StringTableEntry* StringTable::find(const std::string& key) const{
    for (const StringTableEntry& entry : entries){
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

StringTableEntry* StringTable::find(const std::string& key){
    for (StringTableEntry& entry : entries){
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

bool StringTable::erase(const std::string& key){
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const StringTableEntry& e){ return e.key == key; });
    if (it == entries.end()) return false;
    entries.erase(it);
    return true;
}

std::vector<std::string> StringTable::languages() const{
    std::vector<std::string> out;
    for (const StringTableEntry& entry : entries){
        for (const auto& value : entry.values){
            if (std::none_of(out.begin(), out.end(), [&](const std::string& o){ return stSameLanguage(o, value.first); })) out.push_back(value.first);
        }
    }
    return out;
}

StringTable StringTable::onlyLanguages(const std::vector<std::string>& languages) const{
    if (languages.empty()) return *this;
    StringTable out;
    for (const StringTableEntry& entry : entries){
        StringTableEntry kept = entry;
        kept.values.clear();
        for (const auto& value : entry.values){
            if (std::any_of(languages.begin(), languages.end(), [&](const std::string& l){ return stSameLanguage(l, value.first); })) kept.values.push_back(value);
        }
        out.entries.push_back(std::move(kept));
    }
    return out;
}

bool StringTable::parse(const std::string& text, StringTable& out, std::string* error, std::vector<std::string>* problems){
    out = StringTable();
    const StOrderedJson json = StOrderedJson::parse(text, nullptr, false);
    auto fail = [&](const std::string& why){
        if (error) *error = why;
        return false;
    };
    if (json.is_discarded() || !json.is_object()) return fail("not a string table (the file is not a JSON object)");
    auto versionField = json.find("version");
    const int version = versionField != json.end() && versionField->is_number_integer() ? versionField->get<int>() : 0;
    if (version < 1) return fail("not a string table (no \"version\")");
    if (version > VERSION) return fail("a string table of version " + std::to_string(version) + ": this engine reads version " + std::to_string(VERSION));
    auto list = json.find("entries");
    if (list == json.end() || !list->is_array()) return fail("not a string table (no \"entries\" list)");
    auto problem = [&](const std::string& why){
        if (problems) problems->push_back(why);
    };
    for (size_t index = 0; index < list->size(); index++){
        const StOrderedJson& item = (*list)[index];
        const std::string where = "entry " + std::to_string(index + 1);
        if (!item.is_object() || !item.contains("key") || !item["key"].is_string()){
            problem(where + " has no \"key\" (left out)");
            continue;
        }
        StringTableEntry entry;
        entry.key = item["key"].get<std::string>();
        const std::string keyWhy = keyProblem(entry.key);
        if (!keyWhy.empty()){
            problem(where + ": " + keyWhy + " (left out)");
            continue;
        }
        if (out.find(entry.key)){
            problem("the key '" + entry.key + "' is defined twice in the table (the first one is used)");
            continue;
        }
        if (item.contains("comment") && item["comment"].is_string()) entry.comment = item["comment"].get<std::string>();
        auto values = item.find("values");
        if (values != item.end() && values->is_object()){
            for (auto it = values->begin(); it != values->end(); ++it){
                std::string why;
                if (!TitleArtLanguage::isValid(it.key(), &why)){
                    problem("'" + entry.key + "': '" + it.key() + "' is not a language tag (left out)");
                    continue;
                }
                StringTableValue value;
                if (!stReadValue(*it, value, why)){
                    problem("'" + entry.key + "' " + it.key() + ": " + why + " (left out)");
                    continue;
                }
                entry.set(TitleArtLanguage::canonicalize(it.key()), value);
            }
        }
        out.entries.push_back(std::move(entry));
    }
    return true;
}

std::string StringTable::write() const{
    StOrderedJson json = StOrderedJson::object();
    json["version"] = VERSION;
    StOrderedJson list = StOrderedJson::array();
    for (const StringTableEntry& entry : entries){
        StOrderedJson item = StOrderedJson::object();
        item["key"] = entry.key;
        if (!entry.comment.empty()) item["comment"] = entry.comment;
        StOrderedJson values = StOrderedJson::object();
        for (const auto& value : entry.values) values[value.first] = stValueJson(value.second);
        item["values"] = values;
        list.push_back(std::move(item));
    }
    json["entries"] = list;
    return json.dump(2, ' ', false, StOrderedJson::error_handler_t::replace) + "\n";
}

std::string StringTable::keyProblem(const std::string& key){
    if (key.empty()) return "a key cannot be empty";
    if (key.find(':') != std::string::npos) return "the key '" + key + "' has a ':' (\"Table:key\" addresses a table)";
    if (std::isspace((unsigned char)key.front()) || std::isspace((unsigned char)key.back())) return "the key '" + key + "' starts or ends with a space";
    return "";
}

std::string StringTable::nameOfPath(const std::string& path){
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}
