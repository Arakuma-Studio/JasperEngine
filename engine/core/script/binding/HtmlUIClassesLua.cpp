// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua API of HTML/CSS documents (phase 1 §3.9.5): UIDocument, UIElement (+ classList, style,
// dataset), UIEvent, UIRect, and the data-model proxies of createDataModel.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames. Every error raised here is raised
// once no C++ object with a destructor is alive in the raising function (the C++ work runs in
// helpers that return an error text in a plain buffer).

#include "LuaBinding.h"
#include "HtmlUIClassesLua.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"
#include "LuaComponentSetters.h"

#include "Log.h"
#include "Scene.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElementInternal.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"
#include "htmlui/compat/DocumentLoader.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

// the HTML UI module (docs/specs/S34-player-size.md R1): the Lua API description reads this condition
#if JASPERENGINE_HTMLUI

using namespace jasperengine;

namespace {

    const char* const kModelMetatable = "jasperengine.UIDataModel";

    // methods of a data model proxy (its top level); data keys with these names are refused
    const char* const kModelMethods[] = {"onChange", "removeOnChange", "dirty", "dirtyAll", "getName", "isValid", "get", "set"};

    size_t liveRefs = 0;

    typedef char ErrorText[512];

    void setError(ErrorText error, const std::string& text){
        std::snprintf(error, sizeof(ErrorText), "%s", text.c_str());
    }

    std::optional<UIElement> optionalElement(const UIElement& element){
        return element.isValid() ? std::optional<UIElement>(element) : std::nullopt;
    }

    // camelCase -> kebab-case for style keys (custom properties and hyphenated names unchanged); CSSOM's webkit-cased
    // names are the -webkit- properties (style.webkitLineClamp is -webkit-line-clamp, as style.WebkitLineClamp)
    std::string cssName(const std::string& key){
        if (key.rfind("--", 0) == 0 || key.find('-') != std::string::npos)
            return key;
        std::string out;
        if (key.size() > 6 && key.compare(0, 6, "webkit") == 0 && std::isupper((unsigned char)key[6]))
            out = "-";
        for (char c : key){
            if (std::isupper((unsigned char)c)){
                out += '-';
                out += (char)std::tolower((unsigned char)c);
            }else{
                out += c;
            }
        }
        return out;
    }

    bool isStyleMethod(const std::string& name){
        return name == "getPropertyValue" || name == "setProperty" || name == "removeProperty";
    }

    std::string describeElement(const UIElement& element){
        if (!element.isValid())
            return "UIElement(invalid)";
        std::string text = "UIElement(" + element.getTagName();
        const std::string id = element.getId();
        if (!id.empty())
            text += "#" + id;
        return text + ")";
    }

    // ---- dead handles: every member of a dead element raises, isValid excepted ----

    bool isLiveElementArg(lua_State* L, int index){
        if (luabridge::detail::Userdata::isInstance<UIElement>(L, index)){
            const UIElement* element = luabridge::detail::Userdata::get<UIElement>(L, index, true);
            return element && element->isValid();
        }
        const UIElement* owner = nullptr;
        if (luabridge::detail::Userdata::isInstance<UIClassList>(L, index)){
            owner = &luabridge::detail::Userdata::get<UIClassList>(L, index, true)->element;
        }else if (luabridge::detail::Userdata::isInstance<UIStyle>(L, index)){
            owner = &luabridge::detail::Userdata::get<UIStyle>(L, index, true)->element;
        }else if (luabridge::detail::Userdata::isInstance<UIDataset>(L, index)){
            owner = &luabridge::detail::Userdata::get<UIDataset>(L, index, true)->element;
        }
        return !owner || owner->isValid();
    }

