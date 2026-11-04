// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GpuTierDatabase.h"

#include "Log.h"
#include "json.hpp"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace jasperengine;
using nlohmann::json;

namespace {

    bool gpuNameWordChar(char c){
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }

    uint32_t gpuParseHex(const std::string& text){
        if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
            return 0;
        return (uint32_t)std::strtoul(text.c_str() + 2, nullptr, 16);
    }

    // the end of `pattern` matched at `pos` of `text`, or npos (the converter's match_at)
    size_t gpuPatternMatchAt(const std::string& pattern, const std::string& text, size_t pos){
        size_t i = 0;
        while (i < pattern.size()){
            if (pos >= text.size())
                return std::string::npos;
            const char c = pattern[i];
            const char ch = text[pos];
            if (c == '\\'){
                if (ch < '0' || ch > '9')
                    return std::string::npos;
                i += 2;
            }else if (c == '['){
                const size_t end = pattern.find(']', i);
                if (end == std::string::npos)
                    return std::string::npos;
                bool ok = false;
                for (size_t k = i + 1; k < end;){
                    if (k + 2 < end && pattern[k + 1] == '-'){
                        ok = ok || (ch >= pattern[k] && ch <= pattern[k + 2]);
                        k += 3;
                    }else{
                        ok = ok || ch == pattern[k];
                        k++;
                    }
                }
                if (!ok)
                    return std::string::npos;
                i = end + 1;
            }else if (c == '.'){
                i++;
            }else{
                if (c != ch)
                    return std::string::npos;
                i++;
            }
            pos++;
        }
        return pos;
    }

    bool gpuTierFromJson(const json& value, DeviceTier& tier){
        return value.is_string() && DeviceNames::tierFromName(value.get<std::string>().c_str(), tier);
    }

    std::string gpuStringOf(const json& object, const char* key){
        auto it = object.find(key);
        return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

}

const GpuTierDatabase& GpuTierDatabase::builtIn(){
    static const GpuTierDatabase* database = [](){
        GpuTierDatabase* parsed = new GpuTierDatabase();
        std::string error;
        if (!parsed->load(embeddedJson(), error))
            Log::error("GPU tier database: %s (every GPU gets its platform's heuristic tier)", error.c_str());
        return parsed;
    }();
    return *database;
}

std::string GpuTierDatabase::normalize(const std::string& text){
    std::string lower;
    lower.reserve(text.size());
    for (char c : text)
        lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    for (const char* token : {"(tm)", "(r)"}){
        const size_t length = std::strlen(token);
        for (size_t at = lower.find(token); at != std::string::npos; at = lower.find(token, at))
            lower.erase(at, length);
    }
    std::string out;
    out.reserve(lower.size());
    bool space = false;
    for (char c : lower){
        if (gpuNameWordChar(c)){
            if (space && !out.empty())
                out += ' ';
            out += c;
            space = false;
        }else{
            space = true;
        }
    }
    return out;
}

bool GpuTierDatabase::familyMatches(const std::string& pattern, const std::string& normalized){
    for (size_t start = 0; start < normalized.size(); start++){
        if (start > 0 && normalized[start - 1] != ' ')
            continue;
        const size_t end = gpuPatternMatchAt(pattern, normalized, start);
        if (end != std::string::npos && (end == normalized.size() || normalized[end] == ' '))
            return true;
    }
    return false;
}

bool GpuTierDatabase::load(const std::string& text, std::string& error){
    rules.clear();
    vendors.clear();
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()){
        error = "gpu_tiers.json is not a JSON object";
        return false;
    }
    if (!parsed.contains("version") || !parsed["version"].is_number_integer() || parsed["version"].get<int>() != 1){
        error = "gpu_tiers.json has an unknown version (1 expected)";
        return false;
    }
    auto vendorList = parsed.find("vendors");
    if (vendorList != parsed.end() && vendorList->is_array()){
        for (const json& item : *vendorList){
            if (!item.is_object())
                continue;
            Vendor vendor;
            vendor.name = gpuStringOf(item, "name");
            vendor.id = gpuParseHex(gpuStringOf(item, "id"));
            auto tokens = item.find("tokens");
            if (tokens != item.end() && tokens->is_array()){
                for (const json& token : *tokens)
                    if (token.is_string())
                        vendor.tokens.push_back(token.get<std::string>());
            }
            if (!vendor.name.empty())
                vendors.push_back(std::move(vendor));
        }
    }
    auto ruleList = parsed.find("rules");
    if (ruleList == parsed.end() || !ruleList->is_array()){
        error = "gpu_tiers.json has no rules";
        return false;
    }
    size_t index = 0;
    for (const json& item : *ruleList){
        index++;
        Rule rule;
        if (!item.is_object() || !item.contains("tier") || !gpuTierFromJson(item["tier"], rule.tier)){
            error = "rule " + std::to_string(index) + " has no tier of low, medium, high or ultra";
            rules.clear();
            return false;
        }
        rule.platform = gpuStringOf(item, "platform");
        rule.vendor = gpuStringOf(item, "vendor");
        const std::string kind = gpuStringOf(item, "kind");
        if (kind == "name"){
            rule.kind = Rule::Kind::Name;
            rule.text = gpuStringOf(item, "name");
            auto devices = item.find("devices");
            if (devices != item.end() && devices->is_array()){
                for (const json& device : *devices)
                    if (device.is_string())
                        rule.devices.push_back(gpuParseHex(device.get<std::string>()));
            }
        }else if (kind == "family"){
            rule.kind = Rule::Kind::Family;
            rule.text = gpuStringOf(item, "pattern");
        }else if (kind == "vendor"){
            rule.kind = Rule::Kind::Vendor;
        }else{
            error = "rule " + std::to_string(index) + " has the unknown kind \"" + kind + "\"";
            rules.clear();
            return false;
        }
        if (rule.kind != Rule::Kind::Vendor && rule.text.empty()){
            error = "rule " + std::to_string(index) + " has no name or pattern";
            rules.clear();
            return false;
        }
        rules.push_back(std::move(rule));
    }
    return true;
}

