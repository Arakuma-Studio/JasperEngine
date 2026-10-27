// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputActionAssetJson.h"

#include "Input.h"
#include "input/InputControlPath.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
#include <stdexcept>

using namespace jasperengine;

namespace {

    const char* const MAP_KEYS[] = {"name", "priority", "enabled", "consume", "exclusive", "ignoreWhenUIConsumes", "actions"};
    const char* const ACTION_KEYS[] = {"name", "type", "pressPoint", "holdTime", "deadzone", "bindings"};
    const char* const BINDING_KEYS[] = {"id", "path", "scale", "invert", "deadzone", "requiredMods", "forbiddenMods", "groups",
                                        "composite", "normalize", "parts"};

    template<size_t N>
    std::string passthroughOf(const InputJson& json, const char* const (&known)[N]){
        InputJson rest = InputJson::object();
        for (auto it = json.begin(); it != json.end(); ++it){
            if (std::find_if(std::begin(known), std::end(known), [&](const char* key){ return it.key() == key; }) == std::end(known))
                rest[it.key()] = it.value();
        }
        return rest.empty() ? std::string() : rest.dump();
    }

    void appendPassthrough(InputJson& out, const std::string& passthrough){
        if (passthrough.empty())
            return;
        const InputJson rest = InputJson::parse(passthrough, nullptr, false);
        if (!rest.is_object())
            return;
        for (auto it = rest.begin(); it != rest.end(); ++it)
            out[it.key()] = it.value();
    }

    bool readString(const InputJson& json, const char* key, std::string& out, std::string& error, const std::string& where, bool required){
        if (!json.contains(key)){
            if (required){
                error = where + ": \"" + key + "\" is missing";
                return false;
            }
            return true;
        }
        if (!json[key].is_string()){
            error = where + ": \"" + key + "\" must be a string";
            return false;
        }
        out = json[key].get<std::string>();
        return true;
    }

    bool readNumber(const InputJson& json, const char* key, float& out, std::string& error, const std::string& where){
        if (!json.contains(key))
            return true;
        if (!json[key].is_number()){
            error = where + ": \"" + key + "\" must be a number";
            return false;
        }
        out = json[key].get<float>();
        return true;
    }

    bool readBool(const InputJson& json, const char* key, bool& out, std::string& error, const std::string& where){
        if (!json.contains(key))
            return true;
        if (!json[key].is_boolean()){
            error = where + ": \"" + key + "\" must be true or false";
            return false;
        }
        out = json[key].get<bool>();
        return true;
    }

    bool readMods(const InputJson& json, const char* key, int& out, std::string& error, const std::string& where){
        if (!json.contains(key))
            return true;
        const InputJson& value = json[key];
        if (!value.is_array()){
            error = where + ": \"" + key + "\" must be a list of shift, ctrl, alt, super";
            return false;
        }
        std::vector<std::string> names;
        for (const InputJson& item : value){
            if (!item.is_string() || inputModifiersFromNames({item.get<std::string>()}) == 0){
                error = where + ": \"" + key + "\" has " + item.dump() + " (shift, ctrl, alt, super)";
                return false;
            }
            names.push_back(item.get<std::string>());
        }
        out = inputModifiersFromNames(names);
        return true;
    }

    bool compositeFromName(const std::string& name, InputCompositeType& type){
        if (name == "axis1d"){ type = InputCompositeType::AXIS_1D; return true; }
        if (name == "dpad2d"){ type = InputCompositeType::DPAD_2D; return true; }
        return false;
    }

    bool bindingFromJson(const InputJson& json, InputBindingDesc& out, std::string& error, const std::string& where, bool isPart){
        if (!json.is_object()){
            error = where + " must be an object";
            return false;
        }
        out = InputBindingDesc();
        if (!readString(json, "id", out.id, error, where, false)) return false;
        if (!readString(json, "path", out.path, error, where, false)) return false;
        if (!readNumber(json, "scale", out.scale, error, where)) return false;
        if (!readBool(json, "invert", out.invert, error, where)) return false;
        if (json.contains("deadzone")){
            float deadzone = 0.0f;
            if (!readNumber(json, "deadzone", deadzone, error, where)) return false;
            out.deadzone = deadzone;
        }
        if (!readMods(json, "requiredMods", out.requiredMods, error, where)) return false;
        if (!readMods(json, "forbiddenMods", out.forbiddenMods, error, where)) return false;
        if (json.contains("groups")){
            if (!json["groups"].is_array()){
                error = where + ": \"groups\" must be a list of strings";
                return false;
            }
            for (const InputJson& group : json["groups"]){
                if (!group.is_string()){
                    error = where + ": \"groups\" must be a list of strings";
                    return false;
                }
                out.groups.push_back(group.get<std::string>());
            }
        }
        if (json.contains("composite")){
            std::string name;
            if (!readString(json, "composite", name, error, where, false)) return false;
            if (isPart || !compositeFromName(name, out.composite)){
                error = where + ": \"composite\" must be axis1d or dpad2d" + std::string(isPart ? " (and not on a part)" : "");
                return false;
            }
            if (!readBool(json, "normalize", out.normalize, error, where)) return false;
            const InputJson parts = json.value("parts", InputJson::object());
            if (!parts.is_object()){
                error = where + ": \"parts\" must be an object";
                return false;
            }
            for (const std::string& part : inputCompositeParts(out.composite)){
                InputBindingDesc desc;
                if (parts.contains(part)){
                    if (!bindingFromJson(parts[part], desc, error, where + " part " + part, true))
                        return false;
                }
                out.parts.emplace_back(part, desc);
            }
            for (auto it = parts.begin(); it != parts.end(); ++it){
                const std::vector<std::string> names = inputCompositeParts(out.composite);
                if (std::find(names.begin(), names.end(), it.key()) == names.end()){
                    error = where + ": " + std::string(inputCompositeName(out.composite)) + " has no part \"" + it.key() + "\"";
                    return false;
                }
            }
        }
        out.passthrough = passthroughOf(json, BINDING_KEYS);
        return true;
    }

