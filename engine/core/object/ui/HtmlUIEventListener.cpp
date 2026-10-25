// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIEventListener.h"

#include "Log.h"
#include "script/LuaBinding.h"
#include "htmlui/widgets/DragDropSession.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

const char* const HtmlUIEventNames::changeEvent = "htmlui-change";
const char* const HtmlUIEventNames::detailParameter = "htmlui_detail";
const char* const HtmlUIEventNames::preventedParameter = "htmlui_prevented";

namespace {

    // DOM names with the same RmlUi name
    const char* const kSameNames[] = {
        "click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout", "keydown", "keyup",
        "focus", "blur", "scroll", "resize", "load", "unload", "submit", "animationend", "transitionend",
        "dragstart", "drag", "dragend", "dragmove", "dragdrop", "dragout", "textinput",
        // the phase-3 fork's CSS animation and transition events
        "animationstart", "animationiteration", "animationcancel", "transitionrun", "transitionstart", "transitioncancel",
    };

    // DOM events documents never receive: listening to them would never fire
    bool isUnsupported(const std::string& type){
        static const std::unordered_set<std::string> names = {
            "mouseenter", "mouseleave", "contextmenu", "beforeinput",
            "compositionstart", "compositionupdate", "compositionend",
        };
        if (names.count(type))
            return true;
        return type.rfind("pointer", 0) == 0 || type.rfind("touch", 0) == 0;
    }

    // the per-element registry (cleaned in OnDetach) and the id index
    std::unordered_map<Rml::Element*, std::vector<HtmlUIEventListener*>>& byElement(){
        static std::unordered_map<Rml::Element*, std::vector<HtmlUIEventListener*>> registry;
        return registry;
    }

    std::unordered_map<uint64_t, HtmlUIEventListener*>& byId(){
        static std::unordered_map<uint64_t, HtmlUIEventListener*> registry;
        return registry;
    }

    uint64_t nextListenerId = 1;

    // the last key down the context listeners saw, when it repeated a held key
    const Rml::Event* repeatedKeyDown = nullptr;

    bool isTextControl(Rml::Element* element){
        const std::string& tag = element->GetTagName();
        if (tag == "textarea")
            return true;
        if (tag != "input")
            return false;
        const std::string type = element->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password" || type == "search" || type == "email" || type == "url" ||
               type == "tel" || type == "number";
    }

    std::string controlValue(Rml::Element* element){
        if (Rml::ElementFormControl* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element))
            return control->GetValue();
        return element->GetAttribute<Rml::String>("value", "");
    }

    // DOM "change": RmlUi's own change fires on every edit of a text field (that is DOM
    // "input"), so a text field changes on blur when its value differs from the one it had at
    // focus; checkboxes, radios, selects and ranges change with RmlUi. A capture listener of the
    // context root sees focus/blur/change of every document element.
    class HtmlUIChangeSynthesizer final : public Rml::EventListener {
    public:
        explicit HtmlUIChangeSynthesizer(Rml::Context* context): context(context){
        }

        static std::unordered_map<Rml::Context*, HtmlUIChangeSynthesizer*>& installed(){
            static std::unordered_map<Rml::Context*, HtmlUIChangeSynthesizer*> map;
            return map;
        }

        void ProcessEvent(Rml::Event& event) override{
            Rml::Element* target = event.GetTargetElement();
            if (!target)
                return;

            const std::string& type = event.GetType();
            // a gamepad's key events say themselves whether they repeat (p4-input)
            const bool gamepad = event.GetParameters().count("gamepad") > 0;
            if (type == "keydown" && !gamepad){
                const int key = event.GetParameter<int>("key_identifier", 0);
                repeatedKeyDown = heldKeys.count(key) ? &event : nullptr;
                heldKeys.insert(key);
            }else if (type == "keyup" && !gamepad){
                heldKeys.erase(event.GetParameter<int>("key_identifier", 0));
            }else if (type == "focus"){
                if (isTextControl(target)){
                    focusValues[target] = FocusValue{target->GetObserverPtr(), controlValue(target)};
                }
            }else if (type == "blur"){
                auto it = focusValues.find(target);
                if (it != focusValues.end()){
                    const bool changed = it->second.element.get() == target && controlValue(target) != it->second.value;
                    focusValues.erase(it);
                    if (changed)
                        dispatchChange(target);
                }
            }else if (type == "change"){
                if (!isTextControl(target) && rmlui_dynamic_cast<Rml::ElementFormControl*>(target))
                    dispatchChange(target);
            }
        }

        void OnAttach(Rml::Element*) override{
            attachments++;
        }

        void OnDetach(Rml::Element*) override{
            if (--attachments == 0){
                auto& map = installed();
                auto it = map.find(context);
                if (it != map.end() && it->second == this)
                    map.erase(it);
                delete this;
            }
        }

    private:
        struct FocusValue {
            Rml::ObserverPtr<Rml::Element> element;
            std::string value;
        };

        static void dispatchChange(Rml::Element* target){
            Rml::Dictionary parameters;
            parameters["value"] = controlValue(target);
            target->DispatchEvent(HtmlUIEventNames::changeEvent, parameters, true, true);
        }

        Rml::Context* context;
        int attachments = 0;
        std::unordered_map<Rml::Element*, FocusValue> focusValues;
        std::unordered_set<int> heldKeys;   // RmlUi key identifiers down in this context
    };

}

