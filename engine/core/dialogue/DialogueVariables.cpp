// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueVariables.h"

#include "Log.h"
#include "dialogue/DialogueProgram.h"
#include "io/Data.h"
#include "save/SaveGameStore.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace jasperengine;

namespace jasperengine::dialogue {

FunctionSubscribe<void(const std::string&, const Value&)> Variables::onChanged;

namespace {

    using Json = nlohmann::ordered_json;

    struct Store {
        std::map<std::string, Value, std::less<>> values;
        // identity -> node (or node.option) -> count
        std::map<std::string, std::map<std::string, uint32_t, std::less<>>, std::less<>> visits;
        std::vector<std::pair<std::string, Value>> projectDefaults;
        bool projectDefaultsKnown = false;     // set by the editor, else read from the game's file once
        // by id; a removal while they are told empties the slot, compacted afterwards
        std::vector<std::pair<int, std::function<void(const std::string&, const Value&)>>> listeners;
        int nextListener = 1;
        int notifying = 0;
    };

    Store& store() {
        static Store instance;
        return instance;
    }

    Json toJsonValue(const Value& value) {
        if (value.isBool()) return value.asBool();
        if (value.isNumber()) return value.asNumber();
        if (value.isString()) return value.asString();
        return nullptr;
    }

    bool fromJsonValue(const Json& json, Value& out) {
        if (json.is_boolean()) out = Value(json.get<bool>());
        else if (json.is_number()) out = Value(json.get<double>());
        else if (json.is_string()) out = Value(json.get<std::string>());
        else return false;
        return true;
    }

