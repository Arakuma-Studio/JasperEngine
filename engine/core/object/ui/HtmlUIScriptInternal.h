// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUISCRIPTINTERNAL_H
#define HTMLUISCRIPTINTERNAL_H

// Engine-internal (includes RmlUi): the state behind the scripting API of HTML/CSS documents
// (UIElement, UIEvent, UIDataModel, UIDocument; phase 1 §3.9). Public headers never include it.
// These structs hold RmlUi types only by pointer or through RmlUi base classes, so they need no
// JASPERENGINE_INTERNAL (UIDocumentSystem holds HtmlUIScriptState; a hidden type there would
// make GCC warn wherever both are visible). Classes deriving from RmlUi types are internal.

#include "Export.h"
#include "UIElementInternal.h"
#include "UIDataModel.h"
#include "UIEvent.h"
#include "UIReloadReport.h"
#include "UIValue.h"
#include "ecs/Entity.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct lua_State lua_State;

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class Event;
}

namespace jasperengine::htmlui::compat {
    struct DocumentScript;
}

namespace jasperengine{

    class UIDocument;
    class UIDocumentSystem;
    class HtmlUIValueDefinition;
    struct HtmlUIDataModelRecord;

    // Raised by Lua (luaL_error) and logged by C++ (once per method) on a dead element handle
    extern const char* const kHtmlUIDeadElementMessage;

    // ---- events ----

    // What a UIEvent reads while its dispatch runs; `event` is reset when the listener returns,
    // after which every UIEvent getter returns a default
    struct HtmlUIEventState {
        Rml::Event* event = nullptr;
        std::string type;              // DOM name (the listener's, or the custom name)
        const UIValue* detail = nullptr;   // dispatchEvent detail (valid during the dispatch only)
    };

    // ---- data models ----

    // What UIDataModel handles and Lua proxies hold: a weak view of the model record, which
    // HtmlUIScriptState owns. record is null once the model was removed
    struct UIDataModelState {
        HtmlUIDataModelRecord* record = nullptr;
        std::string name;
        bool warned = false;   // C++ use after removal is logged once per model
    };

    // a Lua function kept in the registry (counted by HtmlUIScriptState::liveLuaRefs), with the
    // main thread of the state (LuaBinding::mainThread), whatever thread made it
    struct JASPERENGINE_API HtmlUILuaRef {
        lua_State* L = nullptr;
        int ref = -2;   // LUA_NOREF

        HtmlUILuaRef() = default;
        HtmlUILuaRef(lua_State* L, int index);   // references the value at index
        ~HtmlUILuaRef();
        HtmlUILuaRef(HtmlUILuaRef&& other) noexcept;
        HtmlUILuaRef& operator=(HtmlUILuaRef&& other) noexcept;
        HtmlUILuaRef(const HtmlUILuaRef&) = delete;
        HtmlUILuaRef& operator=(const HtmlUILuaRef&) = delete;

        bool isSet() const;
        // pushes the value on `current`, any thread of the creating state; false (nothing
        // pushed) if that state is gone
        bool push(lua_State* current) const;
        void reset();
    };

    struct HtmlUIDataModelRecord {
        struct EventCallback {
            std::function<void(UIEvent&, const std::vector<UIValue>&)> fn;
            HtmlUILuaRef lua;
        };
        struct ChangeCallback {
            uint64_t id = 0;
            std::string variable;   // top-level name
            std::function<void(const UIValue&)> fn;
            HtmlUILuaRef lua;
        };

        std::string name;
        Entity owner = NULL_ENTITY;
        std::shared_ptr<UIDataModelState> state;
        // top-level variables: stable addresses for RmlUi (BindCustomDataVariable)
        std::map<std::string, std::unique_ptr<UIValue>> storage;
        std::vector<std::string> order;          // insertion order of the top-level keys
        // HtmlUIValueDefinitions (held by their RmlUi base, so this record needs no hidden type)
        std::map<std::string, std::unique_ptr<Rml::VariableDefinition>> definitions;
        std::map<std::string, EventCallback> events;
        std::vector<ChangeCallback> changeCallbacks;
        std::vector<std::string> pendingChanges;   // written by RmlUi (data-value) since the last update
        Rml::Context* context = nullptr;           // the RmlUi model exists in it
        Rml::DataModelHandle handle;
        std::vector<std::string> boundEvents;      // event names bound in the RmlUi model

        ~HtmlUIDataModelRecord();
        UIValue* variable(const std::string& name);
        // a value of the tree by path ("items[2].name"); nullptr when missing
        UIValue* resolve(const std::string& path, std::string* topLevel = nullptr);
        void dirty(const std::string& topLevel);
        void markWritten(const std::string& topLevel);   // HtmlUIValueDefinition::Set
        // RmlUi side: creates the model in context and binds every variable and event
        bool attach(Rml::Context* context);
        void bindEventName(const std::string& eventName);
        void detach();   // Context::RemoveDataModel

        // data-event callbacks running (a model removed by one is destroyed after it returns)
        static int dispatchingEvents;
    };

    // ---- per scene ----

    // p1-lua's state of a UIDocumentSystem (UIDocumentSystemScript.cpp): data models and the
    // onLoad/onUnload queue. Element handles, the detached pool and listeners are process-wide
    // (HtmlUIElementRegistry, HtmlUIEventListener), like RmlUi itself.
    struct HtmlUIScriptState {
        explicit HtmlUIScriptState(UIDocumentSystem* system);
        ~HtmlUIScriptState();