    InputJson bindingToJson(const InputBindingDesc& binding, bool isPart){
        InputJson json = InputJson::object();
        if (!binding.id.empty())
            json["id"] = binding.id;
        if (binding.isComposite()){
            json["composite"] = inputCompositeName(binding.composite);
            if (!binding.normalize)
                json["normalize"] = false;
            InputJson parts = InputJson::object();
            for (const auto& [part, desc] : binding.parts)
                parts[part] = bindingToJson(desc, true);
            json["parts"] = parts;
        }else{
            json["path"] = binding.path;
        }
        if (binding.scale != 1.0f)
            json["scale"] = binding.scale;
        if (binding.invert)
            json["invert"] = true;
        if (binding.deadzone)
            json["deadzone"] = *binding.deadzone;
        if (binding.requiredMods != 0)
            json["requiredMods"] = inputModifierNames(binding.requiredMods);
        if (binding.forbiddenMods != 0)
            json["forbiddenMods"] = inputModifierNames(binding.forbiddenMods);
        if (!binding.groups.empty() && !isPart)
            json["groups"] = binding.groups;
        appendPassthrough(json, binding.passthrough);
        return json;
    }

    template<typename F>
    void forEachBinding(InputActionAssetDesc& asset, F&& visit){
        for (InputActionMapDesc& map : asset.maps){
            for (InputActionDesc& action : map.actions){
                for (InputBindingDesc& binding : action.bindings){
                    visit(binding);
                    for (auto& part : binding.parts)
                        visit(part.second);
                }
            }
        }
    }

}

bool InputBindingDesc::inGroup(const std::string& group) const{
    return std::find(groups.begin(), groups.end(), group) != groups.end();
}

int jasperengine::inputModifiersFromNames(const std::vector<std::string>& names){
    int mods = 0;
    for (const std::string& name : names){
        if (name == "shift") mods |= D_MODIFIER_SHIFT;
        else if (name == "ctrl" || name == "control") mods |= D_MODIFIER_CONTROL;
        else if (name == "alt") mods |= D_MODIFIER_ALT;
        else if (name == "super" || name == "cmd" || name == "meta") mods |= D_MODIFIER_SUPER;
    }
    return mods;
}

std::vector<std::string> jasperengine::inputModifierNames(int mods){
    std::vector<std::string> names;
    if (mods & D_MODIFIER_CONTROL) names.push_back("ctrl");
    if (mods & D_MODIFIER_SHIFT) names.push_back("shift");
    if (mods & D_MODIFIER_ALT) names.push_back("alt");
    if (mods & D_MODIFIER_SUPER) names.push_back("super");
    return names;
}

const char* jasperengine::inputActionTypeName(InputActionType type){
    switch (type){
        case InputActionType::BUTTON: return "BUTTON";
        case InputActionType::AXIS: return "AXIS";
        case InputActionType::VECTOR2: return "VECTOR2";
    }
    return "BUTTON";
}

bool jasperengine::inputActionTypeFromName(const std::string& name, InputActionType& type){
    std::string upper = name;
    for (char& c : upper)
        c = (char)std::toupper(static_cast<unsigned char>(c));
    if (upper == "BUTTON"){ type = InputActionType::BUTTON; return true; }
    if (upper == "AXIS"){ type = InputActionType::AXIS; return true; }
    if (upper == "VECTOR2"){ type = InputActionType::VECTOR2; return true; }
    return false;
}

const char* jasperengine::inputCompositeName(InputCompositeType type){
    switch (type){
        case InputCompositeType::AXIS_1D: return "axis1d";
        case InputCompositeType::DPAD_2D: return "dpad2d";
        case InputCompositeType::NONE: break;
    }
    return "";
}

