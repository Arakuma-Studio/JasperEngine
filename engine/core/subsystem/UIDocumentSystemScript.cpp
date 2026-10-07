// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem scripting (phase 1, p1-lua, spec §3.9): HtmlUIScriptState (data models,
// onLoad/onUnload), the process-wide detached pool of element handles, listener re-attachment
// after a reload, and the Lua access of phase 4.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Engine.h"
#include "Log.h"
#include "script/LuaBinding.h"
#include "htmlui/HtmlUIContext.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>

using namespace jasperengine;

uint64_t HtmlUIScriptState::nextChangeId = 1;

// ---- HtmlUIElementRegistry ----

namespace {

    struct TokenEntry {
        Rml::ObserverPtr<Rml::Element> element;
        std::weak_ptr<int> token;
    };

    struct PoolEntry {
        Rml::ElementPtr root;
        Rml::ElementDocument* document = nullptr;
        Rml::Context* context = nullptr;
    };

    std::unordered_map<Rml::Element*, TokenEntry>& tokens(){
        static std::unordered_map<Rml::Element*, TokenEntry> map;
        return map;
    }

    std::unordered_map<Rml::Element*, PoolEntry>& pool(){
        static std::unordered_map<Rml::Element*, PoolEntry> map;
        return map;
    }

    Rml::Element* rootOf(Rml::Element* element){
        while (element->GetParentNode())
            element = element->GetParentNode();
        return element;
    }

    bool hasHandle(Rml::Element* element){
        auto it = tokens().find(element);
        if (it != tokens().end() && it->second.element.get() == element && !it->second.token.expired())
            return true;
        for (int i = 0; i < element->GetNumChildren(true); i++){
            if (hasHandle(element->GetChild(i)))
                return true;
        }
        return false;
    }

    // erases the pool entries selected by drop; the trees are destroyed after the map is
    // consistent again (destroying elements runs listener OnDetach)
    template<typename Predicate>
    void erasePoolEntries(Predicate drop){
        std::vector<Rml::ElementPtr> doomed;
        for (auto it = pool().begin(); it != pool().end();){
            if (drop(it->second)){
                doomed.push_back(std::move(it->second.root));
                it = pool().erase(it);
            }else{
                ++it;
            }
        }
        doomed.clear();
    }

}

std::shared_ptr<int> HtmlUIElementRegistry::token(Rml::Element* element){
    if (!element)
        return nullptr;

    TokenEntry& entry = tokens()[element];
    // a destroyed element's address can be reused by a new one: its token is not shared
    if (entry.element.get() == element){
        if (std::shared_ptr<int> existing = entry.token.lock())
            return existing;
    }
    std::shared_ptr<int> created = std::make_shared<int>(0);
    entry.element = element->GetObserverPtr();
    entry.token = created;
    return created;
}

Rml::Element* HtmlUIElementRegistry::adopt(Rml::ElementPtr root, Rml::ElementDocument* document){
    Rml::Element* element = root.get();
    if (!element)
        return nullptr;
    PoolEntry& entry = pool()[element];
    entry.root = std::move(root);
    entry.document = document;
    entry.context = document ? document->GetContext() : nullptr;
    return element;
}

Rml::ElementPtr HtmlUIElementRegistry::release(Rml::Element* root){
    auto it = pool().find(root);
    if (it == pool().end())
        return nullptr;
    Rml::ElementPtr element = std::move(it->second.root);
    pool().erase(it);
    return element;
}

bool HtmlUIElementRegistry::isDetachedAlive(Rml::Element* element){
    return element && pool().count(rootOf(element)) > 0;
}

Rml::ElementDocument* HtmlUIElementRegistry::poolDocument(Rml::Element* element){
    if (!element)
        return nullptr;
    auto it = pool().find(rootOf(element));
    return (it != pool().end()) ? it->second.document : nullptr;
}

void HtmlUIElementRegistry::collect(){
    erasePoolEntries([](PoolEntry& entry){ return !hasHandle(entry.root.get()); });

    for (auto it = tokens().begin(); it != tokens().end();){
        if (it->second.token.expired() || !it->second.element.get()){
            it = tokens().erase(it);
        }else{
            ++it;
        }
    }
}

void HtmlUIElementRegistry::freeDocument(Rml::ElementDocument* document){
    erasePoolEntries([document](PoolEntry& entry){ return entry.document == document; });
}

void HtmlUIElementRegistry::freeContext(Rml::Context* context){
    // entries without a context belong to nobody else either (RmlUi must still be initialised
    // when they are destroyed)
    erasePoolEntries([context](PoolEntry& entry){ return entry.context == context || !entry.context; });
}

size_t HtmlUIElementRegistry::poolSize(){
    return pool().size();
}

// ---- HtmlUIScriptState ----

HtmlUIScriptState::HtmlUIScriptState(UIDocumentSystem* system): system(system){
}

HtmlUIScriptState::~HtmlUIScriptState(){
    // the records release their Lua references (when the state that made them is still open)
    models.clear();
}