    // __index/__newindex in front of LuaBridge's (upvalue 1)
    int guardedIndex(lua_State* L){
        const bool allowed = lua_type(L, 2) == LUA_TSTRING && std::strcmp(lua_tostring(L, 2), "isValid") == 0;
        if (!allowed && !isLiveElementArg(L, 1))
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, 1);
        return 1;
    }

    int guardedNewIndex(lua_State* L){
        if (!isLiveElementArg(L, 1))
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_insert(L, 1);
        lua_call(L, lua_gettop(L) - 1, 0);
        return 0;
    }

    void guardMetatable(lua_State* L, const void* registryKey){
        lua_rawgetp(L, LUA_REGISTRYINDEX, registryKey);
        if (lua_istable(L, -1)){
            lua_getfield(L, -1, "__index");
            lua_pushcclosure(L, guardedIndex, 1);
            lua_setfield(L, -2, "__index");
            lua_getfield(L, -1, "__newindex");
            lua_pushcclosure(L, guardedNewIndex, 1);
            lua_setfield(L, -2, "__newindex");
        }
        lua_pop(L, 1);
    }

    template<typename T>
    void guardClass(lua_State* L){
        guardMetatable(L, luabridge::detail::getClassRegistryKey<T>());
        guardMetatable(L, luabridge::detail::getConstRegistryKey<T>());
    }

    // ---- listeners ----

    struct ListenerOptions {
        bool capture = false;
        bool once = false;
    };

    // reads argument 4 (boolean or {capture=, once=}); false when it is neither
    bool readListenerOptions(lua_State* L, int index, ListenerOptions& options){
        if (lua_isnoneornil(L, index))
            return true;
        if (lua_isboolean(L, index)){
            options.capture = lua_toboolean(L, index);
            return true;
        }
        if (lua_istable(L, index)){
            lua_getfield(L, index, "capture");
            options.capture = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, index, "once");
            options.once = lua_toboolean(L, -1);
            lua_pop(L, 1);
            return true;
        }
        return false;
    }

    HtmlUIEventListener* findLuaListener(lua_State* L, Rml::Element* element, const std::string& type, bool capture, int fnIndex){
        for (HtmlUIEventListener* listener : HtmlUIEventListener::listenersOf(element)){
            if (listener->getDomType() != type || listener->isCapture() != capture || !listener->getCallback().lua.isSet())
                continue;
            if (!listener->getCallback().lua.push(L))
                continue;
            const bool same = lua_rawequal(L, -1, fnIndex);
            lua_pop(L, 1);
            if (same)
                return listener;
        }
        return nullptr;
    }

    void addLuaListener(lua_State* L, Rml::Element* element, const std::string& type, const ListenerOptions& options){
        std::string rmlType;
        if (!HtmlUIEventNames::toRml(type, rmlType, "addEventListener"))
            return;
        // the same function, type and phase again is ignored, as in the DOM
        if (findLuaListener(L, element, type, options.capture, 3))
            return;
        HtmlUIEventListener::Callback callback;
        callback.lua = HtmlUILuaRef(L, 3);
        HtmlUIEventListener::attach(element, type, rmlType, options.capture, options.once, std::move(callback));
    }

    void removeLuaListener(lua_State* L, Rml::Element* element, const std::string& type, const ListenerOptions& options){
        if (HtmlUIEventListener* listener = findLuaListener(L, element, type, options.capture, 3))
            listener->remove();
    }

    // (self, type, listener [, captureOrOptions]) on the element of self
    int listenerCall(lua_State* L, Rml::Element* element, bool add){
        luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        ListenerOptions options;
        if (!readListenerOptions(L, 4, options))
            return luaL_argerror(L, 4, "boolean or {capture = boolean, once = boolean} expected");
        if (!element)
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        if (add){
            addLuaListener(L, element, lua_tostring(L, 2), options);
        }else{
            removeLuaListener(L, element, lua_tostring(L, 2), options);
        }
        return 0;
    }

    int elementAddEventListener(lua_State* L){
        UIElement* self = luabridge::detail::Userdata::get<UIElement>(L, 1, false);
        return listenerCall(L, self ? UIElementAccess::get(*self) : nullptr, true);
    }

    int elementRemoveEventListener(lua_State* L){
        UIElement* self = luabridge::detail::Userdata::get<UIElement>(L, 1, false);
        return listenerCall(L, self ? UIElementAccess::get(*self) : nullptr, false);
    }

    Rml::ElementDocument* luaDocumentOf(UIDocument* document){
        return document ? document->getScene()->getSystem<UIDocumentSystem>()->getDocument(document->getEntity()) : nullptr;
    }

    // a script's init can run before the system's first update: the document loads first
    Rml::ElementDocument* loadedDocumentOf(UIDocument* document){
        if (!document)
            return nullptr;
        document->getBody();
        return luaDocumentOf(document);
    }

    int documentAddEventListener(lua_State* L){
        UIDocument* self = luabridge::detail::Userdata::get<UIDocument>(L, 1, false);
        Rml::Element* element = loadedDocumentOf(self);
        if (!element)
            return luaL_error(L, "UIDocument.addEventListener: the document is not loaded");
        return listenerCall(L, element, true);
    }

    int documentRemoveEventListener(lua_State* L){
        UIDocument* self = luabridge::detail::Userdata::get<UIDocument>(L, 1, false);
        Rml::Element* element = luaDocumentOf(self);
        if (!element)
            return 0;
        return listenerCall(L, element, false);
    }

    // (self, type [, detail [, bubbles]]) -> false when a listener prevented it
    bool dispatchFromLua(lua_State* L, UIElement* self, ErrorText error, bool& result){
        UIValue detail;
        std::string conversionError;
        if (!lua_isnoneornil(L, 3) && !HtmlUILua::toValue(L, 3, detail, conversionError)){
            setError(error, "dispatchEvent: detail: " + conversionError);
            return false;
        }
        const bool bubbles = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4);
        result = self->dispatchEvent(lua_tostring(L, 2), detail, bubbles);
        return true;
    }

    int elementDispatchEvent(lua_State* L){
        UIElement* self = luabridge::detail::Userdata::get<UIElement>(L, 1, false);
        luaL_checkstring(L, 2);
        if (!self || !self->isValid())
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        ErrorText error;
        bool result = true;
        if (!dispatchFromLua(L, self, error, result))
            return luaL_error(L, "%s", error);
        lua_pushboolean(L, result);
        return 1;
    }

    // ---- classList varargs ----

    int classListChange(lua_State* L, bool add){
        UIClassList* self = luabridge::detail::Userdata::get<UIClassList>(L, 1, false);
        const int top = lua_gettop(L);
        for (int i = 2; i <= top; i++)
            luaL_checkstring(L, i);
        if (!self || !self->element.isValid())
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        for (int i = 2; i <= top; i++){
            Rml::Element* element = UIElementAccess::get(self->element);
            element->SetClass(lua_tostring(L, i), add);
        }
        return 0;
    }

    int classListAdd(lua_State* L){
        return classListChange(L, true);
    }

    int classListRemove(lua_State* L){
        return classListChange(L, false);
    }

    // ---- data model proxies ----

    struct PathKey {
        bool isIndex = false;
        size_t index = 0;          // 0-based
        std::string name;
    };

    struct ModelProxy {
        std::shared_ptr<UIDataModelState> state;
        std::vector<PathKey> path;   // empty: the model's top level
    };

    std::string describePath(const ModelProxy& proxy){
        std::string text = "data model '" + (proxy.state ? proxy.state->name : std::string()) + "'";
        if (proxy.path.empty())
            return text;
        std::string path;
        for (const PathKey& key : proxy.path){
            if (key.isIndex){
                path += "[" + std::to_string(key.index + 1) + "]";
            }else{
                path += (path.empty() ? "" : ".") + key.name;
            }
        }
        return text + " '" + path + "'";
    }

    // the value a proxy stands for; nullptr when the path no longer leads to one
    UIValue* proxyValue(const ModelProxy& proxy){
        HtmlUIDataModelRecord* record = proxy.state ? proxy.state->record : nullptr;
        if (!record || proxy.path.empty())
            return nullptr;
        UIValue* value = record->variable(proxy.path[0].name);
        for (size_t i = 1; i < proxy.path.size() && value; i++){
            const PathKey& key = proxy.path[i];
            if (key.isIndex){
                if (value->getType() != UIValue::Type::ARRAY || key.index >= value->size())
                    return nullptr;
                value = &HtmlUIScriptAccess::items(*value)[key.index];
            }else{
                value = const_cast<UIValue*>(value->find(key.name));
            }
        }
        return value;
    }

    ModelProxy* checkProxy(lua_State* L, int index){
        return static_cast<ModelProxy*>(luaL_checkudata(L, index, kModelMetatable));
    }

    void pushProxy(lua_State* L, const std::shared_ptr<UIDataModelState>& state, std::vector<PathKey> path){
        void* memory = lua_newuserdatauv(L, sizeof(ModelProxy), 0);
        new (memory) ModelProxy{state, std::move(path)};
        luaL_setmetatable(L, kModelMetatable);
    }

    // a child of value: a nested proxy for tables, the value itself otherwise
    void pushChild(lua_State* L, const ModelProxy& proxy, const UIValue* child, const PathKey& key){
        if (!child){
            lua_pushnil(L);
        }else if (child->getType() == UIValue::Type::ARRAY || child->getType() == UIValue::Type::OBJECT){
            std::vector<PathKey> path = proxy.path;
            path.push_back(key);
            pushProxy(L, proxy.state, std::move(path));
        }else{
            HtmlUILua::pushValue(L, *child);
        }
    }

    bool isModelMethod(const char* name){
        for (const char* method : kModelMethods){
            if (std::strcmp(method, name) == 0)
                return true;
        }
        return false;
    }

    // ---- proxy methods (self is the proxy) ----

    int modelIsValid(lua_State* L){
        ModelProxy* proxy = checkProxy(L, 1);
        lua_pushboolean(L, proxy->state && proxy->state->record);
        return 1;
    }

    int modelGetName(lua_State* L){
        ModelProxy* proxy = checkProxy(L, 1);
        lua_pushstring(L, proxy->state ? proxy->state->name.c_str() : "");
        return 1;
    }

    HtmlUIDataModelRecord* liveRecordOf(lua_State* L, int index, ErrorText error){
        ModelProxy* proxy = checkProxy(L, index);
        HtmlUIDataModelRecord* record = proxy->state ? proxy->state->record : nullptr;
        if (!record)
            std::snprintf(error, sizeof(ErrorText), "data model '%s' was removed", proxy->state ? proxy->state->name.c_str() : "");
        return record;
    }

    bool modelOnChangeImpl(lua_State* L, HtmlUIDataModelRecord* record, ErrorText error, uint64_t& id){
        const std::string variable = lua_tostring(L, 2);
        if (!record->variable(variable)){
            setError(error, "data model '" + record->name + "' has no variable '" + variable + "'");
            return false;
        }
        HtmlUIDataModelRecord::ChangeCallback callback;
        callback.id = HtmlUIScriptState::nextChangeId++;
        callback.variable = variable;
        callback.lua = HtmlUILuaRef(L, 3);
        id = callback.id;
        record->changeCallbacks.push_back(std::move(callback));
        return true;
    }

    int modelOnChange(lua_State* L){
        luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        uint64_t id = 0;
        if (!modelOnChangeImpl(L, record, error, id))
            return luaL_error(L, "%s", error);
        lua_pushinteger(L, (lua_Integer)id);
        return 1;
    }

    int modelRemoveOnChange(lua_State* L){
        const lua_Integer id = luaL_checkinteger(L, 2);
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        UIDataModel model = HtmlUIScriptState::makeHandle(record);
        model.removeOnChange((uint64_t)id);
        return 0;
    }

    int modelDirty(lua_State* L){
        luaL_checkstring(L, 2);
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        record->dirty(lua_tostring(L, 2));
        return 0;
    }

    int modelDirtyAll(lua_State* L){
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        if (record->handle)
            record->handle.DirtyAllVariables();
        return 0;
    }

    void modelGetImpl(lua_State* L, HtmlUIDataModelRecord* record){
        const UIValue* value = record->resolve(lua_tostring(L, 2));
        if (value){
            HtmlUILua::pushValue(L, *value);
        }else{
            lua_pushnil(L);
        }
    }

    int modelGet(lua_State* L){
        luaL_checkstring(L, 2);
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        modelGetImpl(L, record);
        return 1;
    }

    bool modelSetImpl(lua_State* L, HtmlUIDataModelRecord* record, ErrorText error){
        UIValue value;
        std::string conversionError;
        if (!HtmlUILua::toValue(L, 3, value, conversionError)){
            setError(error, "data model '" + record->name + "': " + conversionError);
            return false;
        }
        std::string topLevel;
        const std::string path = lua_tostring(L, 2);
        if (!record->resolve(path, &topLevel) && !record->variable(topLevel)){
            setError(error, "data model '" + record->name + "' has no variable '" + topLevel + "'");
            return false;
        }
        UIDataModel model = HtmlUIScriptState::makeHandle(record);
        model.set(path, value);
        return true;
    }

    int modelSet(lua_State* L){
        luaL_checkstring(L, 2);
        ErrorText error;
        HtmlUIDataModelRecord* record = liveRecordOf(L, 1, error);
        if (!record)
            return luaL_error(L, "%s", error);
        if (!modelSetImpl(L, record, error))
            return luaL_error(L, "%s", error);
        return 0;
    }

    // ---- proxy metamethods ----

    // reads proxy[key]; false with error text when it cannot
    bool proxyIndexImpl(lua_State* L, ErrorText error){
        const ModelProxy& proxy = *checkProxy(L, 1);
        HtmlUIDataModelRecord* record = proxy.state ? proxy.state->record : nullptr;
        if (!record){
            setError(error, "data model '" + (proxy.state ? proxy.state->name : std::string()) + "' was removed");
            return false;
        }

        if (proxy.path.empty()){
            if (lua_type(L, 2) != LUA_TSTRING){
                lua_pushnil(L);
                return true;
            }
            PathKey key;
            key.name = lua_tostring(L, 2);
            pushChild(L, proxy, record->variable(key.name), key);
            return true;
        }

        const UIValue* value = proxyValue(proxy);
        if (value && value->getType() == UIValue::Type::ARRAY && lua_isinteger(L, 2)){
            const lua_Integer index = lua_tointeger(L, 2);
            if (index >= 1 && (size_t)index <= value->size()){
                PathKey key;
                key.isIndex = true;
                key.index = (size_t)(index - 1);
                pushChild(L, proxy, &HtmlUIScriptAccess::items(*value)[key.index], key);
                return true;
            }
        }else if (value && value->getType() == UIValue::Type::OBJECT && lua_type(L, 2) == LUA_TSTRING){
            PathKey key;
            key.name = lua_tostring(L, 2);
            pushChild(L, proxy, value->find(key.name), key);
            return true;
        }
        lua_pushnil(L);
        return true;
    }

    int proxyIndex(lua_State* L){
        // the top level's methods first
        ModelProxy* proxy = checkProxy(L, 1);
        if (proxy->path.empty() && lua_type(L, 2) == LUA_TSTRING && isModelMethod(lua_tostring(L, 2))){
            lua_getmetatable(L, 1);
            lua_getfield(L, -1, "methods");
            lua_getfield(L, -1, lua_tostring(L, 2));
            return 1;
        }
        ErrorText error;
        if (!proxyIndexImpl(L, error))
            return luaL_error(L, "%s", error);
        return 1;
    }

    // proxy[key] = value
    bool proxyNewIndexImpl(lua_State* L, ErrorText error){
        const ModelProxy& proxy = *checkProxy(L, 1);
        HtmlUIDataModelRecord* record = proxy.state ? proxy.state->record : nullptr;
        if (!record){
            setError(error, "data model '" + (proxy.state ? proxy.state->name : std::string()) + "' was removed");
            return false;
        }
        if (lua_type(L, 3) == LUA_TFUNCTION){
            setError(error, describePath(proxy) + ": functions are event callbacks, given in the table of createDataModel");
            return false;
        }

        UIValue value;
        std::string conversionError;
        if (!HtmlUILua::toValue(L, 3, value, conversionError)){
            setError(error, describePath(proxy) + ": " + conversionError);
            return false;
        }

        // the top level: its keys are fixed
        if (proxy.path.empty()){
            const char* name = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : nullptr;
            UIValue* variable = name ? record->variable(name) : nullptr;
            if (!variable){
                setError(error, "data model '" + record->name + "' has no variable '" + (name ? std::string(name) : std::string("?")) +
                                "' (its top-level keys are fixed when it is created)");
                return false;
            }
            *variable = std::move(value);
            record->dirty(name);
            return true;
        }

        UIValue* target = proxyValue(proxy);
        if (!target){
            setError(error, describePath(proxy) + " no longer exists");
            return false;
        }

        if (lua_isinteger(L, 2) && (target->getType() == UIValue::Type::ARRAY)){
            const lua_Integer index = lua_tointeger(L, 2);
            const size_t size = target->size();
            if (index >= 1 && (size_t)index <= size){
                if (value.isNil() && (size_t)index == size){
                    target->erase(size - 1);          // table.remove
                }else{
                    HtmlUIScriptAccess::items(*target)[(size_t)(index - 1)] = std::move(value);
                }
            }else if ((size_t)index == size + 1){
                if (!value.isNil())
                    target->push(std::move(value));   // table.insert / t[#t + 1]
            }else{
                setError(error, describePath(proxy) + ": index " + std::to_string((long long)index) + " is out of range (1.." +
                                std::to_string(size + 1) + ")");
                return false;
            }
        }else if (lua_type(L, 2) == LUA_TSTRING &&
                   (target->getType() == UIValue::Type::OBJECT || (target->getType() == UIValue::Type::ARRAY && target->size() == 0))){
            const std::string key = lua_tostring(L, 2);
            if (value.isNil()){
                target->erase(key);
            }else{
                (*target)[key] = std::move(value);
            }
        }else{
            setError(error, describePath(proxy) + " cannot take this key");
            return false;
        }

        record->dirty(proxy.path[0].name);
        return true;
    }

    int proxyNewIndex(lua_State* L){
        ErrorText error;
        if (!proxyNewIndexImpl(L, error))
            return luaL_error(L, "%s", error);
        return 0;
    }

    int proxyLength(lua_State* L){
        const ModelProxy& proxy = *checkProxy(L, 1);
        const UIValue* value = proxyValue(proxy);
        lua_pushinteger(L, (value && value->getType() == UIValue::Type::ARRAY) ? (lua_Integer)value->size() : 0);
        return 1;
    }

    // pairs(proxy): (proxy, key) -> next key, value
    bool proxyNextImpl(lua_State* L, ErrorText error){
        const ModelProxy& proxy = *checkProxy(L, 1);
        HtmlUIDataModelRecord* record = proxy.state ? proxy.state->record : nullptr;
        if (!record){
            setError(error, "data model '" + (proxy.state ? proxy.state->name : std::string()) + "' was removed");
            return false;
        }

        if (proxy.path.empty()){
            size_t next = 0;
            if (lua_type(L, 2) == LUA_TSTRING){
                auto it = std::find(record->order.begin(), record->order.end(), std::string(lua_tostring(L, 2)));
                next = (it == record->order.end()) ? record->order.size() : (size_t)(it - record->order.begin()) + 1;
            }
            if (next >= record->order.size()){
                lua_pushnil(L);
                return true;
            }
            PathKey key;
            key.name = record->order[next];
            lua_pushstring(L, key.name.c_str());
            pushChild(L, proxy, record->variable(key.name), key);
            return true;
        }

        const UIValue* value = proxyValue(proxy);
        if (value && value->getType() == UIValue::Type::ARRAY){
            const lua_Integer next = lua_isinteger(L, 2) ? lua_tointeger(L, 2) + 1 : 1;
            if (next < 1 || (size_t)next > value->size()){
                lua_pushnil(L);
                return true;
            }
            PathKey key;
            key.isIndex = true;
            key.index = (size_t)(next - 1);
            lua_pushinteger(L, next);
            pushChild(L, proxy, &HtmlUIScriptAccess::items(*value)[key.index], key);
            return true;
        }
        if (value && value->getType() == UIValue::Type::OBJECT){
            const std::vector<std::string>& keys = HtmlUIScriptAccess::objectKeys(*value);
            size_t next = 0;
            if (lua_type(L, 2) == LUA_TSTRING){
                auto it = std::find(keys.begin(), keys.end(), std::string(lua_tostring(L, 2)));
                next = (it == keys.end()) ? keys.size() : (size_t)(it - keys.begin()) + 1;
            }
            if (next >= keys.size()){
                lua_pushnil(L);
                return true;
            }
            PathKey key;
            key.name = keys[next];
            lua_pushstring(L, key.name.c_str());
            pushChild(L, proxy, &HtmlUIScriptAccess::items(*value)[next], key);
            return true;
        }
        lua_pushnil(L);
        return true;
    }

    int proxyNext(lua_State* L){
        lua_settop(L, 2);
        ErrorText error;
        if (!proxyNextImpl(L, error))
            return luaL_error(L, "%s", error);
        return lua_isnil(L, -1) ? 1 : 2;
    }

    int proxyPairs(lua_State* L){
        checkProxy(L, 1);
        lua_pushcfunction(L, proxyNext);
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        return 3;
    }

    int proxyToString(lua_State* L){
        const ModelProxy& proxy = *checkProxy(L, 1);
        std::string text = describePath(proxy);
        if (!proxy.state || !proxy.state->record)
            text += " (removed)";
        lua_pushstring(L, text.c_str());
        return 1;
    }

    int proxyEquals(lua_State* L){
        const ModelProxy* a = static_cast<const ModelProxy*>(luaL_testudata(L, 1, kModelMetatable));
        const ModelProxy* b = static_cast<const ModelProxy*>(luaL_testudata(L, 2, kModelMetatable));
        bool same = a && b && a->state == b->state && a->path.size() == b->path.size();
        for (size_t i = 0; same && i < a->path.size(); i++){
            same = a->path[i].isIndex == b->path[i].isIndex && a->path[i].index == b->path[i].index && a->path[i].name == b->path[i].name;
        }
        lua_pushboolean(L, same);
        return 1;
    }

    int proxyGc(lua_State* L){
        ModelProxy* proxy = static_cast<ModelProxy*>(luaL_checkudata(L, 1, kModelMetatable));
        proxy->~ModelProxy();
        return 0;
    }

    void registerModelMetatable(lua_State* L){
        luaL_newmetatable(L, kModelMetatable);
        const luaL_Reg metamethods[] = {
            {"__index", proxyIndex}, {"__newindex", proxyNewIndex}, {"__len", proxyLength},
            {"__pairs", proxyPairs}, {"__tostring", proxyToString}, {"__eq", proxyEquals},
            {"__gc", proxyGc}, {nullptr, nullptr}};
        luaL_setfuncs(L, metamethods, 0);
        lua_pushstring(L, "UIDataModel");
        lua_setfield(L, -2, "__name");

        lua_newtable(L);
        const luaL_Reg methods[] = {
            {"onChange", modelOnChange}, {"removeOnChange", modelRemoveOnChange}, {"dirty", modelDirty},
            {"dirtyAll", modelDirtyAll}, {"getName", modelGetName}, {"isValid", modelIsValid},
            {"get", modelGet}, {"set", modelSet}, {nullptr, nullptr}};
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "methods");
        lua_pop(L, 1);
    }

    // ---- createDataModel / getDataModel ----

    // doc:createDataModel(name, table): functions become data-event callbacks, the rest the tree
    bool createModelImpl(lua_State* L, UIDocument* document, ErrorText error){
        const std::string name = lua_tostring(L, 2);
        UIValue initial = UIValue::object();
        std::vector<std::string> functions;

        lua_pushnil(L);
        while (lua_next(L, 3) != 0){
            if (lua_type(L, -2) != LUA_TSTRING){
                lua_pop(L, 2);
                setError(error, "createDataModel('" + name + "'): every key of the table must be a string");
                return false;
            }
            const std::string key = lua_tostring(L, -2);
            if (isModelMethod(key.c_str())){
                lua_pop(L, 2);
                setError(error, "createDataModel('" + name + "'): '" + key + "' is the name of a data model method");
                return false;
            }
            if (lua_type(L, -1) == LUA_TFUNCTION){
                functions.push_back(key);
            }else{
                std::string conversionError;
                if (!HtmlUILua::toValue(L, -1, initial[key], conversionError)){
                    lua_pop(L, 2);
                    setError(error, "createDataModel('" + name + "'): '" + key + "': " + conversionError);
                    return false;
                }
            }
            lua_pop(L, 1);
        }

        // a stable order of the top-level keys
        UIValue sorted = UIValue::object();
        std::vector<std::string> keys = initial.keys();
        std::sort(keys.begin(), keys.end());
        for (const std::string& key : keys){
            sorted[key] = *initial.find(key);
        }

        UIDocumentSystem* system = document->getScene()->getSystem<UIDocumentSystem>().get();
        HtmlUIScriptState& script = system->getScriptState();
        std::string createError;
        HtmlUIDataModelRecord* record = script.createModel(document->getEntity(), name, sorted, createError);
        if (!record){
            setError(error, "createDataModel: " + createError);
            return false;
        }

        std::sort(functions.begin(), functions.end());
        for (const std::string& key : functions){
            lua_getfield(L, 3, key.c_str());
            record->events[key].lua = HtmlUILuaRef(L, -1);
            lua_pop(L, 1);
            record->bindEventName(key);
        }

        std::shared_ptr<UIDataModelState> state = record->state;
        // RmlUi binds data-model while parsing: a loaded document that uses it reloads now
        script.reloadUsersOf(document->getEntity(), name);
        HtmlUILua::pushModel(L, state);
        return true;
    }

    int documentCreateDataModel(lua_State* L){
        UIDocument* self = luabridge::detail::Userdata::get<UIDocument>(L, 1, false);
        luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        if (!self)
            return luaL_argerror(L, 1, "UIDocument expected");
        ErrorText error;
        if (!createModelImpl(L, self, error))
            return luaL_error(L, "%s", error);
        return 1;
    }

    int documentGetDataModel(lua_State* L){
        UIDocument* self = luabridge::detail::Userdata::get<UIDocument>(L, 1, false);
        luaL_checkstring(L, 2);
        if (!self)
            return luaL_argerror(L, 1, "UIDocument expected");
        UIDocumentSystem* system = self->getScene()->getSystem<UIDocumentSystem>().get();
        if (!system->pushDataModel(L, self->getEntity(), lua_tostring(L, 2)))
            lua_pushnil(L);
        return 1;
    }

    // ---- UIValue <-> Lua ----

    bool toValueImpl(lua_State* L, int index, UIValue& out, std::string& error, int depth){
        index = lua_absindex(L, index);
        // every level pushes a few values: a deep table must not overflow the C stack
        if (depth > 64 || !lua_checkstack(L, 8)){
            error = "the table nests too deep (or contains itself)";
            return false;
        }
        switch (lua_type(L, index)){
            case LUA_TNIL:
            case LUA_TNONE:
                out = UIValue();
                return true;
            case LUA_TBOOLEAN:
                out = UIValue((bool)lua_toboolean(L, index));
                return true;
            case LUA_TNUMBER:
                out = UIValue((double)lua_tonumber(L, index));
                return true;
            case LUA_TSTRING: {
                size_t length = 0;
                const char* text = lua_tolstring(L, index, &length);
                out = UIValue(std::string(text, length));
                return true;
            }
            case LUA_TUSERDATA:
                if (const ModelProxy* proxy = static_cast<const ModelProxy*>(luaL_testudata(L, index, kModelMetatable))){
                    if (const UIValue* value = proxyValue(*proxy)){
                        out = *value;
                        return true;
                    }
                    error = describePath(*proxy) + " no longer exists";
                    return false;
                }
                error = "a userdata cannot be stored (only nil, booleans, numbers, strings and tables)";
                return false;
            case LUA_TTABLE: {
                // 1..n keys only: an array (an empty table too); else an object with string keys
                const lua_Unsigned length = lua_rawlen(L, index);
                size_t count = 0;
                bool sequence = true;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    count++;
                    if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || (lua_Unsigned)lua_tointeger(L, -2) > length)
                        sequence = false;
                    lua_pop(L, 1);
                }
                if (sequence && count == length){
                    out = UIValue::array();
                    for (lua_Unsigned i = 1; i <= length; i++){
                        lua_rawgeti(L, index, (lua_Integer)i);
                        UIValue item;
                        const bool ok = toValueImpl(L, -1, item, error, depth + 1);
                        lua_pop(L, 1);
                        if (!ok)
                            return false;
                        out.push(std::move(item));
                    }
                    return true;
                }
                std::vector<std::string> keys;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    if (lua_type(L, -2) == LUA_TSTRING){
                        keys.push_back(lua_tostring(L, -2));
                    }else if (lua_type(L, -2) == LUA_TNUMBER){
                        lua_pushvalue(L, -2);
                        keys.push_back(lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }else{
                        lua_pop(L, 2);
                        error = "table keys must be strings or numbers";
                        return false;
                    }
                    lua_pop(L, 1);
                }
                std::sort(keys.begin(), keys.end());
                out = UIValue::object();
                for (const std::string& key : keys){
                    lua_pushstring(L, key.c_str());
                    lua_rawget(L, index);
                    if (lua_isnil(L, -1)){
                        // a numeric key stored as a number
                        lua_pop(L, 1);
                        lua_pushnumber(L, std::strtod(key.c_str(), nullptr));
                        lua_rawget(L, index);
                    }
                    UIValue item;
                    const bool ok = toValueImpl(L, -1, item, error, depth + 1);
                    lua_pop(L, 1);
                    if (!ok){
                        // the key that holds the bad value, once
                        if (depth == 0)
                            error = "'" + key + "': " + error;
                        return false;
                    }
                    out[key] = std::move(item);
                }
                return true;
            }
            case LUA_TFUNCTION:
                error = "a function cannot be stored (functions are event callbacks at the top level of createDataModel)";
                return false;
            default:
                error = std::string("a ") + luaL_typename(L, index) + " cannot be stored";
                return false;
        }
    }

}

