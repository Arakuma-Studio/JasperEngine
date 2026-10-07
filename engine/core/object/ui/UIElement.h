// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIELEMENT_H
#define UIELEMENT_H

#include "Export.h"
#include "math/Rect.h"
#include "UIValue.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// RmlUi stays out of the engine API (swappable core): forward declarations only
namespace Rml {
    class Element;
}

namespace jasperengine{

    class UIEvent;
    struct UIElementHandle;   // UIElementInternal.h: holds Rml::ObserverPtr<Rml::Element>
    struct UIElementAccess;   // UIElementInternal.h: engine-internal access to the RmlUi element

    // getBoundingClientRect, document px
    struct UIRect { float x, y, width, height, top, left, right, bottom; };

    // An element of an HTML/CSS document (UIDocument). A value type holding a weak
    // reference: once the element or its document is gone it is invalid, and every method
    // returns a default (phase 1: and logs once per call site).
    // Phase-0 members are defined in UIElement.cpp; the phase-1 DOM API by p1-lua (same
    // file), the Lottie control by p1-vector (htmlui/vector/UIElementLottie.cpp).
    class JASPERENGINE_API UIElement {
    public:
        UIElement();                                // invalid handle
        bool isValid() const;                       // false once the element or document is gone
        bool operator==(const UIElement&) const;

        // --- phase 0 ---
        std::string getId() const;
        std::string getTagName() const;             // lower case
        std::string getTextContent() const;         // concatenated text of descendants
        void setTextContent(const std::string& text);   // replaces children with escaped text
        std::string getInnerHTML() const;           // RML of the children (HTML after phase 2)
        void setInnerHTML(const std::string& markup);

        bool hasAttribute(const std::string& name) const;
        std::string getAttribute(const std::string& name) const;
        void setAttribute(const std::string& name, const std::string& value);
        void removeAttribute(const std::string& name);

        bool hasClass(const std::string& name) const;
        void setClass(const std::string& name, bool active);
        bool setStyleProperty(const std::string& name, const std::string& value);   // false + warning if RmlUi rejects it
        void removeStyleProperty(const std::string& name);

        Rect getBoundingBox() const;                // border box, canvas px, top-left origin
        bool isPseudoClassSet(const std::string& pseudoClass) const;   // "hover", "active", "focus"

        // --- phase 1 (p1-lua) ---
        // identity / tree
        void setId(const std::string&);
        std::string getClassName() const; void setClassName(const std::string&);
        UIElement getParentElement() const;
        std::vector<UIElement> getChildren() const;   // element children only
        UIElement getFirstElementChild() const; UIElement getLastElementChild() const;
        UIElement getNextElementSibling() const; UIElement getPreviousElementSibling() const;
        int getChildElementCount() const;
        UIElement querySelector(const std::string&) const;
        std::vector<UIElement> querySelectorAll(const std::string&) const;
        UIElement closest(const std::string&) const; bool matches(const std::string&) const;
        bool contains(const UIElement&) const;
        // mutation
        UIElement appendChild(const UIElement& child);              // moves; returns child
        UIElement insertBefore(const UIElement& child, const UIElement& reference);
        UIElement removeChild(const UIElement& child);              // child stays usable (detached pool)
        UIElement replaceChild(const UIElement& newChild, const UIElement& oldChild);
        void remove();
        UIElement cloneNode(bool deep) const;
        // attributes
        std::map<std::string, std::string> getAttributes() const;
        // classList / style / dataset
        void addClass(const std::string&); void removeClass(const std::string&);
        bool toggleClass(const std::string&); bool toggleClass(const std::string&, bool force);
        bool containsClass(const std::string&) const;
        std::string getStyle(const std::string& property) const;   // computed value as CSS text
        void setStyle(const std::string& property, const std::string& value);   // inline; "" removes
        void removeStyle(const std::string& property);
        std::string getDataset(const std::string& camelKey) const; void setDataset(const std::string&, const std::string&);
        // form controls
        std::string getValue() const; void setValue(const std::string&);
        bool getChecked() const; void setChecked(bool); bool getDisabled() const; void setDisabled(bool);
        // focus / interaction / scroll / geometry
        void focus(); void blur(); void click();
        void scrollIntoView(bool alignToTop = true);
        void scrollTo(float x, float y, bool smooth = false); void scrollBy(float dx, float dy, bool smooth = false);
        float getScrollTop() const; void setScrollTop(float); float getScrollLeft() const; void setScrollLeft(float);
        float getScrollWidth() const; float getScrollHeight() const;
        float getClientWidth() const; float getClientHeight() const;
        float getOffsetWidth() const; float getOffsetHeight() const; float getOffsetLeft() const; float getOffsetTop() const;
        UIRect getBoundingClientRect() const;
        // events (C++)
        uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false);
        void removeEventListener(uint64_t listenerId);
        bool dispatchEvent(const std::string& type, const UIValue& detail = UIValue(), bool bubbles = true); // false if prevented

        // --- phase 1 (p1-vector): <lottie> control; logs an error on other elements ---
        void play(); void pause(); void stop(); void seek(float seconds); void setFrame(float frame);
        void setSpeed(float speed); void setDirection(int direction); void setLoop(bool loop); void setLoopCount(int count);
        void setSegment(float startFrame, float endFrame); void setMarker(const std::string& marker);
        bool isPlaying() const; float getCurrentFrame() const; float getTotalFrames() const; float getDuration() const;

    private:
        friend class UIDocumentSystem;
        friend struct UIElementAccess;
        // a weak reference to element (invalid UIElement for nullptr)
        static UIElement wrap(Rml::Element* element);
        Rml::Element* get() const;   // nullptr once invalid

        std::shared_ptr<UIElementHandle> handle;   // phase 0's handle, plus p1-lua's detached-pool token
    };

}

#endif //UIELEMENT_H