const std::string& HtmlUIEventNames::supportedList(){
    static const std::string list = [](){
        std::string text;
        for (const char* name : kSameNames){
            text += name;
            text += ", ";
        }
        text += "dragenter, dragover, dragleave, drop, wheel, input, change and custom events";
        return text;
    }();
    return list;
}

bool HtmlUIEventNames::toRml(const std::string& domType, std::string& rmlType, const char* what){
    if (domType.empty() || isUnsupported(domType)){
        Log::warn("[HTML UI] %s: documents do not receive '%s' events; supported: %s",
                  what, domType.c_str(), supportedList().c_str());
        return false;
    }

    if (const char* drag = htmlui::DragDropSession::rmlEventOf(domType); drag[0]){
        // HTML5's dragenter, dragover (repeating), dragleave and drop (S17 R18)
        rmlType = drag;
    }else if (domType == "wheel"){
        rmlType = "mousescroll";
    }else if (domType == "input"){
        // RmlUi fires change on every edit of a text field and on toggles
        rmlType = "change";
    }else if (domType == "change"){
        rmlType = changeEvent;
    }else{
        // the same name, or a custom event
        rmlType = domType;
    }
    return true;
}

void HtmlUIEventNames::installChangeSynthesizer(Rml::Context* context){
    if (!context || !context->GetRootElement())
        return;

    auto& map = HtmlUIChangeSynthesizer::installed();
    if (map.count(context))
        return;

    HtmlUIChangeSynthesizer* synthesizer = new HtmlUIChangeSynthesizer(context);
    map[context] = synthesizer;
    Rml::Element* root = context->GetRootElement();
    root->AddEventListener("focus", synthesizer, true);
    root->AddEventListener("blur", synthesizer, true);
    root->AddEventListener("change", synthesizer, true);
    root->AddEventListener("keydown", synthesizer, true);
    root->AddEventListener("keyup", synthesizer, true);
}

bool HtmlUIEventNames::isRepeat(const Rml::Event* event){
    if (event && event->GetParameters().count("gamepad"))
        return event->GetType() == "keydown" && event->GetParameter<int>("repeat", 0) != 0;
    return event && event == repeatedKeyDown && event->GetType() == "keydown";
}

// the document whose <script type="text/lua"> is running (ScriptScope); scripts run on the main thread
static const Rml::Element* runningScriptDocument = nullptr;

HtmlUIEventListener::ScriptScope::ScriptScope(const Rml::Element* document): previous(runningScriptDocument){
    runningScriptDocument = document;
}

HtmlUIEventListener::ScriptScope::~ScriptScope(){
    runningScriptDocument = previous;
}

HtmlUIEventListener::HtmlUIEventListener(uint64_t id, std::string domType, std::string rmlType, bool capture, bool once, Callback callback):
    id(id), domType(std::move(domType)), rmlType(std::move(rmlType)), capture(capture), once(once), callback(std::move(callback)){
    byId()[id] = this;
}

HtmlUIEventListener::~HtmlUIEventListener(){
    auto it = byId().find(id);
    if (it != byId().end() && it->second == this){
        byId().erase(it);
    }
}

HtmlUIEventListener* HtmlUIEventListener::attach(Rml::Element* element, const std::string& domType, const std::string& rmlType,
                                                 bool capture, bool once, Callback callback){
    if (!element)
        return nullptr;

    HtmlUIEventListener* listener = new HtmlUIEventListener(nextListenerId++, domType, rmlType, capture, once, std::move(callback));
    listener->scriptDocument = runningScriptDocument;
    element->AddEventListener(rmlType, listener, capture);

    if (domType == "change"){
        HtmlUIEventNames::installChangeSynthesizer(element->GetContext());
    }
    return listener;
}