std::vector<std::string> jasperengine::inputCompositeParts(InputCompositeType type){
    switch (type){
        case InputCompositeType::AXIS_1D: return {"negative", "positive"};
        case InputCompositeType::DPAD_2D: return {"up", "down", "left", "right"};
        case InputCompositeType::NONE: break;
    }
    return {};
}

bool InputActionAssetDesc::parse(const std::string& text, InputActionAssetDesc& out, std::string& error){
    InputJson json = InputJson::parse(text, nullptr, false);
    if (json.is_discarded()){
        error = "not valid JSON";
        return false;
    }
    return inputActionAssetFromJson(json, out, error);
}

bool jasperengine::inputActionAssetFromJson(const InputJson& json, InputActionAssetDesc& out, std::string& error){
    if (!json.is_object()){
        error = "an .inputactions file is a JSON object";
        return false;
    }
    InputActionAssetDesc asset;
    asset.version = json.value("version", InputActionAssetDesc::FORMAT_VERSION);
    if (asset.version > InputActionAssetDesc::FORMAT_VERSION){
        error = "version " + std::to_string(asset.version) + " is newer than this engine reads (" + std::to_string(InputActionAssetDesc::FORMAT_VERSION) + ")";
        return false;
    }
    const InputJson maps = json.value("maps", InputJson::array());
    if (!maps.is_array()){
        error = "\"maps\" must be a list";
        return false;
    }
    std::set<std::string> mapNames;
    for (size_t m = 0; m < maps.size(); m++){
        const InputJson& mapJson = maps[m];
        const std::string mapWhere = "maps[" + std::to_string(m) + "]";
        if (!mapJson.is_object()){
            error = mapWhere + " must be an object";
            return false;
        }
        InputActionMapDesc map;
        if (!readString(mapJson, "name", map.name, error, mapWhere, true)) return false;
        if (map.name.empty() || map.name.find('/') != std::string::npos || !mapNames.insert(map.name).second){
            error = mapWhere + ": the name \"" + map.name + "\" must be unique, not empty and without '/'";
            return false;
        }
        if (mapJson.contains("priority")){
            if (!mapJson["priority"].is_number_integer()){
                error = mapWhere + ": \"priority\" must be an integer";
                return false;
            }
            map.priority = mapJson["priority"].get<int>();
        }
        if (!readBool(mapJson, "enabled", map.enabled, error, mapWhere)) return false;
        if (!readBool(mapJson, "consume", map.consume, error, mapWhere)) return false;
        if (!readBool(mapJson, "exclusive", map.exclusive, error, mapWhere)) return false;
        if (!readBool(mapJson, "ignoreWhenUIConsumes", map.ignoreWhenUIConsumes, error, mapWhere)) return false;
        const InputJson actions = mapJson.value("actions", InputJson::array());
        if (!actions.is_array()){
            error = mapWhere + ": \"actions\" must be a list";
            return false;
        }
        std::set<std::string> actionNames;
        for (size_t a = 0; a < actions.size(); a++){
            const InputJson& actionJson = actions[a];
            const std::string where = map.name + "/actions[" + std::to_string(a) + "]";
            if (!actionJson.is_object()){
                error = where + " must be an object";
                return false;
            }
            InputActionDesc action;
            if (!readString(actionJson, "name", action.name, error, where, true)) return false;
            if (action.name.empty() || action.name.find('/') != std::string::npos || !actionNames.insert(action.name).second){
                error = where + ": the name \"" + action.name + "\" must be unique in its map, not empty and without '/'";
                return false;
            }
            std::string type = "BUTTON";
            if (!readString(actionJson, "type", type, error, where, false)) return false;
            if (!inputActionTypeFromName(type, action.type)){
                error = where + ": \"type\" must be BUTTON, AXIS or VECTOR2";
                return false;
            }
            if (!readNumber(actionJson, "pressPoint", action.pressPoint, error, where)) return false;
            if (!readNumber(actionJson, "holdTime", action.holdTime, error, where)) return false;
            if (actionJson.contains("deadzone")){
                float deadzone = 0.0f;
                if (!readNumber(actionJson, "deadzone", deadzone, error, where)) return false;
                action.deadzone = deadzone;
            }
            const InputJson bindings = actionJson.value("bindings", InputJson::array());
            if (!bindings.is_array()){
                error = where + ": \"bindings\" must be a list";
                return false;
            }
            for (size_t b = 0; b < bindings.size(); b++){
                InputBindingDesc binding;
                if (!bindingFromJson(bindings[b], binding, error, map.name + "/" + action.name + "/bindings[" + std::to_string(b) + "]", false))
                    return false;
                action.bindings.push_back(std::move(binding));
            }
            action.passthrough = passthroughOf(actionJson, ACTION_KEYS);
            map.actions.push_back(std::move(action));
        }
        map.passthrough = passthroughOf(mapJson, MAP_KEYS);
        asset.maps.push_back(std::move(map));
    }
    const char* const assetKeys[] = {"version", "maps"};
    asset.passthrough = passthroughOf(json, assetKeys);
    asset.assignMissingIds();
    out = std::move(asset);
    return true;
}

