// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua API of the S17 widgets: UIItemsView (el.itemsView), el:setMarkdown, el:setTooltip,
// el:clearTooltip, event.dataTransfer / dropIndex / dropPosition, UIDataTransfer, UIDrag,
// doc.tooltip (UIDocumentTooltip) and UIDocument.markdownToHtml. Indices are 1-based (a
// data-model view's template keeps data-for's 0-based i). Re-opens UIElement, UIEvent and
// UIDocument as HtmlUIVectorLua.cpp does.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions below raise errors
// only where no C++ object with a destructor is alive (the work runs in helpers).

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Log.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIDataTransfer.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"
#include "object/ui/UIItemsView.h"
#include "component/UIDocumentComponent.h"
#include "Scene.h"
#include "htmlui/widgets/DragDropSession.h"
#include "htmlui/widgets/TooltipManager.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// the HTML UI module (docs/specs/S34-player-size.md R1): the Lua API description reads this condition
#if JASPERENGINE_HTMLUI

using namespace jasperengine;

namespace {

    typedef char ErrorText[256];

    // doc.tooltip: the document's tooltips
    struct UIDocumentTooltip {
        UIDocument document;
    };

    std::optional<UIElement> optionalElement(const UIElement& element){
        return element.isValid() ? std::optional<UIElement>(element) : std::nullopt;
    }

    // ---- calling Lua from the views and tooltips ----

    // pushes the function of ref; false when its state is gone
    bool pushFunction(lua_State* L, const HtmlUILuaRef& ref){
        return L && ref.isSet() && ref.push(L);
    }