bool HtmlUIEventListener::detach(uint64_t id){
    HtmlUIEventListener* listener = find(id);
    if (!listener)
        return false;
    listener->remove();
    return true;
}

HtmlUIEventListener* HtmlUIEventListener::find(uint64_t id){
    auto it = byId().find(id);
    return (it != byId().end() && !it->second->detached) ? it->second : nullptr;
}

std::vector<HtmlUIEventListener*> HtmlUIEventListener::listenersOf(Rml::Element* element){
    std::vector<HtmlUIEventListener*> result;
    auto it = byElement().find(element);
    if (it != byElement().end()){
        for (HtmlUIEventListener* listener : it->second){
            if (!listener->detached)
                result.push_back(listener);
        }
    }
    return result;
}

std::vector<HtmlUIEventListener*> HtmlUIEventListener::listenersIn(Rml::Element* root){
    std::vector<HtmlUIEventListener*> result;
    if (!root)
        return result;
    for (const auto& pair : byElement()){
        for (Rml::Element* walk = pair.first; walk; walk = walk->GetParentNode()){
            if (walk == root){
                for (HtmlUIEventListener* listener : pair.second){
                    if (!listener->detached)
                        result.push_back(listener);
                }
                break;
            }
        }
    }
    // a stable order: by id (attach order)
    std::sort(result.begin(), result.end(), [](HtmlUIEventListener* a, HtmlUIEventListener* b){ return a->id < b->id; });
    return result;
}

size_t HtmlUIEventListener::liveCount(){
    return byId().size();
}

HtmlUIEventListener* HtmlUIEventListener::moveTo(Rml::Element* target){
    Rml::Element* current = element.get();
    if (!target || detached)
        return nullptr;

    HtmlUIEventListener* moved = new HtmlUIEventListener(id, domType, rmlType, capture, once, std::move(callback));
    moved->scriptDocument = scriptDocument;
    // the id now names the moved listener; this one is deleted without its callback
    byId()[id] = moved;
    target->AddEventListener(rmlType, moved, capture);

    if (current){
        current->RemoveEventListener(rmlType, this, capture);
    }
    return moved;
}

void HtmlUIEventListener::remove(){
    if (detached)
        return;
    if (Rml::Element* current = element.get()){
        current->RemoveEventListener(rmlType, this, capture);
    }
}

void HtmlUIEventListener::OnAttach(Rml::Element* attachedElement){
    element = attachedElement->GetObserverPtr();
    registeredOn = attachedElement;
    byElement()[attachedElement].push_back(this);
}

void HtmlUIEventListener::OnDetach(Rml::Element*){
    if (registeredOn){
        auto it = byElement().find(registeredOn);
        if (it != byElement().end()){
            auto& list = it->second;
            list.erase(std::remove(list.begin(), list.end(), this), list.end());
            if (list.empty())
                byElement().erase(it);
        }
        registeredOn = nullptr;
    }

    detached = true;
    if (dispatching == 0){
        delete this;
    }
}

void HtmlUIEventListener::ProcessEvent(Rml::Event& event){
    if (detached)
        return;

    dispatching++;

    std::shared_ptr<HtmlUIEventState> state = std::make_shared<HtmlUIEventState>();
    state->event = &event;
    state->type = domType;
    state->detail = static_cast<const UIValue*>(event.GetParameter<void*>(HtmlUIEventNames::detailParameter, nullptr));
    UIEvent uiEvent = HtmlUIScriptAccess::makeEvent(state);

    // { once: true } removes it before it runs, as in the DOM
    if (once){
        remove();
    }

    invoke(uiEvent);

    state->event = nullptr;
    state->detail = nullptr;

    dispatching--;
    if (detached && dispatching == 0){
        delete this;
    }
}

void HtmlUIEventListener::invoke(UIEvent& event){
    if (callback.fn){
        callback.fn(event);
        return;
    }

    if (!callback.lua.isSet())
        return;

    lua_State* L = LuaBinding::getLuaState();
    if (!L || !callback.lua.push(L))
        return;
    HtmlUILua::pushEvent(L, event);
    HtmlUILua::call(L, 1, "'" + domType + "' listener");
}
