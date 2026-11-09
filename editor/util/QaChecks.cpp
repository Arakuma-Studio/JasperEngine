// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "QaChecks.h"

#include <cmath>

using namespace jasperengine::editor;

qa::Json qa::row(const std::string& id, const std::string& kind){
    return {{"id", id}, {"kind", kind}, {"status", "SKIP"}, {"round", 0}, {"measured", nullptr}, {"threshold", nullptr},
            {"threshold_source", nullptr}, {"limited_by", nullptr}, {"at", Json::array()}, {"fix_hint", nullptr}, {"method", ""}};
}

void qa::addAt(Json& row, double seconds){
    if ((int)row["at"].size() < MAX_AT) row["at"].push_back(std::round(seconds * 1000.0) / 1000.0);
}

std::string qa::verdict(const Json& rows){
    bool fail = false, warn = false, pass = false;
    for (const Json& r : rows){
        const std::string status = r.value("status", "");
        fail = fail || status == "FAIL";
        warn = warn || status == "WARN";
        pass = pass || status == "PASS";
    }
    return fail ? "FAIL" : warn ? "WARN" : pass ? "PASS" : "SKIP";
}

bool qa::reaches(const std::string& verdict, const std::string& failOn){
    if (failOn == "warn") return verdict == "FAIL" || verdict == "WARN";
    if (failOn == "fail") return verdict == "FAIL";
    return false;
}

bool qa::countRounds(Json& rows, const std::map<std::string, int>& previous, int fixRounds){
    bool stop = false;
    for (Json& r : rows){
        auto found = previous.find(r.value("id", ""));
        const int before = found == previous.end() ? 0 : found->second;
        const int round = r.value("status", "") == "FAIL" ? before + 1 : 0;
        r["round"] = round;
        stop = stop || round > fixRounds;
    }
    return stop;
}

std::map<std::string, int> qa::FailRounds::previous(const std::string& scope) const{
    std::map<std::string, int> out;
    const std::string prefix = scope + "|";
    for (auto it = rounds.lower_bound(prefix); it != rounds.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it){
        out[it->first.substr(prefix.size())] = it->second;
    }
    return out;
}

void qa::FailRounds::record(const std::string& scope, const Json& rows){
    for (const Json& r : rows){
        const std::string key = scope + "|" + r.value("id", "");
        if (r.value("status", "") == "FAIL") rounds[key] = r.value("round", 1);
        else rounds.erase(key);
    }
}

void qa::ThresholdAudit::add(const std::string& key, const Json& value, const Json& standard){
    values[key] = value;
    if (value != standard) changed.push_back({{"key", key}, {"value", value}, {"default", standard}});
}

qa::Json qa::ThresholdAudit::json() const{
    return {{"values", values}, {"changed", changed}};
}

std::string qa::guidance(bool stop, bool anyFail, int fixRounds, const std::string& retry, const std::string& closing){
    std::string text;
    if (stop){
        text = "A check failed " + std::to_string(fixRounds + 1) + " times in a row: stop fixing, and report what it measured to the user. ";
    }else if (anyFail){
        text = "Fix what fix_hint says, " + retry + ". Never loosen a threshold. ";
    }
    return text + closing;
}

std::string qa::rowLine(const Json& row){
    const Json& measured = row.contains("measured") ? row["measured"] : Json();
    std::string text = row.value("status", "") + " " + row.value("id", "") + ": " + (measured.is_null() ? std::string("not measured") : measured.dump());
    if (row.contains("limited_by") && row["limited_by"].is_string()) text += ". " + row["limited_by"].get<std::string>();
    if (row.contains("fix_hint") && row["fix_hint"].is_string()) text += " " + row["fix_hint"].get<std::string>();
    return text;
}