InputJson jasperengine::inputActionAssetToJson(const InputActionAssetDesc& asset){
    InputJson json = InputJson::object();
    json["version"] = asset.version;
    InputJson maps = InputJson::array();
    for (const InputActionMapDesc& map : asset.maps){
        InputJson mapJson = InputJson::object();
        mapJson["name"] = map.name;
        if (map.priority != 0) mapJson["priority"] = map.priority;
        if (!map.enabled) mapJson["enabled"] = false;
        if (!map.consume) mapJson["consume"] = false;
        if (map.exclusive) mapJson["exclusive"] = true;
        if (!map.ignoreWhenUIConsumes) mapJson["ignoreWhenUIConsumes"] = false;
        InputJson actions = InputJson::array();
        for (const InputActionDesc& action : map.actions){
            InputJson actionJson = InputJson::object();
            actionJson["name"] = action.name;
            actionJson["type"] = inputActionTypeName(action.type);
            if (action.pressPoint != 0.5f) actionJson["pressPoint"] = action.pressPoint;
            if (action.holdTime != 0.0f) actionJson["holdTime"] = action.holdTime;
            if (action.deadzone) actionJson["deadzone"] = *action.deadzone;
            InputJson bindings = InputJson::array();
            for (const InputBindingDesc& binding : action.bindings)
                bindings.push_back(bindingToJson(binding, false));
            actionJson["bindings"] = bindings;
            appendPassthrough(actionJson, action.passthrough);
            actions.push_back(actionJson);
        }
        mapJson["actions"] = actions;
        appendPassthrough(mapJson, map.passthrough);
        maps.push_back(mapJson);
    }
    json["maps"] = maps;
    appendPassthrough(json, asset.passthrough);
    return json;
}

std::string InputActionAssetDesc::save() const{
    return inputActionAssetToJson(*this).dump(2) + "\n";
}

InputActionMapDesc* InputActionAssetDesc::findMap(const std::string& name){
    for (InputActionMapDesc& map : maps){
        if (map.name == name)
            return &map;
    }
    return nullptr;
}

const InputActionMapDesc* InputActionAssetDesc::findMap(const std::string& name) const{
    return const_cast<InputActionAssetDesc*>(this)->findMap(name);
}

InputActionDesc* InputActionAssetDesc::findAction(const std::string& mapSlashAction){
    const size_t slash = mapSlashAction.find('/');
    if (slash == std::string::npos)
        return nullptr;
    InputActionMapDesc* map = findMap(mapSlashAction.substr(0, slash));
    if (!map)
        return nullptr;
    const std::string name = mapSlashAction.substr(slash + 1);
    for (InputActionDesc& action : map->actions){
        if (action.name == name)
            return &action;
    }
    return nullptr;
}

const InputActionDesc* InputActionAssetDesc::findAction(const std::string& mapSlashAction) const{
    return const_cast<InputActionAssetDesc*>(this)->findAction(mapSlashAction);
}

InputBindingDesc* InputActionAssetDesc::findBinding(const std::string& id){
    InputBindingDesc* found = nullptr;
    forEachBinding(*this, [&](InputBindingDesc& binding){
        if (!found && binding.id == id)
            found = &binding;
    });
    return found;
}

const InputBindingDesc* InputActionAssetDesc::findBinding(const std::string& id) const{
    return const_cast<InputActionAssetDesc*>(this)->findBinding(id);
}

std::string InputActionAssetDesc::newBindingId() const{
    static std::mt19937 random(std::random_device{}());
    for (;;){
        char id[9];
        std::snprintf(id, sizeof(id), "%08x", (unsigned)random());
        if (!findBinding(id))
            return id;
    }
}

void InputActionAssetDesc::assignMissingIds(){
    std::set<std::string> seen;
    std::vector<InputBindingDesc*> missing;
    forEachBinding(*this, [&](InputBindingDesc& binding){
        if (binding.id.empty() || !seen.insert(binding.id).second)
            missing.push_back(&binding);
    });
    for (InputBindingDesc* binding : missing){
        binding->id.clear();
        binding->id = newBindingId();
    }
}

// ---- edit operations

namespace {

    using Addr = std::pair<InputActionMapDesc*, InputActionDesc*>;

    [[noreturn]] void fail(const std::string& message){
        throw std::invalid_argument(message);
    }

    std::string str(const InputJson& op, const char* key){
        if (!op.contains(key) || !op[key].is_string())
            fail(std::string("\"") + key + "\" (a string) is missing");
        return op[key].get<std::string>();
    }