        UIDocumentSystem* system;
        std::map<std::string, std::unique_ptr<HtmlUIDataModelRecord>> models;
        struct Notification { Entity entity; bool loaded; };
        std::vector<Notification> notifications;   // onUnload/onLoad, fired after the context update
        // S17: the patch reloads whose onReload fires after the context update
        struct Reload { Entity entity; UIReloadReport report; };
        std::vector<Reload> reloads;
        // removed while one of their data-event callbacks ran: destroyed after the update
        std::vector<std::unique_ptr<HtmlUIDataModelRecord>> removedDuringEvents;
        // empty RmlUi models (missing variables allowed) for the data-model names a document
        // uses before a script created them (edit mode, a scene's load before init): the document
        // loads without errors; createDataModel replaces the placeholder and reloads it
        std::vector<std::string> placeholders;
        static uint64_t nextChangeId;   // UIDataModel::onChange ids, process-wide

        // a new model (top-level keys of initial, an OBJECT, fixed from now on); nullptr and
        // error when the name is taken or initial is not an object
        HtmlUIDataModelRecord* createModel(Entity owner, const std::string& name, const UIValue& initial, std::string& error);
        HtmlUIDataModelRecord* findModel(const std::string& name) const;
        void removeModel(const std::string& name);
        void removeModelsOf(Entity owner);
        // the document of owner uses data-model="name": reloaded so RmlUi binds it
        void reloadUsersOf(Entity owner, const std::string& name);

        // listeners of the old document's id-ed elements (and the document itself) move to their
        // counterparts; the others are dropped with one warning (§3.8.6 step 4)
        void onDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew);

        // after the context update: data-value writes (onChange), then onUnload/onLoad
        void afterUpdate();
        // onUnload/onLoad queued so far (also right after a synchronous load: the scene's load,
        // UIDocument::load, so an init that runs after it is not called back for that load)
        void fireNotifications();

        static UIDataModel makeHandle(HtmlUIDataModelRecord* record);
        // Lua references held by listeners, data-model events and onChange callbacks (tests)
        static size_t liveLuaRefs();
    };

    // ---- element handles and the detached pool (process-wide) ----

    struct HtmlUIElementRegistry {
        // the token shared by every handle of element: a detached tree lives while a handle
        // of one of its elements does
        static std::shared_ptr<int> token(Rml::Element* element);
        // takes a detached tree (no parent) into the pool of document's context
        static Rml::Element* adopt(Rml::ElementPtr root, Rml::ElementDocument* document);
        // takes root out of the pool (it is being inserted); nullptr if it is not pooled
        static Rml::ElementPtr release(Rml::Element* root);
        // element is in a pooled detached tree
        static bool isDetachedAlive(Rml::Element* element);
        // the document a pooled tree was detached from (nullptr if not pooled)
        static Rml::ElementDocument* poolDocument(Rml::Element* element);
        // destroys the pooled trees no handle reaches (end of UIDocumentSystem::update)
        static void collect();
        static void freeDocument(Rml::ElementDocument* document);
        static void freeContext(Rml::Context* context);
        static size_t poolSize();
    };

    // ---- friend access to the public classes ----

    struct HtmlUIScriptAccess {
        static UIEvent makeEvent(const std::shared_ptr<HtmlUIEventState>& state);
        static HtmlUIEventState* eventState(const UIEvent& event);
        static UIDataModel makeModel(const std::shared_ptr<UIDataModelState>& state);
        static std::shared_ptr<UIDataModelState> modelState(const UIDataModel& model);
        // ARRAY elements or OBJECT values, in order
        static std::vector<UIValue>& items(UIValue& value);
        static const std::vector<UIValue>& items(const UIValue& value);
        static const std::vector<std::string>& objectKeys(const UIValue& value);
    };

    // ---- Lua (HtmlUIClassesLua.cpp) ----

    struct HtmlUILua {
        // UIValue <-> Lua: tables with keys 1..n are arrays, other tables objects (an empty
        // table is an empty array); functions and cycles are errors
        static void pushValue(lua_State* L, const UIValue& value);
        static bool toValue(lua_State* L, int index, UIValue& out, std::string& error);
        static void pushEvent(lua_State* L, const UIEvent& event);
        static void pushElement(lua_State* L, const UIElement& element);   // nil for an invalid handle
        static void pushModel(lua_State* L, const std::shared_ptr<UIDataModelState>& state);
        // calls the function below its nargs arguments with a traceback; logs and returns
        // false on error (what names the callback in the log)
        static bool call(lua_State* L, int nargs, const std::string& what);
        // phase 2 (p2-html, §3.12.4): a <script type="text/lua"> of a document, in a fresh
        // environment (__index = _G) with `document` and `anime` bound; the chunk is named after
        // its file and starts at its line, so errors read "ui/menu.html:45: ...". False with the
        // Lua error (and traceback) in `error` when it fails; nothing is logged
        static bool runDocumentScript(UIDocument& document, const htmlui::compat::DocumentScript& script, std::string* error = nullptr);
    };

}

#endif //HTMLUISCRIPTINTERNAL_H