    void tell(const std::string& name, const Value& value) {
        Store& s = store();
        // by index: a listener may add (append) or remove (empty its slot) listeners; nothing allocates
        s.notifying++;
        for (size_t i = 0; i < s.listeners.size(); i++) {
            if (s.listeners[i].second) s.listeners[i].second(name, value);
        }
        if (--s.notifying == 0) {
            s.listeners.erase(std::remove_if(s.listeners.begin(), s.listeners.end(), [](const auto& slot) { return !slot.second; }), s.listeners.end());
        }
        Variables::onChanged.call(name, value);
    }

}

Value Variables::get(std::string_view name) {
    const Store& s = store();
    auto it = s.values.find(name);
    return it == s.values.end() ? Value() : it->second;
}

Value Variables::read(std::string_view name, const Program* program) {
    Store& s = store();
    auto it = s.values.find(name);
    if (it != s.values.end()) return it->second;
    const Value* fallback = program ? program->defaultOf(name) : nullptr;
    if (!fallback) fallback = projectDefault(name);
    if (!fallback) return Value();
    s.values.emplace(std::string(name), *fallback);
    return *fallback;
}

void Variables::set(std::string_view name, const Value& value) {
    Store& s = store();
    auto it = s.values.find(name);
    if (it != s.values.end()) {
        if (it->second.same(value)) return;
        it->second = value;
    } else {
        it = s.values.emplace(std::string(name), value).first;
    }
    tell(it->first, it->second);
}

bool Variables::has(std::string_view name) {
    return store().values.count(name) > 0;
}

std::vector<std::pair<std::string, Value>> Variables::all() {
    std::vector<std::pair<std::string, Value>> out;
    for (const auto& [name, value] : store().values) out.emplace_back(name, value);
    return out;
}

uint32_t Variables::visits(std::string_view identity, std::string_view node) {
    const Store& s = store();
    auto file = s.visits.find(identity);
    if (file == s.visits.end()) return 0;
    auto it = file->second.find(node);
    return it == file->second.end() ? 0 : it->second;
}

void Variables::addVisit(std::string_view identity, std::string_view node) {
    Store& s = store();
    auto file = s.visits.find(identity);
    if (file == s.visits.end()) file = s.visits.emplace(std::string(identity), std::map<std::string, uint32_t, std::less<>>()).first;
    auto it = file->second.find(node);
    if (it == file->second.end()) file->second.emplace(std::string(node), 1u);
    else it->second++;
}

void Variables::resetVisits() {
    store().visits.clear();
}

Json Variables::toJson() {
    const Store& s = store();
    Json values = Json::object();
    for (const auto& [name, value] : s.values) {
        if (!value.isNil()) values[name] = toJsonValue(value);
    }
    Json visits = Json::object();
    for (const auto& [identity, nodes] : s.visits) {
        for (const auto& [node, count] : nodes) visits[identity + "#" + node] = count;
    }
    return Json{{"variables", values}, {"visits", visits}};
}

bool Variables::fromJson(const Json& json, std::string* error, bool notify) {
    auto failWith = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    // one of the two keys at least: another namespace of a save slot is not taken for an empty state
    if (!json.is_object() || (!json.contains("variables") && !json.contains("visits"))) {
        return failWith("the dialogue state is an object {variables, visits}");
    }
    std::map<std::string, Value, std::less<>> values;
    if (auto it = json.find("variables"); it != json.end()) {
        if (!it->is_object()) return failWith("variables is an object of name -> bool, number or string");
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            Value value;
            if (!fromJsonValue(entry.value(), value)) return failWith("variable '" + entry.key() + "' is not a bool, number or string");
            values[entry.key()] = value;
        }
    }
    std::map<std::string, std::map<std::string, uint32_t, std::less<>>, std::less<>> visits;
    if (auto it = json.find("visits"); it != json.end()) {
        if (!it->is_object()) return failWith("visits is an object of \"<dialogue>#<node>\" -> count");
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            const size_t hash = entry.key().rfind('#');
            // a save slot's JSON view gives signed integers
            if (hash == std::string::npos || !entry.value().is_number_integer() || entry.value().get<int64_t>() < 0) {
                return failWith("visit '" + entry.key() + "' is not \"<dialogue>#<node>\": count");
            }
            visits[entry.key().substr(0, hash)][entry.key().substr(hash + 1)] = entry.value().get<uint32_t>();
        }
    }
    Store& s = store();
    std::map<std::string, Value, std::less<>> before = std::move(s.values);
    s.values = std::move(values);
    s.visits = std::move(visits);
    if (!notify) return true;
    // what changed is told, as a set would
    for (const auto& [name, value] : s.values) {
        auto old = before.find(name);
        if (old == before.end() || !old->second.same(value)) tell(name, value);
    }
    for (const auto& [name, value] : before) {
        if (!s.values.count(name)) tell(name, Value());
    }
    return true;
}

void Variables::setProjectDefaults(std::vector<std::pair<std::string, Value>> defaults) {
    Store& s = store();
    s.projectDefaults = std::move(defaults);
    s.projectDefaultsKnown = true;
}

bool Variables::parseSettings(std::string_view text, std::vector<std::pair<std::string, Value>>& out, std::string* error) {
    const Json json = Json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("format", std::string()) != SETTINGS_FORMAT) {
        if (error) *error = std::string("not a ") + SETTINGS_FORMAT + " file";
        return false;
    }
    out.clear();
    if (auto variables = json.find("variables"); variables != json.end() && variables->is_object()) {
        for (auto it = variables->begin(); it != variables->end(); ++it) {
            Value value;
            if (fromJsonValue(it.value(), value)) out.emplace_back(it.key(), value);
        }
    }
    return true;
}

const Value* Variables::projectDefault(std::string_view name) {
    Store& s = store();
    if (!s.projectDefaultsKnown) {
        // a game's Project Settings > Dialogue > Variables, written by the export when there are any
        s.projectDefaultsKnown = true;
        Data data;
        if (data.open(SETTINGS_FILE) == FileErrors::FILEDATA_OK) {
            std::string error;
            if (!parseSettings(data.readString(), s.projectDefaults, &error)) Log::error("%s: %s", SETTINGS_FILE, error.c_str());
        }
    }
    for (const auto& [variable, value] : s.projectDefaults) {
        if (variable == name) return &value;
    }
    return nullptr;
}