    // calls the function below its nargs arguments, keeping nresults; logs a Lua error
    bool callLua(lua_State* L, int nargs, int nresults, const char* what){
        if (LuaBinding::pcallWithTraceback(L, nargs, nresults) != LUA_OK){
            Log::error("[HTML UI] Lua error in %s: %s", what, LuaBinding::getLuaStackErrorString(L, -1).c_str());
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    // a key from Lua: a string, or a number written as an integer when it is one
    std::string keyText(lua_State* L, int index){
        if (lua_isinteger(L, index))
            return std::to_string((long long)lua_tointeger(L, index));
        if (lua_type(L, index) == LUA_TNUMBER || lua_type(L, index) == LUA_TSTRING){
            size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            return std::string(text, length);
        }
        return std::string();
    }

    void pushKey(lua_State* L, const std::string& key){
        lua_pushlstring(L, key.data(), key.size());
    }

    // ---- UIItemsView:setProvider ----

    struct LuaProvider {
        HtmlUILuaRef key, height, bind, unbind, children, hasChildren;
    };

    void readFunction(lua_State* L, int table, const char* name, HtmlUILuaRef& out){
        lua_getfield(L, table, name);
        if (lua_isfunction(L, -1))
            out = HtmlUILuaRef(L, -1);
        lua_pop(L, 1);
    }

    UIItemsProvider makeProvider(lua_State* L, int table, int count){
        auto lua = std::make_shared<LuaProvider>();
        readFunction(L, table, "key", lua->key);
        readFunction(L, table, "height", lua->height);
        readFunction(L, table, "bind", lua->bind);
        readFunction(L, table, "unbind", lua->unbind);
        readFunction(L, table, "children", lua->children);
        readFunction(L, table, "hasChildren", lua->hasChildren);

        UIItemsProvider provider;
        provider.count = count;
        if (lua->key.isSet()){
            provider.key = [lua](int index) -> std::string {
                lua_State* L = LuaBinding::getLuaState();
                if (!pushFunction(L, lua->key))
                    return std::to_string(index);
                lua_pushinteger(L, index);
                if (!callLua(L, 1, 1, "an items view's key"))
                    return std::to_string(index);
                std::string key = lua_isnil(L, -1) ? std::to_string(index) : keyText(L, -1);
                lua_pop(L, 1);
                return key;
            };
        }
        if (lua->height.isSet()){
            provider.height = [lua](int index) -> float {
                lua_State* L = LuaBinding::getLuaState();
                if (!pushFunction(L, lua->height))
                    return 0.0f;
                lua_pushinteger(L, index);
                if (!callLua(L, 1, 1, "an items view's height"))
                    return 0.0f;
                const float height = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
                return height;
            };
        }
        auto rowCallback = [lua](const HtmlUILuaRef LuaProvider::* member, const char* what){
            return [lua, member, what](const UIElement& row, int index){
                lua_State* L = LuaBinding::getLuaState();
                if (!pushFunction(L, (*lua).*member))
                    return;
                HtmlUILua::pushElement(L, row);
                lua_pushinteger(L, index);
                callLua(L, 2, 0, what);
            };
        };
        if (lua->bind.isSet())
            provider.bind = rowCallback(&LuaProvider::bind, "an items view's bind");
        if (lua->unbind.isSet())
            provider.unbind = rowCallback(&LuaProvider::unbind, "an items view's unbind");
        if (lua->children.isSet()){
            provider.children = [lua](const std::string& parent) -> std::vector<std::string> {
                std::vector<std::string> keys;
                lua_State* L = LuaBinding::getLuaState();
                if (!pushFunction(L, lua->children))
                    return keys;
                if (parent.empty())
                    lua_pushnil(L);
                else
                    pushKey(L, parent);
                if (!callLua(L, 1, 1, "a tree view's children"))
                    return keys;
                if (lua_istable(L, -1)){
                    const lua_Integer length = (lua_Integer)lua_rawlen(L, -1);
                    for (lua_Integer i = 1; i <= length; i++){
                        lua_rawgeti(L, -1, i);
                        keys.push_back(keyText(L, -1));
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 1);
                return keys;
            };
        }
        if (lua->hasChildren.isSet()){
            provider.hasChildren = [lua](const std::string& key) -> bool {
                lua_State* L = LuaBinding::getLuaState();
                if (!pushFunction(L, lua->hasChildren))
                    return false;
                pushKey(L, key);
                if (!callLua(L, 1, 1, "a tree view's hasChildren"))
                    return false;
                const bool result = lua_toboolean(L, -1);
                lua_pop(L, 1);
                return result;
            };
        }
        return provider;
    }

    bool setProviderImpl(lua_State* L, UIItemsView* view, ErrorText error){
        if (!view->isValid()){
            std::snprintf(error, sizeof(ErrorText), "%s", kHtmlUIDeadElementMessage);
            return false;
        }
        lua_getfield(L, 2, "count");
        const int count = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        view->setProvider(makeProvider(L, 2, count));
        return true;
    }

    int itemsViewSetProvider(lua_State* L){
        UIItemsView* view = luabridge::detail::Userdata::get<UIItemsView>(L, 1, false);
        luaL_checktype(L, 2, LUA_TTABLE);
        ErrorText error;
        if (!view || !setProviderImpl(L, view, error))
            return luaL_error(L, "%s", view ? error : kHtmlUIDeadElementMessage);
        return 0;
    }

    std::vector<int> intList(const luabridge::LuaRef& table){
        std::vector<int> result;
        if (!table.isTable())
            return result;
        for (int i = 1; i <= table.length(); i++){
            luabridge::LuaRef value = table[i];
            if (value.isNumber())
                result.push_back(value.unsafe_cast<int>());
        }
        return result;
    }

    luabridge::LuaRef intTable(lua_State* L, const std::vector<int>& values){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < values.size(); i++)
            table[(int)i + 1] = values[i];
        return table;
    }

    luabridge::LuaRef stringTable(lua_State* L, const std::vector<std::string>& values){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < values.size(); i++)
            table[(int)i + 1] = values[i];
        return table;
    }

    int itemsViewGetVisibleRange(lua_State* L){
        UIItemsView* view = luabridge::detail::Userdata::get<UIItemsView>(L, 1, false);
        const std::pair<int, int> range = view ? view->getVisibleRange() : std::make_pair(0, 0);
        lua_pushinteger(L, range.first);
        lua_pushinteger(L, range.second);
        return 2;
    }

    luabridge::LuaRef stateTable(lua_State* L, const UIItemsViewState& state){
        luabridge::LuaRef table = luabridge::newTable(L);
        table["kind"] = state.kind;
        table["count"] = state.count;
        table["first"] = state.first;
        table["last"] = state.last;
        table["pool"] = state.pool;
        table["poolBound"] = state.poolBound;
        table["overscan"] = state.overscan;
        table["lanes"] = state.lanes;
        table["selection"] = state.selection;
        table["selected"] = intTable(L, state.selected);
        table["selectedKeys"] = stringTable(L, state.selectedKeys);
        table["active"] = state.active;
        table["scrollTop"] = state.scrollTop;
        table["totalHeight"] = state.totalHeight;
        table["source"] = state.source;
        table["layoutBoundary"] = state.layoutBoundary;
        table["rowAliases"] = state.rowAliases;
        table["memoryBytes"] = (double)state.memoryBytes;
        table["sortKey"] = state.sortKey;
        table["sortDirection"] = state.sortDirection;
        return table;
    }

    // ---- tooltips ----

    bool readTooltipOptions(lua_State* L, int index, UITooltipOptions& options, ErrorText error){
        if (lua_isnoneornil(L, index))
            return true;
        if (!lua_istable(L, index)){
            std::snprintf(error, sizeof(ErrorText), "setTooltip: options must be a table");
            return false;
        }
        lua_getfield(L, index, "placement");
        if (lua_type(L, -1) == LUA_TSTRING)
            options.placement = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "delay");
        if (lua_isnumber(L, -1))
            options.delay = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "arrow");
        options.arrow = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "interactive");
        options.interactive = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return true;
    }

    // a function tooltip: evaluated on each show, giving a text or an element (nil: none)
    void setFunctionTooltip(lua_State* L, Rml::Element* element, const UITooltipOptions& options){
        auto fn = std::make_shared<HtmlUILuaRef>(L, 2);
        htmlui::TooltipSpec spec;
        if (options.placement == "auto"){
            spec.automatic = true;
        }else if (!htmlui::parsePopupSide(options.placement.c_str(), spec.placement)){
            Log::warn("[HTML UI] setTooltip: unknown placement '%s' (top, bottom, left, right, auto); top is used", options.placement.c_str());
            spec.placement = htmlui::PopupSide::Top;
        }
        if (options.delay)
            spec.delay = (double)*options.delay;
        spec.arrow = options.arrow;
        spec.interactive = options.interactive;
        spec.content = [fn]() -> htmlui::TooltipContent {
            htmlui::TooltipContent content;
            content.none = true;
            lua_State* L = LuaBinding::getLuaState();
            if (!pushFunction(L, *fn))
                return content;
            if (!callLua(L, 0, 1, "a tooltip function"))
                return content;
            if (luabridge::detail::Userdata::isInstance<UIElement>(L, -1)){
                const UIElement* result = luabridge::detail::Userdata::get<UIElement>(L, -1, true);
                content.element = result ? UIElementAccess::get(*result) : nullptr;
                content.none = content.element == nullptr;
            }else if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER){
                content.text = lua_tostring(L, -1);
                content.none = content.text.empty();
            }
            lua_pop(L, 1);
            return content;
        };
        htmlui::TooltipManager::setTooltip(element, std::move(spec));
    }

    bool setTooltipImpl(lua_State* L, UIElement* self, ErrorText error){
        UITooltipOptions options;
        if (!readTooltipOptions(L, 3, options, error))
            return false;
        Rml::Element* element = UIElementAccess::get(*self);
        if (lua_isfunction(L, 2)){
            setFunctionTooltip(L, element, options);
        }else if (luabridge::detail::Userdata::isInstance<UIElement>(L, 2)){
            const UIElement* content = luabridge::detail::Userdata::get<UIElement>(L, 2, true);
            if (!content || !content->isValid()){
                std::snprintf(error, sizeof(ErrorText), "setTooltip: the content element is gone");
                return false;
            }
            self->setTooltip(*content, options);
        }else if (lua_type(L, 2) == LUA_TSTRING || lua_type(L, 2) == LUA_TNUMBER){
            self->setTooltip(std::string(lua_tostring(L, 2)), options);
        }else if (lua_isnil(L, 2)){
            self->clearTooltip();
        }else{
            std::snprintf(error, sizeof(ErrorText), "setTooltip: a text, an element or a function expected");
            return false;
        }
        return true;
    }

    int elementSetTooltip(lua_State* L){
        UIElement* self = luabridge::detail::Userdata::get<UIElement>(L, 1, false);
        if (!self || !self->isValid())
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        ErrorText error;
        if (!setTooltipImpl(L, self, error))
            return luaL_error(L, "%s", error);
        return 0;
    }

    void configureTooltipsImpl(lua_State* L, UIDocumentTooltip* self){
        UITooltipConfig config = self->document.getTooltipConfig();
        lua_getfield(L, 2, "enabled");
        if (lua_isboolean(L, -1))
            config.enabled = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "delay");
        if (lua_isnumber(L, -1))
            config.delay = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "hopWindow");
        if (lua_isnumber(L, -1))
            config.hopWindow = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        self->document.configureTooltips(config);
    }

    int tooltipConfigure(lua_State* L){
        UIDocumentTooltip* self = luabridge::detail::Userdata::get<UIDocumentTooltip>(L, 1, false);
        luaL_checktype(L, 2, LUA_TTABLE);
        if (self)
            configureTooltipsImpl(L, self);
        return 0;
    }

    // ---- UIDataTransfer: Lua values by reference ----

    void pushEntry(lua_State* L, const UIDataTransferState& state, const UIDataTransferState::Entry& entry){
        if (entry.type == "htmlui/row"){
            lua_createtable(L, 0, 3);
            UIElement view = UIElementAccess::wrap(state.rowView.get());
            HtmlUILua::pushElement(L, view);
            lua_setfield(L, -2, "view");
            lua_pushinteger(L, state.rowIndex + 1);
            lua_setfield(L, -2, "index");
            pushKey(L, state.rowKey);
            lua_setfield(L, -2, "key");
            return;
        }
        if (entry.lua.isSet() && entry.lua.push(L))
            return;
        lua_pushlstring(L, entry.text.data(), entry.text.size());
    }

    UIDataTransferState* transferOf(lua_State* L){
        UIDataTransfer* self = luabridge::detail::Userdata::get<UIDataTransfer>(L, 1, false);
        return self ? UIDataTransferAccess::state(*self) : nullptr;
    }

    // an entry for the value at `index` (strings are kept as text too: getData from C++)
    UIDataTransferState::Entry entryOf(lua_State* L, const char* type, int index){
        UIDataTransferState::Entry entry;
        entry.type = type;
        if (lua_type(L, index) == LUA_TSTRING){
            size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            entry.text.assign(text, length);
        }else{
            entry.lua = HtmlUILuaRef(L, index);
        }
        return entry;
    }

    void setDataImpl(lua_State* L, UIDataTransferState* state){
        const char* type = lua_tostring(L, 2);
        UIDataTransferState::Entry entry = entryOf(L, type, 3);
        if (UIDataTransferState::Entry* existing = state->find(type))
            *existing = std::move(entry);
        else
            state->entries.push_back(std::move(entry));
    }

    int transferSetData(lua_State* L){
        luaL_checkstring(L, 2);
        luaL_checkany(L, 3);
        UIDataTransferState* state = transferOf(L);
        if (!state)
            return luaL_error(L, "dataTransfer: the drag has ended");
        if (std::strcmp(lua_tostring(L, 2), "htmlui/row") == 0)
            return luaL_error(L, "dataTransfer: 'htmlui/row' is the view's own type");
        setDataImpl(L, state);
        return 0;
    }

    void getDataImpl(lua_State* L, UIDataTransferState* state){
        const UIDataTransferState::Entry* entry = state->find(lua_tostring(L, 2));
        if (entry)
            pushEntry(L, *state, *entry);
        else
            lua_pushnil(L);
    }

    int transferGetData(lua_State* L){
        luaL_checkstring(L, 2);
        UIDataTransferState* state = transferOf(L);
        if (!state){
            lua_pushnil(L);
            return 1;
        }
        getDataImpl(L, state);
        return 1;
    }

    int transferSetDragImage(lua_State* L){
        UIDataTransfer* self = luabridge::detail::Userdata::get<UIDataTransfer>(L, 1, false);
        if (!self || !self->isValid())
            return luaL_error(L, "dataTransfer: the drag has ended");
        const float x = (float)luaL_optnumber(L, 3, 0.0);
        const float y = (float)luaL_optnumber(L, 4, 0.0);
        if (lua_isnoneornil(L, 2)){
            self->clearDragImage();
            return 0;
        }
        if (!luabridge::detail::Userdata::isInstance<UIElement>(L, 2))
            return luaL_argerror(L, 2, "UIElement or nil expected");
        const UIElement* image = luabridge::detail::Userdata::get<UIElement>(L, 2, true);
        if (image)
            self->setDragImage(*image, x, y);
        return 0;
    }

    // ---- UIDrag ----

    std::vector<UIDataTransferState::Entry> dragEntries(lua_State* L, int options){
        std::vector<UIDataTransferState::Entry> entries;
        if (!lua_istable(L, options))
            return entries;
        lua_getfield(L, options, "data");
        const int data = lua_gettop(L);
        if (lua_istable(L, data)){
            lua_pushnil(L);
            while (lua_next(L, data) != 0){
                if (lua_type(L, -2) == LUA_TSTRING)
                    entries.push_back(entryOf(L, lua_tostring(L, -2), lua_gettop(L)));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        return entries;
    }

    bool dragBeginImpl(lua_State* L, const UIElement* source){
        std::string effect = "all";
        if (lua_istable(L, 2)){
            lua_getfield(L, 2, "effectAllowed");
            if (lua_type(L, -1) == LUA_TSTRING)
                effect = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        return htmlui::DragDropSession::begin(UIElementAccess::get(*source), dragEntries(L, 2), effect);
    }

    int dragBegin(lua_State* L){
        if (!luabridge::detail::Userdata::isInstance<UIElement>(L, 1))
            return luaL_argerror(L, 1, "UIElement expected");
        const UIElement* source = luabridge::detail::Userdata::get<UIElement>(L, 1, true);
        if (!source || !source->isValid())
            return luaL_error(L, "%s", kHtmlUIDeadElementMessage);
        lua_pushboolean(L, dragBeginImpl(L, source));
        return 1;
    }

    // ---- reloads ----

    void pushReport(lua_State* L, const UIReloadReport& report){
        lua_createtable(L, 0, 12);
        auto field = [L](const char* name, int value){ lua_pushinteger(L, value); lua_setfield(L, -2, name); };
        lua_pushstring(L, report.mode.c_str());
        lua_setfield(L, -2, "mode");
        lua_pushstring(L, report.file.c_str());
        lua_setfield(L, -2, "file");
        field("kept", report.kept);
        field("updated", report.updated);
        field("inserted", report.inserted);
        field("removed", report.removed);
        field("replaced", report.replaced);
        field("runtimeKept", report.runtimeKept);
        field("scriptsRerun", report.scriptsRerun);
        if (report.fallbackReason.empty())
            lua_pushnil(L);
        else
            lua_pushstring(L, report.fallbackReason.c_str());
        lua_setfield(L, -2, "fallbackReason");
        lua_pushnumber(L, report.ms);
        lua_setfield(L, -2, "ms");
        lua_createtable(L, (int)report.causes.size(), 0);
        for (size_t i = 0; i < report.causes.size(); i++){
            lua_pushstring(L, report.causes[i].c_str());
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_setfield(L, -2, "causes");
    }

    // doc.onReload = function(report) ... end (nil removes it)
    void setOnReload(UIDocument* self, lua_State* L){
        UIDocumentComponent& component = self->getComponent<UIDocumentComponent>();
        if (!lua_isfunction(L, -1)){
            component.onReload = nullptr;
            component.onReloadLua.reset();
            return;
        }
        auto fn = std::make_shared<HtmlUILuaRef>(L, lua_gettop(L));
        component.onReloadLua = fn;
        component.onReload = [fn](const UIReloadReport& report){
            lua_State* L = LuaBinding::getLuaState();
            if (!pushFunction(L, *fn))
                return;
            pushReport(L, report);
            callLua(L, 1, 0, "onReload");
        };
    }

    luabridge::LuaRef getOnReload(const UIDocument* self, lua_State* L){
        const UIDocumentComponent* component = self->getScene()->findComponent<UIDocumentComponent>(self->getEntity());
        if (!component || !component->onReloadLua || !component->onReloadLua->push(L))
            return luabridge::LuaRef(L);
        return luabridge::LuaRef::fromStack(L);
    }

    // doc:reload() / doc:reload{mode = "rebuild"|"patch"}
    int documentReload(lua_State* L){
        UIDocument* self = luabridge::detail::Userdata::get<UIDocument>(L, 1, false);
        if (!self)
            return 0;
        if (!lua_istable(L, 2)){
            self->reload();
            return 0;
        }
        lua_getfield(L, 2, "mode");
        const char* mode = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!mode)
            self->reload();
        else if (std::strcmp(mode, "rebuild") == 0)
            self->reload(UIReloadMode::REBUILD);
        else if (std::strcmp(mode, "patch") == 0)
            self->reload(UIReloadMode::PATCH);
        else
            return luaL_error(L, "UIDocument.reload: mode must be \"patch\" or \"rebuild\"");
        return 0;
    }

    UIElement elementArgument(lua_State* L, int index){
        if (!luabridge::detail::Userdata::isInstance<UIElement>(L, index))
            return UIElement();
        const UIElement* element = luabridge::detail::Userdata::get<UIElement>(L, index, true);
        return element ? *element : UIElement();
    }

}

void LuaBinding::registerHtmlUIWidgetClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginClass<UIItemsView>("UIItemsView")
        .addFunction("isValid", &UIItemsView::isValid)
        .addProperty("element", [](const UIItemsView* self) -> std::optional<UIElement> { return optionalElement(self->getElement()); })
        .addProperty("kind", &UIItemsView::getKind)
        .addProperty("count", &UIItemsView::getCount, &UIItemsView::setCount)
        .addFunction("setProvider", &itemsViewSetProvider)
        .addFunction("clearProvider", &UIItemsView::clearProvider)
        .addFunction("setCount", &UIItemsView::setCount)
        .addFunction("refresh", &UIItemsView::refresh)
        .addFunction("refreshItems", [](UIItemsView* self, const luabridge::LuaRef& indices) { self->refreshItems(intList(indices)); })
        .addFunction("scrollToIndex", [](UIItemsView* self, int index, std::optional<std::string> align) {
            self->scrollToIndex(index, align.value_or("nearest"));
        })
        .addFunction("getVisibleRange", &itemsViewGetVisibleRange)
        .addFunction("setSelection", [](UIItemsView* self, const luabridge::LuaRef& indices) { self->setSelection(intList(indices)); })
        .addFunction("getSelection", [](const UIItemsView* self, lua_State* L) { return intTable(L, self->getSelection()); })
        .addFunction("getSelectedKeys", [](const UIItemsView* self, lua_State* L) { return stringTable(L, self->getSelectedKeys()); })
        .addProperty("activeIndex", &UIItemsView::getActiveIndex, &UIItemsView::setActiveIndex)
        .addFunction("activate", &UIItemsView::activate)
        .addFunction("keyOf", &UIItemsView::keyOf)
        .addFunction("indexOfKey", [](const UIItemsView* self, lua_State* L) -> int {
            return self->indexOfKey(keyText(L, 2));
        })
        .addFunction("getRow", [](const UIItemsView* self, int index) -> std::optional<UIElement> { return optionalElement(self->getRow(index)); })
        .addFunction("indexOf", [](const UIItemsView* self, const UIElement& element) -> int { return self->indexOf(element); })
        .addFunction("expand", [](UIItemsView* self, lua_State* L) -> bool { return self->expand(keyText(L, 2)); })
        .addFunction("collapse", [](UIItemsView* self, lua_State* L) -> bool { return self->collapse(keyText(L, 2)); })
        .addFunction("toggle", [](UIItemsView* self, lua_State* L) -> bool { return self->toggle(keyText(L, 2)); })
        .addFunction("isExpanded", [](const UIItemsView* self, lua_State* L) -> bool { return self->isExpanded(keyText(L, 2)); })
        .addFunction("getNode", [](const UIItemsView* self, int index, lua_State* L) -> luabridge::LuaRef {
            const UITreeNode node = self->getNode(index);
            if (node.key.empty())
                return luabridge::LuaRef(L);
            luabridge::LuaRef table = luabridge::newTable(L);
            table["key"] = node.key;
            table["depth"] = node.depth;
            table["expanded"] = node.expanded;
            table["hasChildren"] = node.hasChildren;
            table["parent"] = node.parent;
            return table;
        })
        .addFunction("getColumns", [](const UIItemsView* self, lua_State* L) -> luabridge::LuaRef {
            luabridge::LuaRef table = luabridge::newTable(L);
            int i = 1;
            for (const UITableColumn& column : self->getColumns()){
                luabridge::LuaRef entry = luabridge::newTable(L);
                entry["key"] = column.key;
                entry["width"] = column.width;
                entry["minWidth"] = column.minWidth;
                entry["sortable"] = column.sortable;
                entry["resizable"] = column.resizable;
                table[i++] = entry;
            }
            return table;
        })
        .addFunction("setColumnWidth", &UIItemsView::setColumnWidth)
        .addFunction("sortBy", [](UIItemsView* self, const std::string& key, std::optional<std::string> direction) -> bool {
            return self->sortBy(key, direction.value_or(std::string()));
        })
        .addFunction("getState", [](const UIItemsView* self, lua_State* L) { return stateTable(L, self->getState()); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIElement>("UIElement")
        .addProperty("itemsView", [](const UIElement* self) -> std::optional<UIItemsView> {
            UIItemsView view = self->getItemsView();
            return view.isValid() ? std::optional<UIItemsView>(view) : std::nullopt;
        })
        .addFunction("setMarkdown", &UIElement::setMarkdown)
        .addFunction("setTooltip", &elementSetTooltip)
        .addFunction("clearTooltip", &UIElement::clearTooltip)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIDataTransfer>("UIDataTransfer")
        .addFunction("isValid", &UIDataTransfer::isValid)
        .addFunction("setData", &transferSetData)
        .addFunction("getData", &transferGetData)
        .addProperty("types", [](const UIDataTransfer* self, lua_State* L) { return stringTable(L, self->getTypes()); })
        .addFunction("clearData", [](UIDataTransfer* self, std::optional<std::string> type) { self->clearData(type.value_or(std::string())); })
        .addProperty("effectAllowed", &UIDataTransfer::getEffectAllowed, &UIDataTransfer::setEffectAllowed)
        .addProperty("dropEffect", &UIDataTransfer::getDropEffect, &UIDataTransfer::setDropEffect)
        .addFunction("setDragImage", &transferSetDragImage)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIEvent>("UIEvent")
        .addProperty("dataTransfer", [](const UIEvent* self) -> std::optional<UIDataTransfer> {
            UIDataTransfer transfer = self->getDataTransfer();
            return transfer.isValid() ? std::optional<UIDataTransfer>(transfer) : std::nullopt;
        })
        .addProperty("dropIndex", &UIEvent::getDropIndex)
        .addProperty("dropPosition", &UIEvent::getDropPosition)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("UIDrag")
        .addFunction("begin", &dragBegin)
        .addFunction("over", [](lua_State* L) -> bool { return UIDrag::over(elementArgument(L, 1)); })
        .addFunction("drop", [](lua_State* L) -> bool { return UIDrag::drop(elementArgument(L, 1)); })
        .addFunction("cancel", &UIDrag::cancel)
        .addFunction("isActive", &UIDrag::isActive)
        .addFunction("getDataTransfer", []() -> std::optional<UIDataTransfer> {
            UIDataTransfer transfer = UIDrag::getDataTransfer();
            return transfer.isValid() ? std::optional<UIDataTransfer>(transfer) : std::nullopt;
        })
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIDocumentTooltip>("UIDocumentTooltip")
        .addFunction("configure", &tooltipConfigure)
        .addProperty("enabled", [](const UIDocumentTooltip* self) -> bool { return self->document.getTooltipConfig().enabled; })
        .addProperty("delay", [](const UIDocumentTooltip* self) -> float { return self->document.getTooltipConfig().delay; })
        .addProperty("hopWindow", [](const UIDocumentTooltip* self) -> float { return self->document.getTooltipConfig().hopWindow; })
        .addProperty("visible", [](const UIDocumentTooltip* self) -> bool { return self->document.getTooltip().visible; })
        .addProperty("source", [](const UIDocumentTooltip* self) -> std::optional<UIElement> {
            return optionalElement(self->document.getTooltip().source);
        })
        .addProperty("element", [](const UIDocumentTooltip* self) -> std::optional<UIElement> {
            return optionalElement(self->document.getTooltip().tooltip);
        })
        .addProperty("text", [](const UIDocumentTooltip* self) -> std::string { return self->document.getTooltip().text; })
        .addFunction("show", [](UIDocumentTooltip* self, const UIElement& source) -> bool { return self->document.showTooltip(source); })
        .addFunction("hide", [](UIDocumentTooltip* self) { self->document.hideTooltip(); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UIDocument>("UIDocument")
        .addProperty("tooltip", [](const UIDocument* self) -> UIDocumentTooltip { return UIDocumentTooltip{*self}; })
        .addStaticFunction("markdownToHtml", &UIDocument::markdownToHtml)
        .addFunction("reload", &documentReload)
        .addProperty("onReload", &getOnReload, &setOnReload)
        .addProperty("lastReloadReport", [](const UIDocument* self, lua_State* L) -> luabridge::LuaRef {
            const UIReloadReport report = self->getLastReloadReport();
            if (report.mode.empty())
                return luabridge::LuaRef(L);
            pushReport(L, report);
            return luabridge::LuaRef::fromStack(L);
        })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_HTMLUI
