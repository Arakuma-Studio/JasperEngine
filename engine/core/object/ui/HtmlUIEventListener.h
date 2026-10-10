// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIEVENTLISTENER_H
#define HTMLUIEVENTLISTENER_H

// Engine-internal (RmlUi): the listeners behind UIElement/UIDocument addEventListener (C++
// functions and Lua functions) and the DOM event names (phase 1 §3.9.2-§3.9.3)

#include "HtmlUIScriptInternal.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Rml {
    class Context;
}

namespace jasperengine{

    // the RmlUi event behind a DOM event name used by scripts
    struct JASPERENGINE_INTERNAL HtmlUIEventNames {
        // false (and a warning naming the supported events) for DOM events documents never
        // receive (mouseenter, pointer*, touch* ...); what says where ("addEventListener")
        static bool toRml(const std::string& domType, std::string& rmlType, const char* what);
        // the comma-separated list of the DOM events with their own mapping (warnings, docs)
        static const std::string& supportedList();
        // RmlUi event type of the synthesized DOM "change" (text fields: on blur when edited)
        static const char* const changeEvent;
        // parameters of events sent by dispatchEvent: the UIValue detail and the prevented flag
        static const char* const detailParameter;
        static const char* const preventedParameter;
        // the context synthesizes DOM "change" and tracks held keys (installed once per
        // context, idempotent)
        static void installChangeSynthesizer(Rml::Context* context);
        // event is a key down of a key already held (DOM KeyboardEvent.repeat): RmlUi has no
        // repeat flag, the context root's capture listener sees every key down and key up first
        static bool isRepeat(const Rml::Event* event);
    };

    class JASPERENGINE_INTERNAL HtmlUIEventListener final : public Rml::EventListener {
    public:
        struct Callback {
            std::function<void(UIEvent&)> fn;
            HtmlUILuaRef lua;
        };

        // attaches a listener for the DOM event domType (already mapped to rmlType); returns
        // it (its id for removeEventListener) or nullptr if element is null
        static HtmlUIEventListener* attach(Rml::Element* element, const std::string& domType, const std::string& rmlType,
                                           bool capture, bool once, Callback callback);
        // detaches (and so deletes) the listener with this id; false if there is none
        static bool detach(uint64_t id);
        static HtmlUIEventListener* find(uint64_t id);
        // the listeners attached to element, in attach order
        static std::vector<HtmlUIEventListener*> listenersOf(Rml::Element* element);
        // every listener attached to element or one of its descendants
        static std::vector<HtmlUIEventListener*> listenersIn(Rml::Element* root);
        static size_t liveCount();

        uint64_t getId() const { return id; }
        const std::string& getDomType() const { return domType; }
        const std::string& getRmlType() const { return rmlType; }
        bool isCapture() const { return capture; }
        bool isOnce() const { return once; }
        Rml::Element* getElement() const { return element.get(); }
        const Callback& getCallback() const { return callback; }

        // moves this listener (callback and id) to target, a counterpart after a reload; this
        // one is detached and deleted. Returns the new listener
        HtmlUIEventListener* moveTo(Rml::Element* target);
        // detaches it from its element (it is deleted)
        void remove();

        // The document whose <script type="text/lua"> is running (phase 2): a listener attached
        // meanwhile is that script's. When its document reloads, the script runs again and
        // attaches it again, so it is removed with the old document instead of moved to the
        // new elements (moved, every reload added one more copy of it).
        class JASPERENGINE_INTERNAL ScriptScope {
        public:
            explicit ScriptScope(const Rml::Element* document);
            ~ScriptScope();
            ScriptScope(const ScriptScope&) = delete;
            ScriptScope& operator=(const ScriptScope&) = delete;
        private:
            const Rml::Element* previous;
        };
        // the document whose script attached this listener; nullptr for any other listener
        const Rml::Element* getScriptDocument() const { return scriptDocument; }

        void ProcessEvent(Rml::Event& event) override;
        void OnAttach(Rml::Element* element) override;
        void OnDetach(Rml::Element* element) override;

    private:
        HtmlUIEventListener(uint64_t id, std::string domType, std::string rmlType, bool capture, bool once, Callback callback);
        ~HtmlUIEventListener();

        void invoke(UIEvent& event);

        uint64_t id;
        std::string domType;
        std::string rmlType;
        bool capture;
        bool once;
        Callback callback;
        Rml::ObserverPtr<Rml::Element> element;
        Rml::Element* registeredOn = nullptr;   // key in the per-element registry
        const Rml::Element* scriptDocument = nullptr;   // see ScriptScope
        int dispatching = 0;   // ProcessEvent depth: deletion waits for it
        bool detached = false;
    };

}

#endif //HTMLUIEVENTLISTENER_H
