// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDataModel.h"
#include "HtmlUIScriptInternal.h"
#include "HtmlUIValueDefinition.h"

#include "Log.h"
#include "script/LuaBinding.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine;

int HtmlUIDataModelRecord::dispatchingEvents = 0;

namespace {

    struct PathToken {
        bool isIndex = false;
        size_t index = 0;
        std::string name;
    };

    // "items[2].name" -> items, [2], name; false when malformed
    bool parsePath(const std::string& path, std::vector<PathToken>& tokens){
        size_t i = 0;
        const size_t n = path.size();
        while (i < n){
            if (path[i] == '['){
                const size_t close = path.find(']', i);
                if (close == std::string::npos || close == i + 1 || tokens.empty())
                    return false;
                const std::string digits = path.substr(i + 1, close - i - 1);
                if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c){ return std::isdigit(c); }))
                    return false;
                PathToken token;
                token.isIndex = true;
                token.index = (size_t)std::strtoull(digits.c_str(), nullptr, 10);
                tokens.push_back(token);
                i = close + 1;
            }else{
                if (path[i] == '.'){
                    if (tokens.empty())
                        return false;
                    i++;
                }
                size_t end = i;
                while (end < n && path[end] != '.' && path[end] != '[')
                    end++;
                if (end == i)
                    return false;
                PathToken token;
                token.name = path.substr(i, end - i);
                tokens.push_back(token);
                i = end;
            }
        }
        return !tokens.empty() && !tokens[0].isIndex;
    }

    UIValue* child(UIValue* value, const PathToken& token){
        if (!value)
            return nullptr;
        if (token.isIndex){
            if (value->getType() != UIValue::Type::ARRAY || token.index >= value->size())
                return nullptr;
            return &HtmlUIScriptAccess::items(*value)[token.index];
        }
        if (value->getType() != UIValue::Type::OBJECT)
            return nullptr;
        return const_cast<UIValue*>(value->find(token.name));
    }

    // the DOM name of an RmlUi event (data-event-* callbacks see browser names)
    std::string domName(const std::string& rmlType){
        if (rmlType == "mousescroll") return "wheel";
        if (rmlType == "change") return "input";
        if (rmlType == "htmlui-change") return "change";
        // S17: HTML5's drag events (data-event-drop and the like)
        if (rmlType == "htmlui-dragenter" || rmlType == "htmlui-dragover" || rmlType == "htmlui-dragleave" || rmlType == "htmlui-drop")
            return rmlType.substr(7);
        return rmlType;
    }

    HtmlUIDataModelRecord* liveRecord(const std::shared_ptr<UIDataModelState>& state, const char* method){
        if (!state)
            return nullptr;
        if (!state->record && !state->warned){
            state->warned = true;
            Log::warn("[HTML UI] UIDataModel::%s: data model '%s' was removed", method, state->name.c_str());
        }
        return state->record;
    }

}

// ---- HtmlUIDataModelRecord ----

HtmlUIDataModelRecord::~HtmlUIDataModelRecord(){
    detach();
    if (state){
        state->record = nullptr;
    }
}

UIValue* HtmlUIDataModelRecord::variable(const std::string& variableName){
    auto it = storage.find(variableName);
    return (it != storage.end()) ? it->second.get() : nullptr;
}

UIValue* HtmlUIDataModelRecord::resolve(const std::string& path, std::string* topLevel){
    std::vector<PathToken> tokens;
    if (!parsePath(path, tokens))
        return nullptr;
    if (topLevel)
        *topLevel = tokens[0].name;
    UIValue* value = variable(tokens[0].name);
    for (size_t i = 1; i < tokens.size() && value; i++){
        value = child(value, tokens[i]);
    }
    return value;
}

void HtmlUIDataModelRecord::dirty(const std::string& topLevel){
    if (handle)
        handle.DirtyVariable(topLevel);
}

void HtmlUIDataModelRecord::markWritten(const std::string& topLevel){
    if (std::find(pendingChanges.begin(), pendingChanges.end(), topLevel) == pendingChanges.end())
        pendingChanges.push_back(topLevel);
}

bool HtmlUIDataModelRecord::attach(Rml::Context* target){
    if (context == target)
        return true;
    detach();
    if (!target)
        return false;

    Rml::DataModelConstructor constructor = target->CreateDataModel(name);
    if (!constructor){
        Log::error("[HTML UI] could not create data model '%s'", name.c_str());
        return false;
    }

    for (const std::string& key : order){
        auto definition = std::make_unique<HtmlUIValueDefinition>(this, key);
        constructor.BindCustomDataVariable(key, Rml::DataVariable(definition.get(), storage[key].get()));
        definitions[key] = std::move(definition);
    }

    context = target;
    handle = constructor.GetModelHandle();
    boundEvents.clear();
    for (const auto& pair : events){
        bindEventName(pair.first);
    }
    return true;
}