HtmlUIDataModelRecord* HtmlUIScriptState::createModel(Entity owner, const std::string& name, const UIValue& initial, std::string& error){
    if (name.empty()){
        error = "a data model needs a name";
        return nullptr;
    }
    if (models.count(name)){
        error = "a data model named '" + name + "' already exists in this scene";
        return nullptr;
    }
    if (initial.getType() != UIValue::Type::OBJECT && !initial.isNil()){
        error = "the initial value of data model '" + name + "' must be an object (a table with named keys)";
        return nullptr;
    }

    std::unique_ptr<HtmlUIDataModelRecord> record = std::make_unique<HtmlUIDataModelRecord>();
    record->name = name;
    record->owner = owner;
    record->state = std::make_shared<UIDataModelState>();
    record->state->record = record.get();
    record->state->name = name;

    const std::vector<std::string> keys = initial.keys();
    const std::vector<UIValue>& values = HtmlUIScriptAccess::items(initial);
    for (size_t i = 0; i < keys.size(); i++){
        record->order.push_back(keys[i]);
        record->storage[keys[i]] = std::make_unique<UIValue>(values[i]);
    }

    HtmlUIDataModelRecord* created = record.get();
    models[name] = std::move(record);

    // a model made before the first load is bound when the context is created
    if (HtmlUIContext* context = system->getContext()){
        created->attach(context->getRmlContext());
    }
    return created;
}

HtmlUIDataModelRecord* HtmlUIScriptState::findModel(const std::string& name) const{
    auto it = models.find(name);
    return (it != models.end()) ? it->second.get() : nullptr;
}

void HtmlUIScriptState::removeModel(const std::string& name){
    auto it = models.find(name);
    if (it == models.end())
        return;
    std::unique_ptr<HtmlUIDataModelRecord> record = std::move(it->second);
    models.erase(it);
    // handles see it removed at once
    record->state->record = nullptr;
    // a data-event callback is running: RmlUi may be calling a function of this model, so
    // the RmlUi side goes after the update
    if (HtmlUIDataModelRecord::dispatchingEvents > 0){
        removedDuringEvents.push_back(std::move(record));
    }
}

void HtmlUIScriptState::removeModelsOf(Entity owner){
    std::vector<std::string> names;
    for (const auto& pair : models){
        if (pair.second->owner == owner)
            names.push_back(pair.first);
    }
    for (const std::string& name : names){
        removeModel(name);
    }
}

void HtmlUIScriptState::reloadUsersOf(Entity owner, const std::string& name){
    Rml::ElementDocument* document = system->getDocument(owner);
    if (!document)
        return;

    // RmlUi binds data-model while parsing: a document that uses the model reloads
    bool uses = false;
    std::vector<Rml::Element*> stack = {document};
    while (!stack.empty() && !uses){
        Rml::Element* element = stack.back();
        stack.pop_back();
        if (element->GetAttribute<Rml::String>("data-model", "") == name){
            uses = true;
            break;
        }
        for (int i = 0; i < element->GetNumChildren(); i++){
            stack.push_back(element->GetChild(i));
        }
    }

    if (uses){
        system->reloadDocument(owner, true);
    }
}

void HtmlUIScriptState::onDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew){
    // the old document is the key whose counterpart is a document
    Rml::ElementDocument* oldDocument = nullptr;
    for (const auto& pair : oldToNew){
        if (Rml::ElementDocument* document = rmlui_dynamic_cast<Rml::ElementDocument*>(pair.first)){
            oldDocument = document;
            break;
        }
    }
    if (!oldDocument)
        return;

    size_t dropped = 0;
    for (HtmlUIEventListener* listener : HtmlUIEventListener::listenersIn(oldDocument)){
        auto it = oldToNew.find(listener->getElement());
        if (it != oldToNew.end() && it->second){
            listener->moveTo(it->second);
        }else{
            listener->remove();
            dropped++;
        }
    }

    // detached elements made from the old document go with it
    HtmlUIElementRegistry::freeDocument(oldDocument);

    if (dropped > 0){
        const UIDocumentComponent* component = system->scene->findComponent<UIDocumentComponent>(entity);
        Log::warn("[HTML UI] %s: %zu event listener(s) on elements without an id were dropped by the reload; "
                  "give the element an id or rebind in onLoad",
                  component ? component->filename.c_str() : "document", dropped);
    }
}

