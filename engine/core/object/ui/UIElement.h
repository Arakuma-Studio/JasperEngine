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

    // Phase-0 members and the phase-1 DOM API are defined in UIElement.cpp (p1-lua), the
    // Lottie control by p1-vector (htmlui/vector/UIElementLottie.cpp). On an invalid handle
    // every C++ method returns a default and logs a warning (once per method).

    // An element of an HTML/CSS document (UIDocument), with browser names. A value type holding
    // a weak reference: isValid() is false once the element or its document is gone.
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

        void setId(const std::string& id);
        std::string getClassName() const; void setClassName(const std::string& className);
        UIElement getParentElement() const;
        std::vector<UIElement> getChildren() const;   // element children only
        UIElement getFirstElementChild() const; UIElement getLastElementChild() const;
        UIElement getNextElementSibling() const; UIElement getPreviousElementSibling() const;
        int getChildElementCount() const;
        UIElement querySelector(const std::string& selector) const;
        std::vector<UIElement> querySelectorAll(const std::string& selector) const;
        UIElement closest(const std::string& selector) const; bool matches(const std::string& selector) const;
        bool contains(const UIElement& other) const;
        // mutation

        UIElement appendChild(const UIElement& child);              // moves; returns child
        UIElement insertBefore(const UIElement& child, const UIElement& reference);
        UIElement removeChild(const UIElement& child);              // child stays usable (detached pool)
        UIElement replaceChild(const UIElement& newChild, const UIElement& oldChild);
        void remove();
        UIElement cloneNode(bool deep) const;
        std::map<std::string, std::string> getAttributes() const;   // name -> value

        // classList / style / dataset

        void addClass(const std::string& name); void removeClass(const std::string& name);
        bool toggleClass(const std::string& name); bool toggleClass(const std::string& name, bool force);
        bool containsClass(const std::string& name) const;
        std::string getStyle(const std::string& property) const;   // computed value as CSS text
        void setStyle(const std::string& property, const std::string& value);   // inline; "" removes
        void removeStyle(const std::string& property);
        std::string getDataset(const std::string& camelKey) const; void setDataset(const std::string& camelKey, const std::string& value);
        // form controls

        std::string getValue() const; void setValue(const std::string& value);
        bool getChecked() const; void setChecked(bool checked); bool getDisabled() const; void setDisabled(bool disabled);
        // focus / interaction / scroll / geometry

        void focus(); void blur(); void click();
        void scrollIntoView(bool alignToTop = true);
        void scrollTo(float x, float y, bool smooth = false); void scrollBy(float dx, float dy, bool smooth = false);
        float getScrollTop() const; void setScrollTop(float value); float getScrollLeft() const; void setScrollLeft(float value);
        float getScrollWidth() const; float getScrollHeight() const;
        float getClientWidth() const; float getClientHeight() const;
        float getOffsetWidth() const; float getOffsetHeight() const; float getOffsetLeft() const; float getOffsetTop() const;
        UIRect getBoundingClientRect() const;       // border box in document px, through the transforms of the last drawn frame

        // events (C++)

        uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false);
        void removeEventListener(uint64_t listenerId);
        bool dispatchEvent(const std::string& type, const UIValue& detail = UIValue(), bool bubbles = true); // false if prevented

        // --- phase 2 (p2-html, hook H5): HTML documents (loaded through the compat layer) ---
        // getTagName gives the DOM tagName there (upper case for HTML: "DIV", <body> -> "BODY");
        // the engine's own data-htmlui-* attributes and htmlui-* classes are not visible to
        // scripts; selectors, innerHTML and style values are HTML/CSS and are rewritten

        std::string getLocalName() const;           // "div", "body", "template" (lower case)
        std::string getOuterHTML() const;
        void setOuterHTML(const std::string& html); // replaces the element (it becomes detached)
        // position: "beforebegin", "afterbegin", "beforeend", "afterend"
        void insertAdjacentHTML(const std::string& position, const std::string& html);
        // <template>: a detached fragment element holding a fresh copy of its content
        // (appendChild/insertBefore of it move its children, like a DocumentFragment)
        UIElement getContent() const;
        // <dialog>: show/showModal set `open` (no modal backdrop), close removes it and dispatches `close`
        void show(); void showModal(); void close(const std::string& returnValue = std::string());
        std::string getCssText() const;             // the inline declarations
        void setCssText(const std::string& css);    // replaces them

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