void HtmlUIDataModelRecord::bindEventName(const std::string& eventName){
    if (!context || std::find(boundEvents.begin(), boundEvents.end(), eventName) != boundEvents.end())
        return;

    Rml::DataModelConstructor constructor = context->GetDataModel(name);
    if (!constructor)
        return;

    // the callback is looked up when the event fires, so a later bindEvent replaces it
    constructor.BindEventCallback(eventName, [this, eventName](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& arguments){
        auto it = events.find(eventName);
        if (it == events.end())
            return;

        std::vector<UIValue> values;
        values.reserve(arguments.size());
        for (const Rml::Variant& argument : arguments){
            values.push_back(HtmlUIValueDefinition::fromVariant(argument));
        }

        std::shared_ptr<HtmlUIEventState> eventState = std::make_shared<HtmlUIEventState>();
        eventState->event = &event;
        eventState->type = domName(event.GetType());
        UIEvent uiEvent = HtmlUIScriptAccess::makeEvent(eventState);

        dispatchingEvents++;
        if (it->second.fn){
            it->second.fn(uiEvent, values);
        }else if (it->second.lua.isSet()){
            lua_State* L = LuaBinding::getLuaState();
            if (L && it->second.lua.push(L)){
                HtmlUILua::pushEvent(L, uiEvent);
                for (const UIValue& value : values){
                    HtmlUILua::pushValue(L, value);
                }
                HtmlUILua::call(L, 1 + (int)values.size(), "data model '" + name + "' event '" + eventName + "'");
            }
        }
        dispatchingEvents--;

        eventState->event = nullptr;
    });
    boundEvents.push_back(eventName);
}

void HtmlUIDataModelRecord::detach(){
    if (context){
        context->RemoveDataModel(name);
    }
    context = nullptr;
    handle = Rml::DataModelHandle();
    definitions.clear();
    boundEvents.clear();
}

// ---- UIDataModel ----

UIDataModel HtmlUIScriptAccess::makeModel(const std::shared_ptr<UIDataModelState>& state){
    UIDataModel model;
    model.state = state;
    return model;
}

std::shared_ptr<UIDataModelState> HtmlUIScriptAccess::modelState(const UIDataModel& model){
    return model.state;
}

bool UIDataModel::isValid() const{
    return state && state->record;
}

std::string UIDataModel::getName() const{
    return state ? state->name : std::string();
}

UIValue UIDataModel::get(const std::string& path) const{
    HtmlUIDataModelRecord* record = liveRecord(state, "get");
    if (!record)
        return UIValue();
    const UIValue* value = record->resolve(path);
    return value ? *value : UIValue();
}

void UIDataModel::set(const std::string& path, const UIValue& value){
    HtmlUIDataModelRecord* record = liveRecord(state, "set");
    if (!record)
        return;

    std::vector<PathToken> tokens;
    if (!parsePath(path, tokens)){
        Log::error("[HTML UI] UIDataModel::set: invalid path '%s' (model '%s')", path.c_str(), record->name.c_str());
        return;
    }

    UIValue* top = record->variable(tokens[0].name);
    if (!top){
        Log::error("[HTML UI] UIDataModel::set: '%s' is not a variable of data model '%s' (top-level keys are fixed when it is created)",
                   tokens[0].name.c_str(), record->name.c_str());
        return;
    }

    if (tokens.size() == 1){
        *top = value;
    }else{
        UIValue* parent = top;
        for (size_t i = 1; i + 1 < tokens.size() && parent; i++){
            parent = child(parent, tokens[i]);
        }
        const PathToken& last = tokens.back();
        bool done = false;
        if (parent && last.isIndex){
            if (parent->getType() == UIValue::Type::ARRAY && last.index < parent->size()){
                HtmlUIScriptAccess::items(*parent)[last.index] = value;
                done = true;
            }else if ((parent->getType() == UIValue::Type::ARRAY || parent->isNil()) && last.index == parent->size()){
                parent->push(value);
                done = true;
            }
        }else if (parent && (parent->getType() == UIValue::Type::OBJECT || parent->isNil() ||
                             (parent->getType() == UIValue::Type::ARRAY && parent->size() == 0))){
            (*parent)[last.name] = value;
            done = true;
        }
        if (!done){
            Log::error("[HTML UI] UIDataModel::set: no value at '%s' in data model '%s'", path.c_str(), record->name.c_str());
            return;
        }
    }

    record->dirty(tokens[0].name);
}

void UIDataModel::dirty(const std::string& topLevelName){
    if (HtmlUIDataModelRecord* record = liveRecord(state, "dirty"))
        record->dirty(topLevelName);
}

void UIDataModel::dirtyAll(){
    HtmlUIDataModelRecord* record = liveRecord(state, "dirtyAll");
    if (record && record->handle)
        record->handle.DirtyAllVariables();
}

void UIDataModel::bindEvent(const std::string& name, std::function<void(UIEvent&, const std::vector<UIValue>& args)> fn){
    HtmlUIDataModelRecord* record = liveRecord(state, "bindEvent");
    if (!record)
        return;
    HtmlUIDataModelRecord::EventCallback& callback = record->events[name];
    callback.fn = std::move(fn);
    callback.lua.reset();
    record->bindEventName(name);
}

uint64_t UIDataModel::onChange(const std::string& topLevelName, std::function<void(const UIValue&)> fn){
    HtmlUIDataModelRecord* record = liveRecord(state, "onChange");
    if (!record || !fn)
        return 0;
    if (!record->variable(topLevelName)){
        Log::warn("[HTML UI] UIDataModel::onChange: '%s' is not a variable of data model '%s'", topLevelName.c_str(), record->name.c_str());
        return 0;
    }
    HtmlUIDataModelRecord::ChangeCallback callback;
    callback.id = HtmlUIScriptState::nextChangeId++;
    callback.variable = topLevelName;
    callback.fn = std::move(fn);
    record->changeCallbacks.push_back(std::move(callback));
    return record->changeCallbacks.back().id;
}

void UIDataModel::removeOnChange(uint64_t id){
    HtmlUIDataModelRecord* record = liveRecord(state, "removeOnChange");
    if (!record)
        return;
    auto& callbacks = record->changeCallbacks;
    callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(), [id](const HtmlUIDataModelRecord::ChangeCallback& c){ return c.id == id; }),
                    callbacks.end());
}