    InputActionMapDesc& mapAt(InputActionAssetDesc& asset, const std::string& name){
        InputActionMapDesc* map = asset.findMap(name);
        if (!map)
            fail("no map \"" + name + "\"");
        return *map;
    }

    InputActionDesc& actionAt(InputActionAssetDesc& asset, const std::string& path){
        InputActionDesc* action = asset.findAction(path);
        if (!action)
            fail("no action \"" + path + "\" (Map/Action)");
        return *action;
    }

    // the list holding a binding (top level of an action) and its index; parts are not in lists
    struct BindingPlace {
        InputActionDesc* action = nullptr;
        size_t index = 0;
        InputBindingDesc* binding = nullptr;
        InputBindingDesc* composite = nullptr;   // the composite of a part
    };

    BindingPlace bindingAt(InputActionAssetDesc& asset, const std::string& id){
        BindingPlace place;
        for (InputActionMapDesc& map : asset.maps){
            for (InputActionDesc& action : map.actions){
                for (size_t i = 0; i < action.bindings.size(); i++){
                    InputBindingDesc& binding = action.bindings[i];
                    if (binding.id == id){
                        place.action = &action;
                        place.index = i;
                        place.binding = &binding;
                        return place;
                    }
                    for (auto& part : binding.parts){
                        if (part.second.id == id){
                            place.action = &action;
                            place.index = i;
                            place.binding = &part.second;
                            place.composite = &binding;
                            return place;
                        }
                    }
                }
            }
        }
        fail("no binding \"" + id + "\"");
    }

    size_t indexArg(const InputJson& op, size_t size, bool inserting){
        if (!op.contains("index"))
            return inserting ? size : size - 1;
        if (!op["index"].is_number_integer())
            fail("\"index\" must be an integer");
        const long long index = op["index"].get<long long>();
        const long long limit = inserting ? (long long)size : (long long)size - 1;
        if (index < 0 || index > limit)
            fail("\"index\" " + std::to_string(index) + " is out of 0.." + std::to_string(limit));
        return (size_t)index;
    }

    void checkName(const std::string& name, const char* what){
        if (name.empty() || name.find('/') != std::string::npos)
            fail(std::string("a ") + what + " name must not be empty or contain '/'");
    }

    float numberValue(const InputJson& value, const std::string& key){
        if (!value.is_number())
            fail("\"" + key + "\" must be a number");
        return value.get<float>();
    }

    bool boolValue(const InputJson& value, const std::string& key){
        if (!value.is_boolean())
            fail("\"" + key + "\" must be true or false");
        return value.get<bool>();
    }

    int modsValue(const InputJson& value, const std::string& key){
        if (!value.is_array())
            fail("\"" + key + "\" must be a list of shift, ctrl, alt, super");
        std::vector<std::string> names;
        for (const InputJson& item : value){
            if (!item.is_string() || inputModifiersFromNames({item.get<std::string>()}) == 0)
                fail("\"" + key + "\" has " + item.dump() + " (shift, ctrl, alt, super)");
            names.push_back(item.get<std::string>());
        }
        return inputModifiersFromNames(names);
    }

    void checkPath(const std::string& path){
        if (!InputControlPath::parse(path).isValid())
            fail("\"" + path + "\" is not a control path (keyboard/space, mouse/left, mouse/delta, gamepad/a, "
                 "gamepad/left_stick, touch/primary/press, virtual/<name>...)");
    }

    void setMapValue(InputActionMapDesc& map, const std::string& key, const InputJson& value){
        if (key == "priority"){
            if (!value.is_number_integer()) fail("\"priority\" must be an integer");
            map.priority = value.get<int>();
        }else if (key == "enabled") map.enabled = boolValue(value, key);
        else if (key == "consume") map.consume = boolValue(value, key);
        else if (key == "exclusive") map.exclusive = boolValue(value, key);
        else if (key == "ignore_when_ui_consumes") map.ignoreWhenUIConsumes = boolValue(value, key);
        else fail("a map has priority, enabled, consume, exclusive, ignore_when_ui_consumes (rename for name), not \"" + key + "\"");
    }

    void setActionValue(InputActionDesc& action, const std::string& key, const InputJson& value){
        if (key == "type"){
            if (!value.is_string() || !inputActionTypeFromName(value.get<std::string>(), action.type))
                fail("\"type\" must be BUTTON, AXIS or VECTOR2");
        }else if (key == "press_point") action.pressPoint = numberValue(value, key);
        else if (key == "hold_time") action.holdTime = std::max(0.0f, numberValue(value, key));
        else if (key == "deadzone"){
            if (value.is_null()) action.deadzone.reset();
            else action.deadzone = numberValue(value, key);
        }
        else fail("an action has type, press_point, hold_time, deadzone (rename for name), not \"" + key + "\"");
    }