// ---- HtmlUILuaRef ----

HtmlUILuaRef::HtmlUILuaRef(lua_State* from, int index): L(LuaBinding::mainThread(from)){
    // the main thread is kept, never a coroutine's (it ends long before a listener fires)
    lua_pushvalue(from, index);
    ref = luaL_ref(from, LUA_REGISTRYINDEX);
    liveRefs++;
}

HtmlUILuaRef::~HtmlUILuaRef(){
    reset();
}

HtmlUILuaRef::HtmlUILuaRef(HtmlUILuaRef&& other) noexcept: L(other.L), ref(other.ref){
    other.L = nullptr;
    other.ref = LUA_NOREF;
}

HtmlUILuaRef& HtmlUILuaRef::operator=(HtmlUILuaRef&& other) noexcept{
    if (this != &other){
        reset();
        L = other.L;
        ref = other.ref;
        other.L = nullptr;
        other.ref = LUA_NOREF;
    }
    return *this;
}

bool HtmlUILuaRef::isSet() const{
    return ref != LUA_NOREF && ref != LUA_REFNIL;
}

bool HtmlUILuaRef::push(lua_State* current) const{
    // the state closes before the scenes are destroyed (Engine::systemShutdown)
    if (!isSet() || !current || LuaBinding::getLuaState() != L || LuaBinding::mainThread(current) != L)
        return false;
    // on the thread running now (a coroutine's, or the main one)
    lua_rawgeti(current, LUA_REGISTRYINDEX, ref);
    return true;
}

