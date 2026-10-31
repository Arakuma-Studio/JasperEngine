// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_ONSCREEN_CONTROLS_H
#define HTMLUI_ONSCREEN_CONTROLS_H

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>

#include <string>
#include <vector>

namespace jasperengine{

    // The HTML UI's on-screen controls (ui-multitouch spec 7.5): <onscreen-stick>,
    // <onscreen-button> and <touch-area>, input sources of the Input actions like the component
    // UI's On-Screen Stick / Button / Touch Area, in document px. They take the first pointer that
    // presses them (Pointer Events of the fork, FORK.md G5) and clear what they wrote on release,
    // removal, display: none, a hidden or unloaded document. UA styles: compat/ua/useragent.css.
    class UIOnScreenControl : public Rml::Element, public Rml::EventListener {
    public:
        RMLUI_RTTI_DeclareWithParent(UIOnScreenControl, Rml::Element)

        explicit UIOnScreenControl(const Rml::String& tag);
        ~UIOnScreenControl() override;

        // the three tags' instancers (HtmlUI::initialize)
        static void registerInstancers();
        static void releaseInstancers();

        void ProcessEvent(Rml::Event& event) override;

        // the stick's value (-1..1, y up), pressed state; the touch area's last drag (document px)
        float getValueX() const { return valueX; }
        float getValueY() const { return valueY; }
        bool isPressed() const { return !pointers.empty(); }

    protected:
        void OnUpdate() override;
        void OnChildRemove(Rml::Element* child) override;

    private:
        enum class Kind { STICK, BUTTON, AREA };
        Kind kind;
        bool listening = false;
        std::vector<int> pointers;           // pointer ids holding it (the stick and area: one)
        bool pointerIsMouse = false;
        float centerX = 0, centerY = 0;      // the stick's base centre while held
        float baseOffsetX = 0, baseOffsetY = 0;
        float lastX = 0, lastY = 0;          // the area's last pointer position
        float valueX = 0, valueY = 0;
        bool hiddenForDevice = false;

        uintptr_t owner() const { return reinterpret_cast<uintptr_t>(this); }
        float attributeFloat(const char* name, float fallback) const;
        std::string attributeString(const char* name, const char* fallback) const;
        bool attributeFlag(const char* name) const;
        Rml::Element* part(const char* className);
        void ensureParts();
        void press(Rml::Event& event, int pointerId);
        void move(Rml::Event& event, int pointerId);
        void release(int pointerId, bool dispatch);
        void releaseAll(bool dispatch);
        void updateStick(float px, float py, bool first);
        void placePart(Rml::Element* element, float x, float y);
        void dispatchControlEvent(const char* type, float x, float y);
    };

}

#endif // HTMLUI_ONSCREEN_CONTROLS_H