    void setBindingValue(InputBindingDesc& binding, bool isPart, const std::string& key, const InputJson& value){
        if (key == "path"){
            if (binding.isComposite()) fail("a composite has no path: set its parts' paths");
            if (!value.is_string()) fail("\"path\" must be a string");
            checkPath(value.get<std::string>());
            binding.path = InputControlPath::parse(value.get<std::string>()).toString();
        }else if (key == "scale") binding.scale = numberValue(value, key);
        else if (key == "invert") binding.invert = boolValue(value, key);
        else if (key == "deadzone"){
            if (value.is_null()) binding.deadzone.reset();
            else binding.deadzone = numberValue(value, key);
        }
        else if (key == "required_mods") binding.requiredMods = modsValue(value, key);
        else if (key == "forbidden_mods") binding.forbiddenMods = modsValue(value, key);
        else if (key == "groups" && !isPart){
            if (!value.is_array()) fail("\"groups\" must be a list of strings");
            binding.groups.clear();
            for (const InputJson& group : value){
                if (!group.is_string()) fail("\"groups\" must be a list of strings");
                binding.groups.push_back(group.get<std::string>());
            }
        }else if (key == "normalize" && binding.isComposite()) binding.normalize = boolValue(value, key);
        else fail("a binding has path, scale, invert, deadzone, required_mods, forbidden_mods" +
                  std::string(isPart ? "" : ", groups") + std::string(binding.isComposite() ? ", normalize" : "") +
                  ", not \"" + key + "\"");
    }

    void applyValues(const InputJson& op, const std::function<void(const std::string&, const InputJson&)>& set){
        if (!op.contains("values") || !op["values"].is_object() || op["values"].empty())
            fail("\"values\" (an object of keys to set) is missing");
        for (auto it = op["values"].begin(); it != op["values"].end(); ++it)
            set(it.key(), it.value());
    }

    InputBindingDesc bindingFromOp(const InputJson& op, bool isPart){
        InputBindingDesc binding;
        const InputJson spec = op.is_string() ? InputJson{{"path", op}} : op;
        if (!spec.is_object())
            fail("a binding is a path or {path, ...}");
        binding.path = str(spec, "path");
        checkPath(binding.path);
        binding.path = InputControlPath::parse(binding.path).toString();
        for (auto it = spec.begin(); it != spec.end(); ++it){
            const std::string key = it.key();
            if (key == "path" || key == "op" || key == "action" || key == "index")
                continue;
            setBindingValue(binding, isPart, key, it.value());
        }
        return binding;
    }

    void freshIds(InputActionAssetDesc& asset, InputBindingDesc& binding){
        binding.id = asset.newBindingId();
        for (auto& part : binding.parts){
            // the new ids must also differ from each other
            part.second.id = "";
            part.second.id = asset.newBindingId();
            while (part.second.id == binding.id)
                part.second.id = asset.newBindingId();
        }
    }

    std::string where(const InputJson& op){
        if (op.contains("binding")) return "binding";
        if (op.contains("action")) return "action";
        if (op.contains("map")) return "map";
        fail("name a map, an action (Map/Action) or a binding (its id)");
    }

}