void HtmlUILuaRef::reset(){
    if (ref != LUA_NOREF){
        if (L && LuaBinding::getLuaState() == L)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        liveRefs--;
    }
    L = nullptr;
    ref = LUA_NOREF;
}

size_t HtmlUIScriptState::liveLuaRefs(){
    return liveRefs;
}

// ---- HtmlUILua ----

void HtmlUILua::pushValue(lua_State* L, const UIValue& value){
    if (!lua_checkstack(L, 4))
        return;
    switch (value.getType()){
        case UIValue::Type::BOOL:
            lua_pushboolean(L, value.asBool());
            break;
        case UIValue::Type::NUMBER: {
            const double number = value.asNumber();
            if (std::floor(number) == number && std::fabs(number) < 9007199254740992.0){
                lua_pushinteger(L, (lua_Integer)number);
            }else{
                lua_pushnumber(L, number);
            }
            break;
        }
        case UIValue::Type::STRING:
            lua_pushlstring(L, value.asString().data(), value.asString().size());
            break;
        case UIValue::Type::ARRAY: {
            const std::vector<UIValue>& items = HtmlUIScriptAccess::items(value);
            lua_createtable(L, (int)items.size(), 0);
            for (size_t i = 0; i < items.size(); i++){
                pushValue(L, items[i]);
                lua_rawseti(L, -2, (lua_Integer)(i + 1));
            }
            break;
        }
        case UIValue::Type::OBJECT: {
            const std::vector<UIValue>& items = HtmlUIScriptAccess::items(value);
            const std::vector<std::string>& keys = HtmlUIScriptAccess::objectKeys(value);
            lua_createtable(L, 0, (int)items.size());
            for (size_t i = 0; i < items.size(); i++){
                pushValue(L, items[i]);
                lua_setfield(L, -2, keys[i].c_str());
            }
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
}

bool HtmlUILua::toValue(lua_State* L, int index, UIValue& out, std::string& error){
    return toValueImpl(L, index, out, error, 0);
}

void HtmlUILua::pushEvent(lua_State* L, const UIEvent& event){
    if (!luabridge::push(L, event))
        lua_pushnil(L);
}

void HtmlUILua::pushElement(lua_State* L, const UIElement& element){
    if (!luabridge::push(L, optionalElement(element)))
        lua_pushnil(L);
}

void HtmlUILua::pushModel(lua_State* L, const std::shared_ptr<UIDataModelState>& state){
    pushProxy(L, state, std::vector<PathKey>());
}

bool HtmlUILua::runDocumentScript(UIDocument& document, const htmlui::compat::DocumentScript& script, std::string* error){
    lua_State* L = LuaBinding::getLuaState();
    if (!L){
        if (error)
            *error = "no Lua state";
        return false;
    }
    const int top = lua_gettop(L);

    // as many line breaks as the code starts after, so Lua's line numbers are the file's
    std::string code(script.firstLine > 1 ? script.firstLine - 1 : 0, '\n');
    code += script.code;
    const std::string chunk = "=" + (script.sourceFile.empty() ? script.chunkName : script.sourceFile);
    if (luaL_loadbuffer(L, code.data(), code.size(), chunk.c_str()) != LUA_OK){
        if (error)
            *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "syntax error";
        lua_settop(L, top);
        return false;
    }

    // a fresh environment per script: globals stay readable (__index = _G), what the script
    // defines stays its own
    lua_newtable(L);
    lua_newtable(L);
    if (!LuaBinding::pushPreviewEnvironment(L)) lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    if (!luabridge::push(L, document))
        lua_pushnil(L);
    lua_setfield(L, -2, "document");
    lua_getglobal(L, "anime");
    lua_setfield(L, -2, "anime");
    if (!lua_setupvalue(L, -2, 1))   // _ENV
        lua_pop(L, 1);

    if (LuaBinding::pcallWithTraceback(L, 0, 0) != LUA_OK){
        if (error)
            *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "runtime error";
        lua_settop(L, top);
        return false;
    }
    lua_settop(L, top);
    return true;
}

bool HtmlUILua::call(lua_State* L, int nargs, const std::string& what){
    if (LuaBinding::pcallWithTraceback(L, nargs, 0) != LUA_OK){
        Log::error("[HTML UI] Lua error in %s: %s", what.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// ---- registration ----

void LuaBinding::registerHtmlUIClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    registerModelMetatable(L);

    // getBoundingClientRect: document px, through transforms
    luabridge::getGlobalNamespace(L)
        .beginClass<UIRect>("UIRect")
        .addProperty("x", &UIRect::x)
        .addProperty("y", &UIRect::y)
        .addProperty("width", &UIRect::width)
        .addProperty("height", &UIRect::height)
        .addProperty("top", &UIRect::top)
        .addProperty("left", &UIRect::left)
        .addProperty("right", &UIRect::right)
        .addProperty("bottom", &UIRect::bottom)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIEvent>("UIEvent")
        .addFunction("isValid", &UIEvent::isValid)
        .addProperty("type", &UIEvent::getType)
        .addProperty("target", [](const UIEvent* self) -> std::optional<UIElement> { return optionalElement(self->getTarget()); })
        .addProperty("currentTarget", [](const UIEvent* self) -> std::optional<UIElement> { return optionalElement(self->getCurrentTarget()); })
        .addProperty("eventPhase", &UIEvent::getEventPhase)
        .addProperty("key", &UIEvent::getKey)
        .addProperty("code", &UIEvent::getCode)
        .addProperty("keyCode", &UIEvent::getKeyCode)
        .addProperty("repeat", &UIEvent::isRepeat)
        .addProperty("gamepad", &UIEvent::getGamepad)
        .addProperty("ctrlKey", &UIEvent::getCtrlKey)
        .addProperty("shiftKey", &UIEvent::getShiftKey)
        .addProperty("altKey", &UIEvent::getAltKey)
        .addProperty("metaKey", &UIEvent::getMetaKey)
        .addProperty("clientX", &UIEvent::getClientX)
        .addProperty("clientY", &UIEvent::getClientY)
        .addProperty("button", &UIEvent::getButton)
        .addProperty("deltaX", &UIEvent::getDeltaX)
        .addProperty("deltaY", &UIEvent::getDeltaY)
        .addProperty("pointerId", &UIEvent::getPointerId)
        .addProperty("pointerType", &UIEvent::getPointerType)
        .addProperty("isPrimary", &UIEvent::isPrimary)
        .addProperty("buttons", &UIEvent::getButtons)
        .addProperty("pressure", &UIEvent::getPressure)
        .addProperty("width", &UIEvent::getWidth)
        .addProperty("height", &UIEvent::getHeight)
        .addProperty("fingerId", &UIEvent::getFingerId)
        .addProperty("data", &UIEvent::getData)
        .addProperty("animationName", &UIEvent::getAnimationName)
        .addProperty("propertyName", &UIEvent::getPropertyName)
        .addProperty("elapsedTime", &UIEvent::getElapsedTime)
        // the dispatchEvent detail, copied (nil, boolean, number, string or table)
        .addProperty("detail", [](const UIEvent* self, lua_State* L) -> luabridge::LuaRef {
            HtmlUILua::pushValue(L, self->getDetail());
            luabridge::LuaRef value = luabridge::LuaRef::fromStack(L);
            return value;
        })
        .addProperty("defaultPrevented", &UIEvent::isDefaultPrevented)
        .addFunction("stopPropagation", &UIEvent::stopPropagation)
        .addFunction("stopImmediatePropagation", &UIEvent::stopImmediatePropagation)
        .addFunction("preventDefault", &UIEvent::preventDefault)
        .endClass();

    // In Lua, any member but isValid() of an invalid element raises an error
    luabridge::getGlobalNamespace(L)
        .beginClass<UIElement>("UIElement")
        .addFunction("isValid", &UIElement::isValid)
        .addFunction("__eq", &UIElement::operator==)
        .addFunction("__tostring", [](const UIElement* self) -> std::string { return describeElement(*self); })
        .addProperty("id", &UIElement::getId, &UIElement::setId)
        .addProperty("tagName", &UIElement::getTagName)
        .addProperty("className", &UIElement::getClassName, &UIElement::setClassName)
        .addProperty("textContent", &UIElement::getTextContent, &UIElement::setTextContent)
        .addProperty("innerHTML", &UIElement::getInnerHTML, &UIElement::setInnerHTML)
        .addProperty("parentElement", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getParentElement()); })
        .addProperty("children", &UIElement::getChildren)
        .addProperty("firstElementChild", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getFirstElementChild()); })
        .addProperty("lastElementChild", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getLastElementChild()); })
        .addProperty("nextElementSibling", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getNextElementSibling()); })
        .addProperty("previousElementSibling", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getPreviousElementSibling()); })
        .addProperty("childElementCount", &UIElement::getChildElementCount)
        .addProperty("value", &UIElement::getValue, &UIElement::setValue)
        .addProperty("checked", &UIElement::getChecked, &UIElement::setChecked)
        .addProperty("disabled", &UIElement::getDisabled, &UIElement::setDisabled)
        .addProperty("scrollTop", &UIElement::getScrollTop, &UIElement::setScrollTop)
        .addProperty("scrollLeft", &UIElement::getScrollLeft, &UIElement::setScrollLeft)
        .addProperty("scrollWidth", &UIElement::getScrollWidth)
        .addProperty("scrollHeight", &UIElement::getScrollHeight)
        .addProperty("clientWidth", &UIElement::getClientWidth)
        .addProperty("clientHeight", &UIElement::getClientHeight)
        .addProperty("offsetWidth", &UIElement::getOffsetWidth)
        .addProperty("offsetHeight", &UIElement::getOffsetHeight)
        .addProperty("offsetLeft", &UIElement::getOffsetLeft)
        .addProperty("offsetTop", &UIElement::getOffsetTop)
        .addProperty("classList", [](const UIElement* self) -> UIClassList { return UIClassList{*self}; })
        .addProperty("style", [](const UIElement* self) -> UIStyle { return UIStyle{*self}; })
        .addProperty("dataset", [](const UIElement* self) -> UIDataset { return UIDataset{*self}; })
        // nil when the attribute is absent
        .addFunction("getAttribute", [](const UIElement* self, const std::string& name) -> std::optional<std::string> {
            return self->hasAttribute(name) ? std::optional<std::string>(self->getAttribute(name)) : std::nullopt;
        })
        .addFunction("setAttribute", &UIElement::setAttribute)
        .addFunction("hasAttribute", &UIElement::hasAttribute)
        .addFunction("removeAttribute", &UIElement::removeAttribute)
        .addFunction("getAttributes", &UIElement::getAttributes)
        .addFunction("getEffects", [](const UIElement* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef list = luabridge::newTable(L);
            int index = 1;
            for (const UIEffectInfo& effect : self->getEffects()){
                luabridge::LuaRef item = luabridge::newTable(L);
                item["property"] = effect.property;
                item["index"] = effect.index;
                item["name"] = effect.name;
                item["form"] = effect.form;
                luabridge::LuaRef params = luabridge::newTable(L);
                for (const auto& param : effect.params)
                    params[param.first] = param.second;
                item["params"] = params;
                // phase 6: a stateful effect's simulation
                item["stateful"] = effect.stateful;
                if (effect.stateful){
                    item["stateBytes"] = (double)effect.stateBytes;
                    item["steps"] = (double)effect.steps;
                    item["simTime"] = effect.simTime;
                    item["paused"] = effect.paused;
                }
                list[index++] = item;
            }
            return list;
        })
        .addFunction("effectSplat", [](UIElement* self, const luabridge::LuaRef& options) -> int {
            UIEffectSplat splat;
            if (!options.isTable()){
                Log::warn("[HTML UI] effectSplat takes a table: { x=, y=, radius=, dx=, dy=, color=, strength=, effect= }");
                return 0;
            }
            auto number = [&](const char* key, float fallback){
                const luabridge::LuaRef value = options[key];
                return value.isNumber() ? value.unsafe_cast<float>() : fallback;
            };
            splat.x = number("x", 0.f);
            splat.y = number("y", 0.f);
            splat.radius = number("radius", splat.radius);
            splat.dx = number("dx", 0.f);
            splat.dy = number("dy", 0.f);
            splat.strength = number("strength", splat.strength);
            const luabridge::LuaRef color = options["color"];
            if (color.isString())
                splat.color = color.unsafe_cast<std::string>();
            const luabridge::LuaRef effect = options["effect"];
            if (effect.isString())
                splat.effect = effect.unsafe_cast<std::string>();
            return self->effectSplat(splat);
        })
        .addFunction("effectReset", [](UIElement* self, std::optional<std::string> effect) -> int {
            return self->effectReset(effect.value_or(std::string()));
        })
        .addFunction("querySelector", [](const UIElement* self, const std::string& selector) -> std::optional<UIElement> {
            return optionalElement(self->querySelector(selector));
        })
        .addFunction("querySelectorAll", &UIElement::querySelectorAll)
        .addFunction("closest", [](const UIElement* self, const std::string& selector) -> std::optional<UIElement> {
            return optionalElement(self->closest(selector));
        })
        .addFunction("matches", &UIElement::matches)
        .addFunction("contains", &UIElement::contains)
        // moves child here (from its parent, or detached); returns it
        .addFunction("appendChild", [](UIElement* self, const UIElement& child) -> std::optional<UIElement> {
            return optionalElement(self->appendChild(child));
        })
        // before reference (nil appends)
        .addFunction("insertBefore", [](UIElement* self, const UIElement& child, std::optional<UIElement> reference) -> std::optional<UIElement> {
            return optionalElement(self->insertBefore(child, reference ? *reference : UIElement()));
        })
        // the removed child stays usable (and insertable) while a handle of it exists
        .addFunction("removeChild", [](UIElement* self, const UIElement& child) -> std::optional<UIElement> {
            return optionalElement(self->removeChild(child));
        })
        .addFunction("replaceChild", [](UIElement* self, const UIElement& newChild, const UIElement& oldChild) -> std::optional<UIElement> {
            return optionalElement(self->replaceChild(newChild, oldChild));
        })
        .addFunction("remove", &UIElement::remove)
        .addFunction("cloneNode", [](const UIElement* self, std::optional<bool> deep) -> std::optional<UIElement> {
            return optionalElement(self->cloneNode(deep.value_or(false)));
        })
        .addFunction("focus", &UIElement::focus)
        .addFunction("blur", &UIElement::blur)
        .addFunction("click", &UIElement::click)
        .addFunction("setPointerCapture", &UIElement::setPointerCapture)
        .addFunction("releasePointerCapture", &UIElement::releasePointerCapture)
        .addFunction("hasPointerCapture", &UIElement::hasPointerCapture)
        .addFunction("scrollIntoView", [](UIElement* self, std::optional<bool> alignToTop) { self->scrollIntoView(alignToTop.value_or(true)); })
        .addFunction("scrollTo", [](UIElement* self, float x, float y, std::optional<bool> smooth) { self->scrollTo(x, y, smooth.value_or(false)); })
        .addFunction("scrollBy", [](UIElement* self, float dx, float dy, std::optional<bool> smooth) { self->scrollBy(dx, dy, smooth.value_or(false)); })
        .addFunction("getBoundingClientRect", &UIElement::getBoundingClientRect)
        .addFunction("addEventListener", &elementAddEventListener)
        .addFunction("removeEventListener", &elementRemoveEventListener)
        .addFunction("dispatchEvent", &elementDispatchEvent)
        .addFunction("hasClass", &UIElement::hasClass)
        .addFunction("setClass", &UIElement::setClass)
        .addFunction("setStyleProperty", &UIElement::setStyleProperty)
        .addFunction("removeStyleProperty", &UIElement::removeStyleProperty)
        .addFunction("getBoundingBox", &UIElement::getBoundingBox)
        .addFunction("isPseudoClassSet", &UIElement::isPseudoClassSet)
        // phase 2 (p2-html): HTML documents
        .addProperty("localName", &UIElement::getLocalName)
        .addProperty("outerHTML", &UIElement::getOuterHTML, &UIElement::setOuterHTML)
        .addFunction("insertAdjacentHTML", &UIElement::insertAdjacentHTML)
        // <template>: a fragment with a copy of its content (cloneNode it, append it)
        .addProperty("content", [](const UIElement* self) -> std::optional<UIElement> { return optionalElement(self->getContent()); })
        // <dialog>
        .addFunction("show", &UIElement::show)
        .addFunction("showModal", &UIElement::showModal)
        .addFunction("close", [](UIElement* self, std::optional<std::string> returnValue) { self->close(returnValue.value_or(std::string())); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIClassList>("UIClassList")
        .addFunction("add", &classListAdd)
        .addFunction("remove", &classListRemove)
        // flips the class (or sets it to force); returns whether it is set now
        .addFunction("toggle", [](UIClassList* self, const std::string& name, std::optional<bool> force) -> bool {
            return force ? self->element.toggleClass(name, *force) : self->element.toggleClass(name);
        })
        .addFunction("contains", [](const UIClassList* self, const std::string& name) -> bool { return self->element.containsClass(name); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIStyle>("UIStyle")
        // the computed value as CSS text ("" when the property is unknown)
        .addFunction("getPropertyValue", [](const UIStyle* self, const std::string& name) -> std::string {
            return self->element.getStyle(cssName(name));
        })
        // an inline declaration; "" removes it
        .addFunction("setProperty", [](UIStyle* self, const std::string& name, const std::string& value) {
            self->element.setStyle(cssName(name), value);
        })
        // removes the inline declaration; returns the computed value it had
        .addFunction("removeProperty", [](UIStyle* self, const std::string& name) -> std::string {
            const std::string old = self->element.getStyle(cssName(name));
            self->element.removeStyle(cssName(name));
            return old;
        })
        // (LuaBridge asks the fallback first: nil for the method names lets it find them)
        .addIndexMetaMethod([](UIStyle& self, const luabridge::LuaRef& key, lua_State* L) -> luabridge::LuaRef {
            if (!key.isString() || isStyleMethod(key.unsafe_cast<std::string>()))
                return luabridge::LuaRef(L);
            // the inline declarations as text (phase 2)
            if (key.unsafe_cast<std::string>() == "cssText")
                return luabridge::LuaRef(L, self.element.getCssText());
            return luabridge::LuaRef(L, self.element.getStyle(cssName(key.unsafe_cast<std::string>())));
        })
        .addNewIndexMetaMethod([](UIStyle& self, const luabridge::LuaRef& key, const luabridge::LuaRef& value, lua_State* L) -> luabridge::LuaRef {
            if (key.isString() && key.unsafe_cast<std::string>() == "cssText"){
                self.element.setCssText(value.isNil() ? std::string() : value.tostring());
                return luabridge::LuaRef(L);
            }
            if (key.isString()){
                const std::string name = cssName(key.unsafe_cast<std::string>());
                if (value.isNil()){
                    self.element.removeStyle(name);
                }else{
                    self.element.setStyle(name, value.tostring());
                }
            }
            return luabridge::LuaRef(L);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIDataset>("UIDataset")
        .addIndexMetaMethod([](UIDataset& self, const luabridge::LuaRef& key, lua_State* L) -> luabridge::LuaRef {
            if (!key.isString())
                return luabridge::LuaRef(L);
            const std::string name = key.unsafe_cast<std::string>();
            const std::string attribute = "data-" + cssName(name);
            if (!self.element.hasAttribute(attribute))
                return luabridge::LuaRef(L);
            return luabridge::LuaRef(L, self.element.getDataset(name));
        })
        .addNewIndexMetaMethod([](UIDataset& self, const luabridge::LuaRef& key, const luabridge::LuaRef& value, lua_State* L) -> luabridge::LuaRef {
            if (key.isString()){
                const std::string name = key.unsafe_cast<std::string>();
                if (value.isNil()){
                    self.element.removeAttribute("data-" + cssName(name));
                }else{
                    self.element.setDataset(name, value.tostring());
                }
            }
            return luabridge::LuaRef(L);
        })
        .endClass();

    guardClass<UIElement>(L);
    guardClass<UIClassList>(L);
    guardClass<UIStyle>(L);
    guardClass<UIDataset>(L);

    // p1-rtt: SCREEN draws over the scene, TEXTURE into UIDocument:getTargetTexture()
    luabridge::getGlobalNamespace(L)
        .beginNamespace("UIDocumentRenderMode")
        .addVariable("SCREEN", UIDocumentRenderMode::SCREEN)
        .addVariable("TEXTURE", UIDocumentRenderMode::TEXTURE)
        .endNamespace();

    // the component of a UIDocument (UIDocument:getUIDocumentComponent())
    luabridge::getGlobalNamespace(L)
        .beginClass<UIDocumentComponent>("UIDocumentComponent")
        .addProperty("filename", &UIDocumentComponent::filename, &setTrackedMember<&UIDocumentComponent::filename>)
        .addProperty("sortOrder", &UIDocumentComponent::sortOrder, &setTrackedMember<&UIDocumentComponent::sortOrder>)
        .addProperty("enabled", &UIDocumentComponent::enabled, &setTrackedMember<&UIDocumentComponent::enabled>)
        .addProperty("interactive", &UIDocumentComponent::interactive, &setTrackedMember<&UIDocumentComponent::interactive>)
        .addProperty("loaded", &UIDocumentComponent::loaded)
        .addProperty("needReload", &UIDocumentComponent::needReload, &setTrackedMember<&UIDocumentComponent::needReload>)
        // p1-rtt: a mode change takes effect at the next (re)load; size and ratio at once
        .addProperty("renderMode", &UIDocumentComponent::renderMode, &setTrackedMember<&UIDocumentComponent::renderMode>)
        .addProperty("textureWidth", &UIDocumentComponent::textureWidth, &setTrackedMember<&UIDocumentComponent::textureWidth>)
        .addProperty("textureHeight", &UIDocumentComponent::textureHeight, &setTrackedMember<&UIDocumentComponent::textureHeight>)
        .addProperty("inputSurface", &UIDocumentComponent::inputSurface, &setTrackedMember<&UIDocumentComponent::inputSurface>)
        .addProperty("pixelRatio", &UIDocumentComponent::pixelRatio, &setTrackedMember<&UIDocumentComponent::pixelRatio>)
        .addProperty("onLoad", [] (UIDocumentComponent* self, lua_State* L) { return &self->onLoad; }, [] (UIDocumentComponent* self, lua_State* L) { self->onLoad = L; })
        .addProperty("onUnload", [] (UIDocumentComponent* self, lua_State* L) { return &self->onUnload; }, [] (UIDocumentComponent* self, lua_State* L) { self->onUnload = L; })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<UIDocument, GameObject>("UIDocument")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("filename", &UIDocument::getFilename, &UIDocument::setFilename)
        .addFunction("getUIDocumentComponent", &UIDocument::getComponent<UIDocumentComponent>)
        .addFunction("load", &UIDocument::load)
        .addFunction("reload", (void (UIDocument::*)())&UIDocument::reload)
        .addFunction("isLoaded", &UIDocument::isLoaded)
        .addProperty("loaded", &UIDocument::isLoaded)
        .addFunction("show", &UIDocument::show)
        .addFunction("hide", &UIDocument::hide)
        .addProperty("visible", &UIDocument::isVisible, &UIDocument::setVisible)
        .addProperty("sortOrder", &UIDocument::getSortOrder, &UIDocument::setSortOrder)
        .addFunction("getElementById", [](const UIDocument* self, const std::string& id) -> std::optional<UIElement> {
            return optionalElement(self->getElementById(id));
        })
        .addFunction("querySelector", [](const UIDocument* self, const std::string& selector) -> std::optional<UIElement> {
            return optionalElement(self->querySelector(selector));
        })
        .addFunction("querySelectorAll", &UIDocument::querySelectorAll)
        // the document element (RML body)
        .addProperty("body", [](const UIDocument* self) -> std::optional<UIElement> { return optionalElement(self->getBody()); })
        // a detached element: insert it with appendChild/insertBefore
        .addFunction("createElement", [](const UIDocument* self, const std::string& tag) -> std::optional<UIElement> {
            return optionalElement(self->createElement(tag));
        })
        .addFunction("createTextNode", [](const UIDocument* self, const std::string& text) -> std::optional<UIElement> {
            return optionalElement(self->createTextNode(text));
        })
        .addProperty("title", &UIDocument::getTitle, &UIDocument::setTitle)
        .addFunction("addEventListener", &documentAddEventListener)
        .addFunction("removeEventListener", &documentRemoveEventListener)
        .addFunction("createDataModel", &documentCreateDataModel)
        .addFunction("getDataModel", &documentGetDataModel)
        .addFunction("removeDataModel", &UIDocument::removeDataModel)
        .addFunction("getTargetTexture", &UIDocument::getTargetTexture)
        // S16 R28: the HtmlUIRenderStats of the last frame as a table of numbers
        .addFunction("getRenderStats", [](const UIDocument* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            self->getRenderStats().forEachField([&table](const char* name, uint32_t value){ table[name] = (lua_Integer)value; });
            return table;
        })
        .addStaticFunction("setDebuggerVisible", &UIDocument::setDebuggerVisible)
        .addStaticFunction("isDebuggerVisible", &UIDocument::isDebuggerVisible)
        // diagnostics: Lua functions held by documents (listeners, data-model callbacks); 0 once
        // no document holds any (after play stops)
        .addStaticFunction("getLiveLuaRefCount", []() -> int { return (int)HtmlUIScriptState::liveLuaRefs(); })
        // after every (re)load / before every unload of the document
        .addProperty("onLoad", [] (UIDocument* self, lua_State* L) -> FunctionSubscribe<void()>* { return &self->getComponent<UIDocumentComponent>().onLoad; }, [] (UIDocument* self, lua_State* L) { self->getComponent<UIDocumentComponent>().onLoad = L; })
        .addProperty("onUnload", [] (UIDocument* self, lua_State* L) -> FunctionSubscribe<void()>* { return &self->getComponent<UIDocumentComponent>().onUnload; }, [] (UIDocument* self, lua_State* L) { self->getComponent<UIDocumentComponent>().onUnload = L; })
        // phase 2 (p2-html): the <html> element (body is the HTML <body>), diagnostics, themes
        .addProperty("documentElement", [](const UIDocument* self) -> std::optional<UIElement> { return optionalElement(self->getDocumentElement()); })
        .addFunction("getDiagnostics", [](const UIDocument* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef list = luabridge::newTable(L);
            int index = 1;
            for (const htmlui::compat::Diagnostic& diagnostic : self->getDiagnostics()){
                luabridge::LuaRef item = luabridge::newTable(L);
                item["severity"] = std::string(htmlui::compat::severityName(diagnostic.severity));
                item["code"] = diagnostic.code.id();
                if (const htmlui::compat::DiagCodeInfo* info = htmlui::compat::findDiagnosticCode(diagnostic.code))
                    item["name"] = std::string(info->name);
                item["message"] = diagnostic.message;
                item["suggestion"] = diagnostic.suggestion;
                item["subject"] = diagnostic.subject;
                item["file"] = diagnostic.file;
                item["line"] = (int)diagnostic.line;
                item["column"] = (int)diagnostic.column;
                item["text"] = htmlui::compat::toText(diagnostic);
                list[index++] = item;
            }
            return list;
        })
        .addFunction("setColorScheme", &UIDocument::setColorScheme)
        .addFunction("setReducedMotion", &UIDocument::setReducedMotion)
        .addStaticFunction("setEffectTime", [](std::optional<double> seconds){ UIDocument::setEffectTime(seconds); })
        .addStaticFunction("getEffectTime", &UIDocument::getEffectTime)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_HTMLUI
