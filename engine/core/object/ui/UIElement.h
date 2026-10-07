// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIELEMENT_H
#define UIELEMENT_H

#include "Export.h"
#include "math/Rect.h"

#include <memory>
#include <string>

namespace jasperengine{

    struct UIElementHandle;   // defined in UIElement.cpp: holds Rml::ObserverPtr<Rml::Element>

    // An element of an HTML/CSS document (UIDocument). A value type holding a weak
    // reference: once the element or its document is gone it is invalid, and every method
    // is a no-op returning empty/false.
    class JASPERENGINE_API UIElement {
    public:
        UIElement();
        bool isValid() const;                       // false once the element or document is gone

        std::string getId() const;
        std::string getTagName() const;
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

    private:
        friend class UIDocumentSystem;
        std::shared_ptr<UIElementHandle> handle;
    };

}

#endif //UIELEMENT_H