bool GpuTierDatabase::platformApplies(const std::string& rulePlatform, const std::string& queryPlatform){
    return rulePlatform == "any" || queryPlatform == "web" || rulePlatform == queryPlatform;
}

std::string GpuTierDatabase::vendorOf(const std::string& normalized, uint32_t vendorId) const{
    if (vendorId != 0){
        for (const Vendor& vendor : vendors)
            if (vendor.id == vendorId)
                return vendor.name;
    }
    const std::string padded = " " + normalized + " ";
    for (const Vendor& vendor : vendors){
        for (const std::string& token : vendor.tokens){
            if (padded.find(" " + token + " ") != std::string::npos)
                return vendor.name;
        }
    }
    return std::string();
}

uint32_t GpuTierDatabase::vendorIdOf(const std::string& vendor) const{
    for (const Vendor& entry : vendors)
        if (entry.name == vendor)
            return entry.id;
    return 0;
}

GpuMatch GpuTierDatabase::match(const GpuQuery& query) const{
    GpuMatch result;
    const std::string text = normalize(query.renderer);

    // ANGLE's renderer string (Chrome, Edge, Firefox on Windows) carries the device id:
    // "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002487) Direct3D11 ...)"
    uint32_t deviceId = query.deviceId;
    if (deviceId == 0 && text.rfind("angle ", 0) == 0){
        for (size_t at = text.find(" 0x"); at != std::string::npos; at = text.find(" 0x", at + 1)){
            const size_t end = text.find(' ', at + 1);
            const uint32_t id = gpuParseHex(text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
            if (id != 0){
                deviceId = id;
                break;
            }
        }
    }
    result.vendor = vendorOf(text, query.vendorId);
    const uint32_t vendorId = query.vendorId != 0 ? query.vendorId : vendorIdOf(result.vendor);

    if (deviceId != 0 && vendorId != 0){
        for (const Rule& rule : rules){
            if (rule.kind != Rule::Kind::Name || !platformApplies(rule.platform, query.platform) || vendorIdOf(rule.vendor) != vendorId)
                continue;
            for (uint32_t device : rule.devices){
                if (device == deviceId){
                    result.matched = true;
                    result.tier = rule.tier;
                    result.kind = "device";
                    result.rule = rule.text;
                    return result;
                }
            }
        }
    }

    const std::string padded = " " + text + " ";
    const Rule* best = nullptr;
    for (const Rule& rule : rules){
        if (rule.kind != Rule::Kind::Name || !platformApplies(rule.platform, query.platform))
            continue;
        if ((!best || rule.text.size() > best->text.size()) && padded.find(" " + rule.text + " ") != std::string::npos)
            best = &rule;
    }
    if (best){
        result.matched = true;
        result.tier = best->tier;
        result.kind = "name";
        result.rule = best->text;
        return result;
    }

    for (const Rule& rule : rules){
        if (rule.kind == Rule::Kind::Family && platformApplies(rule.platform, query.platform) && familyMatches(rule.text, text)){
            result.matched = true;
            result.tier = rule.tier;
            result.kind = "family";
            result.rule = rule.text;
            return result;
        }
    }

    if (!result.vendor.empty()){
        for (const Rule& rule : rules){
            // not on the web: a browser's masked "Apple GPU" says nothing of the device's age
            if (rule.kind == Rule::Kind::Vendor && (rule.platform == "any" || rule.platform == query.platform) && rule.vendor == result.vendor){
                result.matched = true;
                result.tier = rule.tier;
                result.kind = "vendor";
                result.rule = rule.vendor;
                return result;
            }
        }
    }
    return result;
}