void HtmlUIScriptState::afterUpdate(){
    // models removed by their own data-event callbacks
    if (HtmlUIDataModelRecord::dispatchingEvents == 0){
        removedDuringEvents.clear();
    }

    // data-value writes, after RmlUi's update (callbacks may remove models or callbacks)
    std::vector<std::string> names;
    for (const auto& pair : models){
        if (!pair.second->pendingChanges.empty())
            names.push_back(pair.first);
    }
    for (const std::string& name : names){
        HtmlUIDataModelRecord* record = findModel(name);
        if (!record)
            continue;
        std::vector<std::string> changed = std::move(record->pendingChanges);
        record->pendingChanges.clear();
        std::shared_ptr<UIDataModelState> state = record->state;
        for (const std::string& variable : changed){
            std::vector<uint64_t> ids;
            for (const auto& callback : record->changeCallbacks){
                if (callback.variable == variable)
                    ids.push_back(callback.id);
            }
            for (uint64_t id : ids){
                if (!state->record)
                    break;
                auto& callbacks = state->record->changeCallbacks;
                auto it = std::find_if(callbacks.begin(), callbacks.end(), [id](const HtmlUIDataModelRecord::ChangeCallback& c){ return c.id == id; });
                if (it == callbacks.end())
                    continue;
                const UIValue* value = state->record->variable(variable);
                const UIValue current = value ? *value : UIValue();
                if (it->fn){
                    std::function<void(const UIValue&)> fn = it->fn;
                    fn(current);
                }else if (it->lua.isSet()){
                    lua_State* L = LuaBinding::getLuaState();
                    if (L && it->lua.push(L)){
                        HtmlUILua::pushValue(L, current);
                        HtmlUILua::call(L, 1, "data model '" + name + "' onChange('" + variable + "')");
                    }
                }
            }
            if (!state->record)
                break;
        }
    }

    // onUnload/onLoad, in the order they happened
    std::vector<Notification> pending = std::move(notifications);
    notifications.clear();
    for (const Notification& notification : pending){
        UIDocumentComponent* component = system->scene->findComponent<UIDocumentComponent>(notification.entity);
        if (!component)
            continue;
        if (notification.loaded){
            component->onLoad.call();
        }else{
            component->onUnload.call();
        }
    }
}

UIDataModel HtmlUIScriptState::makeHandle(HtmlUIDataModelRecord* record){
    return record ? HtmlUIScriptAccess::makeModel(record->state) : UIDataModel();
}

// ---- UIDocumentSystem (p1-lua) ----

HtmlUIScriptState& UIDocumentSystem::getScriptState(){
    if (!scriptState){
        scriptState = std::make_shared<HtmlUIScriptState>(this);
    }
    return *scriptState;
}

void UIDocumentSystem::scriptLoadDocuments(){
    // phase 0 loaded lazily in the first update; scripts' init runs before it and queries
    // elements, so every enabled document loads with the scene
    if (!Engine::isViewLoaded())
        return;

    std::vector<Entity> entities;
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        const UIDocumentComponent& document = components->getComponentFromIndex(i);
        if (document.enabled && !document.filename.empty() && (document.needReload || !document.loaded))
            entities.push_back(components->getEntity(i));
    }
    for (Entity entity : entities){
        loadDocumentNow(entity);
    }
}

void UIDocumentSystem::scriptContextCreated(){
    Rml::Context* rmlContext = getContext() ? getContext()->getRmlContext() : nullptr;
    if (!rmlContext)
        return;

    HtmlUIEventNames::installChangeSynthesizer(rmlContext);

    if (scriptState){
        for (auto& pair : scriptState->models){
            pair.second->attach(rmlContext);
        }
    }
}

void UIDocumentSystem::scriptContextDestroying(){
    for (const auto& pair : documents){
        HtmlUIElementRegistry::freeDocument(pair.second);
    }
    if (HtmlUIContext* htmlContext = getContext()){
        HtmlUIElementRegistry::freeContext(htmlContext->getRmlContext());
    }

    // the models stay (with their values and callbacks) and are bound again in the next context
    if (scriptState){
        for (auto& pair : scriptState->models){
            pair.second->detach();
        }
        // their RmlUi side goes now, while the context still exists
        scriptState->removedDuringEvents.clear();
    }
}

void UIDocumentSystem::scriptDocumentLoaded(Entity entity){
    getScriptState().notifications.push_back({entity, true});
}

void UIDocumentSystem::scriptDocumentUnloading(Entity entity, Rml::ElementDocument* document){
    HtmlUIElementRegistry::freeDocument(document);
    getScriptState().notifications.push_back({entity, false});
}

void UIDocumentSystem::scriptComponentRemoved(Entity entity){
    if (scriptState){
        scriptState->removeModelsOf(entity);
    }
}

void UIDocumentSystem::scriptUpdate(){
    HtmlUIElementRegistry::collect();
    if (scriptState){
        scriptState->afterUpdate();
    }
}

void UIDocumentSystem::scriptDocumentReloaded(Entity entity, const std::unordered_map<Rml::Element*, Rml::Element*>& oldToNew){
    getScriptState().onDocumentReloaded(entity, oldToNew);
}

std::vector<std::string> UIDocumentSystem::getDataModelNames(Entity entity) const{
    std::vector<std::string> names;
    if (scriptState){
        for (const auto& pair : scriptState->models){
            if (pair.second->owner == entity)
                names.push_back(pair.first);
        }
    }
    return names;
}

bool UIDocumentSystem::pushDataModel(lua_State* L, Entity entity, const std::string& name){
    HtmlUIDataModelRecord* record = scriptState ? scriptState->findModel(name) : nullptr;
    if (!L || !record || record->owner != entity)
        return false;
    HtmlUILua::pushModel(L, record->state);
    return true;
}