std::string jasperengine::applyInputActionsOp(InputActionAssetDesc& asset, const InputJson& op){
    if (!op.is_object())
        fail("an op is an object {op, ...}");
    const std::string kind = str(op, "op");
    // edits work on a copy: a failing op leaves the asset as it was
    InputActionAssetDesc edit = asset;
    std::string name;

    if (kind == "add_map"){
        InputActionMapDesc map;
        map.name = str(op, "name");
        checkName(map.name, "map");
        if (edit.findMap(map.name)) fail("a map \"" + map.name + "\" exists");
        for (auto it = op.begin(); it != op.end(); ++it){
            if (it.key() != "op" && it.key() != "name" && it.key() != "index")
                setMapValue(map, it.key(), it.value());
        }
        const size_t index = indexArg(op, edit.maps.size(), true);
        edit.maps.insert(edit.maps.begin() + (std::ptrdiff_t)index, map);
        name = "Add Action Map " + map.name;
    }else if (kind == "add_action"){
        InputActionMapDesc& map = mapAt(edit, str(op, "map"));
        InputActionDesc action;
        action.name = str(op, "name");
        checkName(action.name, "action");
        for (const InputActionDesc& other : map.actions){
            if (other.name == action.name) fail("\"" + map.name + "/" + action.name + "\" exists");
        }
        for (auto it = op.begin(); it != op.end(); ++it){
            if (it.key() != "op" && it.key() != "name" && it.key() != "index" && it.key() != "map")
                setActionValue(action, it.key(), it.value());
        }
        const size_t index = indexArg(op, map.actions.size(), true);
        map.actions.insert(map.actions.begin() + (std::ptrdiff_t)index, action);
        name = "Add Action " + map.name + "/" + action.name;
    }else if (kind == "add_binding"){
        const std::string path = str(op, "action");
        InputActionDesc& action = actionAt(edit, path);
        InputBindingDesc binding = bindingFromOp(op, false);
        freshIds(edit, binding);
        const size_t index = indexArg(op, action.bindings.size(), true);
        action.bindings.insert(action.bindings.begin() + (std::ptrdiff_t)index, binding);
        name = "Add Binding " + binding.path + " to " + path;
    }else if (kind == "add_composite"){
        const std::string path = str(op, "action");
        InputActionDesc& action = actionAt(edit, path);
        InputBindingDesc binding;
        if (!compositeFromName(str(op, "composite"), binding.composite))
            fail("\"composite\" must be axis1d or dpad2d");
        const InputJson parts = op.value("parts", InputJson::object());
        if (!parts.is_object())
            fail("\"parts\" must be an object {part: path | {path, ...}}");
        const std::vector<std::string> partNames = inputCompositeParts(binding.composite);
        for (auto it = parts.begin(); it != parts.end(); ++it){
            if (std::find(partNames.begin(), partNames.end(), it.key()) == partNames.end())
                fail(std::string(inputCompositeName(binding.composite)) + " has no part \"" + it.key() + "\"");
        }
        for (const std::string& part : partNames){
            InputBindingDesc desc;
            if (parts.contains(part))
                desc = bindingFromOp(parts[part], true);
            binding.parts.emplace_back(part, desc);
        }
        if (op.contains("groups")) setBindingValue(binding, false, "groups", op["groups"]);
        if (op.contains("normalize")) setBindingValue(binding, false, "normalize", op["normalize"]);
        freshIds(edit, binding);
        const size_t index = indexArg(op, action.bindings.size(), true);
        action.bindings.insert(action.bindings.begin() + (std::ptrdiff_t)index, binding);
        name = "Add " + std::string(binding.composite == InputCompositeType::DPAD_2D ? "2D Vector" : "1D Axis") +
               " Composite to " + path;
    }else if (kind == "set"){
        const std::string target = where(op);
        if (target == "map"){
            InputActionMapDesc& map = mapAt(edit, str(op, "map"));
            applyValues(op, [&](const std::string& key, const InputJson& value){ setMapValue(map, key, value); });
            name = "Edit Action Map " + map.name;
        }else if (target == "action"){
            const std::string path = str(op, "action");
            InputActionDesc& action = actionAt(edit, path);
            applyValues(op, [&](const std::string& key, const InputJson& value){ setActionValue(action, key, value); });
            name = "Edit Action " + path;
        }else{
            BindingPlace place = bindingAt(edit, str(op, "binding"));
            applyValues(op, [&](const std::string& key, const InputJson& value){
                setBindingValue(*place.binding, place.composite != nullptr, key, value);
            });
            name = "Edit Binding " + (place.binding->isComposite() ? std::string(inputCompositeName(place.binding->composite)) : place.binding->path);
        }
    }else if (kind == "remove"){
        const std::string target = where(op);
        if (target == "map"){
            const std::string mapName = str(op, "map");
            mapAt(edit, mapName);
            edit.maps.erase(std::remove_if(edit.maps.begin(), edit.maps.end(), [&](const InputActionMapDesc& m){ return m.name == mapName; }), edit.maps.end());
            name = "Delete Action Map " + mapName;
        }else if (target == "action"){
            const std::string path = str(op, "action");
            actionAt(edit, path);
            InputActionMapDesc& map = mapAt(edit, path.substr(0, path.find('/')));
            const std::string actionName = path.substr(path.find('/') + 1);
            map.actions.erase(std::remove_if(map.actions.begin(), map.actions.end(), [&](const InputActionDesc& a){ return a.name == actionName; }), map.actions.end());
            name = "Delete Action " + path;
        }else{
            BindingPlace place = bindingAt(edit, str(op, "binding"));
            if (place.composite)
                fail("a composite's part cannot be removed: set its path, or remove the composite");
            name = "Delete Binding " + (place.binding->isComposite() ? std::string(inputCompositeName(place.binding->composite)) : place.binding->path);
            place.action->bindings.erase(place.action->bindings.begin() + (std::ptrdiff_t)place.index);
        }
    }else if (kind == "rename"){
        const std::string newName = str(op, "name");
        if (op.contains("action")){
            const std::string path = str(op, "action");
            checkName(newName, "action");
            InputActionDesc& action = actionAt(edit, path);
            InputActionMapDesc& map = mapAt(edit, path.substr(0, path.find('/')));
            for (const InputActionDesc& other : map.actions){
                if (other.name == newName && &other != &action) fail("\"" + map.name + "/" + newName + "\" exists");
            }
            action.name = newName;
            name = "Rename Action " + path + " to " + newName;
        }else if (op.contains("map")){
            checkName(newName, "map");
            InputActionMapDesc& map = mapAt(edit, str(op, "map"));
            if (InputActionMapDesc* other = edit.findMap(newName); other && other != &map) fail("a map \"" + newName + "\" exists");
            name = "Rename Action Map " + map.name + " to " + newName;
            map.name = newName;
        }else{
            fail("rename takes a map or an action");
        }
    }else if (kind == "move"){
        const std::string target = where(op);
        if (target == "map"){
            const std::string mapName = str(op, "map");
            mapAt(edit, mapName);
            auto it = std::find_if(edit.maps.begin(), edit.maps.end(), [&](const InputActionMapDesc& m){ return m.name == mapName; });
            InputActionMapDesc moved = *it;
            edit.maps.erase(it);
            const size_t index = indexArg(op, edit.maps.size() + 1, false);
            edit.maps.insert(edit.maps.begin() + (std::ptrdiff_t)index, moved);
            name = "Move Action Map " + mapName;
        }else if (target == "action"){
            const std::string path = str(op, "action");
            actionAt(edit, path);
            InputActionMapDesc& map = mapAt(edit, path.substr(0, path.find('/')));
            const std::string actionName = path.substr(path.find('/') + 1);
            auto it = std::find_if(map.actions.begin(), map.actions.end(), [&](const InputActionDesc& a){ return a.name == actionName; });
            InputActionDesc moved = *it;
            map.actions.erase(it);
            const size_t index = indexArg(op, map.actions.size() + 1, false);
            map.actions.insert(map.actions.begin() + (std::ptrdiff_t)index, moved);
            name = "Move Action " + path;
        }else{
            BindingPlace place = bindingAt(edit, str(op, "binding"));
            if (place.composite)
                fail("a composite's parts keep their order");
            InputBindingDesc moved = *place.binding;
            place.action->bindings.erase(place.action->bindings.begin() + (std::ptrdiff_t)place.index);
            const size_t index = indexArg(op, place.action->bindings.size() + 1, false);
            place.action->bindings.insert(place.action->bindings.begin() + (std::ptrdiff_t)index, moved);
            name = "Move Binding " + (moved.isComposite() ? std::string(inputCompositeName(moved.composite)) : moved.path);
        }
    }else if (kind == "duplicate"){
        const std::string target = where(op);
        if (target == "map"){
            const std::string mapName = str(op, "map");
            InputActionMapDesc copy = mapAt(edit, mapName);
            std::string copyName = mapName + " Copy";
            for (int n = 2; edit.findMap(copyName); n++)
                copyName = mapName + " Copy " + std::to_string(n);
            copy.name = copyName;
            for (InputActionDesc& action : copy.actions){
                for (InputBindingDesc& binding : action.bindings){
                    binding.id.clear();
                    for (auto& part : binding.parts) part.second.id.clear();
                }
            }
            auto it = std::find_if(edit.maps.begin(), edit.maps.end(), [&](const InputActionMapDesc& m){ return m.name == mapName; });
            edit.maps.insert(it + 1, copy);
            edit.assignMissingIds();
            name = "Duplicate Action Map " + mapName;
        }else if (target == "action"){
            const std::string path = str(op, "action");
            InputActionDesc copy = actionAt(edit, path);
            InputActionMapDesc& map = mapAt(edit, path.substr(0, path.find('/')));
            std::string copyName = copy.name + " Copy";
            auto taken = [&](const std::string& candidate){
                return std::any_of(map.actions.begin(), map.actions.end(), [&](const InputActionDesc& a){ return a.name == candidate; });
            };
            for (int n = 2; taken(copyName); n++)
                copyName = copy.name + " Copy " + std::to_string(n);
            const std::string originalName = copy.name;
            copy.name = copyName;
            for (InputBindingDesc& binding : copy.bindings){
                binding.id.clear();
                for (auto& part : binding.parts) part.second.id.clear();
            }
            auto it = std::find_if(map.actions.begin(), map.actions.end(), [&](const InputActionDesc& a){ return a.name == originalName; });
            map.actions.insert(it + 1, copy);
            edit.assignMissingIds();
            name = "Duplicate Action " + path;
        }else{
            BindingPlace place = bindingAt(edit, str(op, "binding"));
            if (place.composite)
                fail("duplicate the composite, not its part");
            InputBindingDesc copy = *place.binding;
            freshIds(edit, copy);
            place.action->bindings.insert(place.action->bindings.begin() + (std::ptrdiff_t)place.index + 1, copy);
            name = "Duplicate Binding " + (copy.isComposite() ? std::string(inputCompositeName(copy.composite)) : copy.path);
        }
    }else{
        fail("op must be add_map, add_action, add_binding, add_composite, set, remove, rename, move or duplicate, not \"" + kind + "\"");
    }

    asset = std::move(edit);
    return name;
}