void Variables::reset() {
    Store& s = store();
    s.values.clear();
    s.visits.clear();
}

int Variables::addListener(std::function<void(const std::string&, const Value&)> listener) {
    Store& s = store();
    const int id = s.nextListener++;
    s.listeners.emplace_back(id, std::move(listener));
    return id;
}

void Variables::removeListener(int id) {
    Store& s = store();
    for (auto& slot : s.listeners) {
        if (slot.first == id) slot.second = nullptr;
    }
    if (s.notifying == 0) {
        s.listeners.erase(std::remove_if(s.listeners.begin(), s.listeners.end(), [](const auto& slot) { return !slot.second; }), s.listeners.end());
    }
}

std::string SaveHooks::capture() {
    return Variables::toJson().dump();
}

bool SaveHooks::restore(std::string_view text, std::string* error) {
    const Json json = Json::parse(text.begin(), text.end(), nullptr, false);
    if (json.is_discarded()) {
        if (error) *error = "the dialogue state is not JSON";
        return false;
    }
    return Variables::fromJson(json, error);
}

namespace {

    // SaveGame's JSON view (savegame.md R21) doubles a table key's leading '#' or '$'
    std::string savedKey(const std::string& key) {
        return !key.empty() && (key[0] == '#' || key[0] == '$') ? key.substr(0, 1) + key : key;
    }

    std::string loadedKey(const std::string& key) {
        return key.size() > 1 && (key[0] == '#' || key[0] == '$') && key[1] == key[0] ? key.substr(1) : key;
    }

    // Objects and scalars, the keys renamed; an integral number becomes an integer, as Lua's
    // toTable gives it, so a slot reads the same whichever side saved it
    template<class To, class From>
    To convertJson(const From& in, std::string (*key)(const std::string&)) {
        if (in.is_object()) {
            To out = To::object();
            for (auto it = in.begin(); it != in.end(); ++it) out[key(it.key())] = convertJson<To>(it.value(), key);
            return out;
        }
        if (in.is_array()) {
            To out = To::array();
            for (const auto& item : in) out.push_back(convertJson<To>(item, key));
            return out;
        }
        if (in.is_boolean()) return To(in.template get<bool>());
        if (in.is_number_unsigned()) return To(in.template get<uint64_t>());
        if (in.is_number_integer()) return To(in.template get<int64_t>());
        if (in.is_number()) {
            const double number = in.template get<double>();
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) return To(static_cast<int64_t>(number));
            return To(number);
        }
        if (in.is_string()) return To(in.template get<std::string>());
        return To();
    }

}

savegame::SaveError SaveHooks::save(std::string_view slot, std::string_view nameSpace, std::function<void(const savegame::SaveError&)> onComplete) {
    std::string payload;
    savegame::SaveError error;
    if (!savegame::fromJson(convertJson<nlohmann::json>(Variables::toJson(), savedKey), payload, error)) return error;
    savegame::WriteOptions options;
    options.version = savegame::gameDataVersion();
    return savegame::SaveGameStore::instance().write(slot, nameSpace, std::move(payload), options, std::move(onComplete));
}

savegame::SaveError SaveHooks::load(std::string_view slot, std::string_view nameSpace) {
    std::string payload;
    savegame::ReadInfo info;
    savegame::SaveError error = savegame::SaveGameStore::instance().read(slot, nameSpace, payload, info);
    if (!error.ok()) return error;
    nlohmann::json json;
    if (!savegame::toJson(payload, json, error)) return error;
    std::string problem;
    if (!Variables::fromJson(convertJson<Json>(json, loadedKey), &problem)) {
        return savegame::makeError(savegame::SaveErrorCode::Corrupt, "namespace \"" + std::string(nameSpace) + "\" of slot \"" +
                                   std::string(slot) + "\" is not a dialogue state: " + problem);
    }
    return {};
}

}  // namespace jasperengine::dialogue
