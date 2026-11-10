// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIElement.h"
#include "UIElementInternal.h"
#include "UIEvent.h"
#include "HtmlUIEventListener.h"
#include "HtmlUIScriptInternal.h"

#include "Engine.h"
#include "Log.h"
#include "htmlui/HtmlUIInspectorInternal.h"
#include "htmlui/anim/AnimSvg.h"
#include "htmlui/compat/LuaHooks.h"
#include "htmlui/compat/runtime/HtmlDialogElement.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"
#include "htmlui/effects/EffectInstancers.h"
#include "htmlui/effects/EffectRegistry.h"
#include "htmlui/effects/EffectRuntime.h"
#include "htmlui/effects/EffectValue.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleTypes.h>

// the accumulated transform of an element (RmlUi keeps TransformState private)
#include "../../../libs/rmlui/Source/Core/TransformState.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>

using namespace jasperengine;

const char* const jasperengine::kHtmlUIDeadElementMessage =
    "UIElement: element no longer exists (its document was unloaded or it was removed)";

// Every method starts with it: the RmlUi element, or a default return after one warning per
// method (an invalid handle is a script bug, but must not flood the Console every frame)
#define HTMLUI_ELEMENT(...) \
    Rml::Element* element = get(); \
    if (!element){ \
        static bool logged = false; \
        logDeadElement(logged, __func__); \
        return __VA_ARGS__; \
    }

namespace {

    void logDeadElement(bool& logged, const char* method){
        if (logged)
            return;
        logged = true;
        Log::warn("[HTML UI] %s (UIElement::%s)", kHtmlUIDeadElementMessage, method);
    }

    void appendText(Rml::Element* element, std::string& out){
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
            out += text->GetText();
            return;
        }
        for (int i = 0; i < element->GetNumChildren(); i++){
            appendText(element->GetChild(i), out);
        }
    }

    // an element of the DOM scripts see: no text node, no data-for template
    bool isElementNode(Rml::Element* element){
        return element && !rmlui_dynamic_cast<Rml::ElementText*>(element) && !UIElementAccess::isDataForTemplate(element);
    }

    Rml::Element* rootOf(Rml::Element* element){
        while (element->GetParentNode())
            element = element->GetParentNode();
        return element;
    }

    // the document an element belongs to, or the one its detached tree came from
    Rml::ElementDocument* documentOf(Rml::Element* element){
        if (Rml::ElementDocument* document = element->GetOwnerDocument())
            return document;
        return HtmlUIElementRegistry::poolDocument(rootOf(element));
    }

    // ---- phase 2 (p2-html, hook H5) ----

    using htmlui::compat::LuaHooks;

    // the element is in (or was detached from) an HTML document of the compat layer
    bool isCompatElement(Rml::Element* element){
        return LuaHooks::isCompat(documentOf(element));
    }

    // selectors of HTML documents are CSS: names (body, :focus, ...) are rewritten
    std::string selectorFor(Rml::Element* element, const std::string& selector){
        return isCompatElement(element) ? LuaHooks::selector(selector) : selector;
    }

    bool hiddenAttribute(Rml::Element* element, const std::string& name){
        return LuaHooks::isHiddenAttribute(name) && isCompatElement(element);
    }

    // element (or one of its ancestors) is ancestor
    bool isInclusiveAncestor(Rml::Element* ancestor, Rml::Element* element){
        for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
            if (walk == ancestor)
                return true;
        }
        return false;
    }

    // takes element out of its parent or the detached pool, for an insertion under parent;
    // nullptr (and an error) when the DOM forbids it
    Rml::ElementPtr takeForInsertion(Rml::Element* parent, Rml::Element* child, const char* method){
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(child)){
            Log::error("[HTML UI] UIElement::%s: a document cannot be inserted into an element", method);
            return nullptr;
        }
        if (isInclusiveAncestor(child, parent)){
            Log::error("[HTML UI] UIElement::%s: the new child contains the parent (HierarchyRequestError)", method);
            return nullptr;
        }
        if (Rml::Element* oldParent = child->GetParentNode())
            return oldParent->RemoveChild(child);
        return HtmlUIElementRegistry::release(child);
    }

    // keeps a removed element usable: it goes to the detached pool of its document
    UIElement keepDetached(Rml::ElementPtr removed, Rml::ElementDocument* document){
        Rml::Element* element = HtmlUIElementRegistry::adopt(std::move(removed), document);
        return UIElementAccess::wrap(element);
    }

    // camelCase -> kebab-case (backgroundColor -> background-color)
    std::string hyphenate(const std::string& camel){
        std::string out;
        out.reserve(camel.size() + 4);
        for (char c : camel){
            if (std::isupper((unsigned char)c)){
                out += '-';
                out += (char)std::tolower((unsigned char)c);
            }else{
                out += c;
            }
        }
        return out;
    }

    Rml::ElementFormControl* formControl(Rml::Element* element){
        return rmlui_dynamic_cast<Rml::ElementFormControl*>(element);
    }

    const Rml::TransformState* transformStateOf(Rml::Element* element){
        for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
            if (const Rml::TransformState* state = walk->GetTransformState())
                return state;
        }
        return nullptr;
    }

}

UIElement::UIElement(){
}

UIElement UIElement::wrap(Rml::Element* element){
    UIElement result;
    if (element){
        result.handle = std::make_shared<UIElementHandle>();
        result.handle->element = element->GetObserverPtr();
        result.handle->token = HtmlUIElementRegistry::token(element);
    }
    return result;
}

Rml::Element* UIElement::get() const{
    if (!handle)
        return nullptr;

    Rml::Element* element = handle->element.get();
    if (!element)
        return nullptr;

    // an unloaded document is detached at once but only released by the next update
    if (Rml::ElementDocument* document = element->GetOwnerDocument()){
        if (!document->GetParentNode() || !document->GetContext())
            return nullptr;
        return element;
    }

    // not in a document: alive while its tree is in the detached pool (createElement, removeChild ...)
    return HtmlUIElementRegistry::isDetachedAlive(element) ? element : nullptr;
}

bool UIElement::isValid() const{
    return get() != nullptr;
}

bool UIElement::operator==(const UIElement& other) const{
    const Rml::Element* a = handle ? handle->element.get() : nullptr;
    const Rml::Element* b = other.handle ? other.handle->element.get() : nullptr;
    return a == b;
}

// ---- phase 0 ----

std::string UIElement::getId() const{
    HTMLUI_ELEMENT(std::string())
    return element->GetId();
}

std::string UIElement::getTagName() const{
    HTMLUI_ELEMENT(std::string())
    if (isCompatElement(element))
        return LuaHooks::tagName(element);
    return element->GetTagName();
}

std::string UIElement::getTextContent() const{
    HTMLUI_ELEMENT(std::string())
    std::string text;
    appendText(element, text);
    return text;
}

void UIElement::setTextContent(const std::string& text){
    HTMLUI_ELEMENT()

    // a text node has no children of its own
    if (Rml::ElementText* textNode = rmlui_dynamic_cast<Rml::ElementText*>(element)){
        textNode->SetText(text);
        return;
    }

    while (element->GetNumChildren() > 0){
        element->RemoveChild(element->GetFirstChild());
    }

    // a text node takes the string as it is: no markup or entity is parsed
    if (!text.empty()){
        Rml::ElementPtr node = Rml::Factory::InstanceElement(nullptr, "#text", "#text", Rml::XMLAttributes());
        if (Rml::ElementText* textNode = rmlui_dynamic_cast<Rml::ElementText*>(node.get())){
            textNode->SetText(text);
            element->AppendChild(std::move(node));
        }
    }
}

std::string UIElement::getInnerHTML() const{
    HTMLUI_ELEMENT(std::string())
    if (isCompatElement(element))
        return LuaHooks::innerHTML(element);
    return element->GetInnerRML();
}

void UIElement::setInnerHTML(const std::string& markup){
    HTMLUI_ELEMENT()
    for (Rml::Element* ancestor = element; ancestor; ancestor = ancestor->GetParentNode()){
        if (ancestor->GetTagName() == "svg"){
            element->SetInnerRML(markup);
            return;
        }
    }
    if (isCompatElement(element)){
        element->SetInnerRML(LuaHooks::fragmentToRml(markup, documentOf(element), "innerHTML"));
        return;
    }
    element->SetInnerRML(markup);
}

bool UIElement::hasAttribute(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    if (hiddenAttribute(element, name))
        return false;
    return element->HasAttribute(name);
}

std::string UIElement::getAttribute(const std::string& name) const{
    HTMLUI_ELEMENT(std::string())
    if (isCompatElement(element)){
        if (LuaHooks::isHiddenAttribute(name))
            return std::string();
        if (name == "class")
            return LuaHooks::visibleClassNames(element->GetAttribute<Rml::String>("class", ""));
    }
    return element->GetAttribute<Rml::String>(name, "");
}

void UIElement::setAttribute(const std::string& name, const std::string& value){
    HTMLUI_ELEMENT()
    if (name == "draw" && element->HasAttribute("data-anime-drawable")){
        AnimSvg::setDraw(*this, value);
        return;
    }
    if (isCompatElement(element)){
        if (LuaHooks::isHiddenAttribute(name)){
            Log::warn("[HTML UI] UIElement::setAttribute: '%s' is reserved for the engine", name.c_str());
            return;
        }
        if (name == "class"){
            element->SetAttribute("class", LuaHooks::withHiddenClasses(value, element->GetAttribute<Rml::String>("class", "")));
            return;
        }
        if (name == "style"){
            setCssText(value);
            return;
        }
        if (name == "data-lucide"){
            // the icon is expanded again, in place (lucide.createIcons() on this element)
            std::string shapes;
            std::vector<std::pair<std::string, std::string>> attributes;
            const std::string svg = LuaHooks::lucideSvg(element, value, &shapes, &attributes);
            if (svg.empty()){
                Log::warn("[HTML UI] Lucide icon '%s' does not exist", value.c_str());
                element->SetAttribute(name, value);
                return;
            }
            if (element->GetTagName() == "svg"){
                for (const auto& pair : attributes){
                    if (pair.first == "class")
                        element->SetAttribute("class", LuaHooks::withHiddenClasses(pair.second, element->GetAttribute<Rml::String>("class", "")));
                    else
                        element->SetAttribute(pair.first, pair.second);
                }
                element->SetInnerRML(shapes);
            }else{
                setOuterHTML(svg);
            }
            return;
        }
    }
    element->SetAttribute(name, value);
}

void UIElement::removeAttribute(const std::string& name){
    HTMLUI_ELEMENT()
    if (isCompatElement(element)){
        if (LuaHooks::isHiddenAttribute(name))
            return;
        if (name == "class"){
            const std::string hidden = LuaHooks::withHiddenClasses("", element->GetAttribute<Rml::String>("class", ""));
            if (!hidden.empty()){
                element->SetAttribute("class", hidden);
                return;
            }
        }
    }
    element->RemoveAttribute(name);
}

bool UIElement::hasClass(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return false;
    return element->IsClassSet(name);
}

void UIElement::setClass(const std::string& name, bool active){
    HTMLUI_ELEMENT()
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return;
    element->SetClass(name, active);
}

bool UIElement::setStyleProperty(const std::string& name, const std::string& value){
    HTMLUI_ELEMENT(false)

    // parsed here first, so a rejected declaration gives one warning naming the element
    Rml::PropertyDictionary properties;
    if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(properties, name, value) || !element->SetProperty(name, value)){
        Log::warn("[HTML UI] style '%s: %s' rejected on element %s", name.c_str(), value.c_str(), element->GetAddress().c_str());
        return false;
    }

    return true;
}

void UIElement::removeStyleProperty(const std::string& name){
    HTMLUI_ELEMENT()
    element->RemoveProperty(name);
}

Rect UIElement::getBoundingBox() const{
    HTMLUI_ELEMENT(Rect(0, 0, 0, 0))
    // no box (display: none, or not in a document): 0, as getBoundingClientRect
    if (!htmlui::inspect::hasLayoutBox(element))
        return Rect(0, 0, 0, 0);

    const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);

    // document px to canvas px: they differ only while a reference size lays the context
    // out at another size than the canvas (the editor)
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (Rml::Context* context = element->GetContext()){
        const Rml::Vector2i dimensions = context->GetDimensions();
        if (dimensions.x > 0 && dimensions.y > 0 && Engine::getCanvasWidth() > 0 && Engine::getCanvasHeight() > 0){
            scaleX = (float)Engine::getCanvasWidth() / (float)dimensions.x;
            scaleY = (float)Engine::getCanvasHeight() / (float)dimensions.y;
        }
    }

    return Rect(position.x * scaleX, position.y * scaleY, size.x * scaleX, size.y * scaleY);
}

bool UIElement::isPseudoClassSet(const std::string& pseudoClass) const{
    HTMLUI_ELEMENT(false)
    return element->IsPseudoClassSet(pseudoClass);
}

// ---- phase 1: identity / tree ----

void UIElement::setId(const std::string& id){
    HTMLUI_ELEMENT()
    element->SetId(id);
}

std::string UIElement::getClassName() const{
    HTMLUI_ELEMENT(std::string())
    if (isCompatElement(element))
        return LuaHooks::visibleClassNames(element->GetClassNames());
    return element->GetClassNames();
}

void UIElement::setClassName(const std::string& className){
    HTMLUI_ELEMENT()
    if (isCompatElement(element)){
        element->SetClassNames(LuaHooks::withHiddenClasses(className, element->GetClassNames()));
        return;
    }
    element->SetClassNames(className);
}

UIElement UIElement::getParentElement() const{
    HTMLUI_ELEMENT(UIElement())
    Rml::Element* parent = element->GetParentNode();
    // the context root above a document is no element of it
    if (!parent || rmlui_dynamic_cast<Rml::ElementDocument*>(element))
        return UIElement();
    return wrap(parent);
}

std::vector<UIElement> UIElement::getChildren() const{
    HTMLUI_ELEMENT(std::vector<UIElement>())
    std::vector<UIElement> children;
    for (int i = 0; i < element->GetNumChildren(); i++){
        Rml::Element* child = element->GetChild(i);
        if (isElementNode(child))
            children.push_back(wrap(child));
    }
    return children;
}

UIElement UIElement::getFirstElementChild() const{
    HTMLUI_ELEMENT(UIElement())
    for (int i = 0; i < element->GetNumChildren(); i++){
        if (isElementNode(element->GetChild(i)))
            return wrap(element->GetChild(i));
    }
    return UIElement();
}

UIElement UIElement::getLastElementChild() const{
    HTMLUI_ELEMENT(UIElement())
    for (int i = element->GetNumChildren() - 1; i >= 0; i--){
        if (isElementNode(element->GetChild(i)))
            return wrap(element->GetChild(i));
    }
    return UIElement();
}

UIElement UIElement::getNextElementSibling() const{
    HTMLUI_ELEMENT(UIElement())
    if (rmlui_dynamic_cast<Rml::ElementDocument*>(element))
        return UIElement();
    for (Rml::Element* sibling = element->GetNextSibling(); sibling; sibling = sibling->GetNextSibling()){
        if (isElementNode(sibling))
            return wrap(sibling);
    }
    return UIElement();
}

UIElement UIElement::getPreviousElementSibling() const{
    HTMLUI_ELEMENT(UIElement())
    if (rmlui_dynamic_cast<Rml::ElementDocument*>(element))
        return UIElement();
    for (Rml::Element* sibling = element->GetPreviousSibling(); sibling; sibling = sibling->GetPreviousSibling()){
        if (isElementNode(sibling))
            return wrap(sibling);
    }
    return UIElement();
}

int UIElement::getChildElementCount() const{
    HTMLUI_ELEMENT(0)
    int count = 0;
    for (int i = 0; i < element->GetNumChildren(); i++){
        if (isElementNode(element->GetChild(i)))
            count++;
    }
    return count;
}

UIElement UIElement::querySelector(const std::string& selector) const{
    HTMLUI_ELEMENT(UIElement())
    const std::string rewritten = selectorFor(element, selector);
    Rml::Element* found = element->QuerySelector(rewritten);
    // past a data-for template: the first match that is not one
    if (UIElementAccess::isDataForTemplate(found)){
        Rml::ElementList elements;
        element->QuerySelectorAll(elements, rewritten);
        found = nullptr;
        for (Rml::Element* candidate : elements){
            if (!UIElementAccess::isDataForTemplate(candidate)){
                found = candidate;
                break;
            }
        }
    }
    return wrap(found);
}

std::vector<UIElement> UIElement::querySelectorAll(const std::string& selector) const{
    HTMLUI_ELEMENT(std::vector<UIElement>())
    Rml::ElementList elements;
    element->QuerySelectorAll(elements, selectorFor(element, selector));
    std::vector<UIElement> result;
    result.reserve(elements.size());
    for (Rml::Element* found : elements){
        if (!UIElementAccess::isDataForTemplate(found))
            result.push_back(wrap(found));
    }
    return result;
}

UIElement UIElement::closest(const std::string& selector) const{
    HTMLUI_ELEMENT(UIElement())
    const std::string rewritten = selectorFor(element, selector);
    // the DOM's closest starts at the element itself, RmlUi's at its parent
    if (element->Matches(rewritten))
        return wrap(element);
    return wrap(element->Closest(rewritten));
}

bool UIElement::matches(const std::string& selector) const{
    HTMLUI_ELEMENT(false)
    return element->Matches(selectorFor(element, selector));
}

bool UIElement::contains(const UIElement& other) const{
    HTMLUI_ELEMENT(false)
    Rml::Element* otherElement = other.get();
    return otherElement && isInclusiveAncestor(element, otherElement);
}

// ---- mutation ----

UIElement UIElement::appendChild(const UIElement& child){
    HTMLUI_ELEMENT(UIElement())
    Rml::Element* childElement = child.get();
    if (!childElement){
        Log::error("[HTML UI] UIElement::appendChild: the child is not a valid element");
        return UIElement();
    }
    // template.content (a DocumentFragment): its children move, the fragment stays empty
    if (childElement->GetTagName() == LuaHooks::kFragmentTag && !childElement->GetParentNode()){
        while (childElement->GetNumChildren() > 0)
            element->AppendChild(childElement->RemoveChild(childElement->GetChild(0)));
        return child;
    }
    Rml::ElementPtr taken = takeForInsertion(element, childElement, "appendChild");
    if (!taken)
        return UIElement();
    return wrap(element->AppendChild(std::move(taken)));
}

UIElement UIElement::insertBefore(const UIElement& child, const UIElement& reference){
    HTMLUI_ELEMENT(UIElement())
    Rml::Element* childElement = child.get();
    if (!childElement){
        Log::error("[HTML UI] UIElement::insertBefore: the child is not a valid element");
        return UIElement();
    }
    // no reference: append, as the DOM does for null
    Rml::Element* referenceElement = reference.get();
    if (!referenceElement)
        return appendChild(child);
    if (referenceElement->GetParentNode() != element){
        Log::error("[HTML UI] UIElement::insertBefore: the reference element is not a child of this element (NotFoundError)");
        return UIElement();
    }
    if (childElement->GetTagName() == LuaHooks::kFragmentTag && !childElement->GetParentNode()){
        while (childElement->GetNumChildren() > 0)
            element->InsertBefore(childElement->RemoveChild(childElement->GetChild(0)), referenceElement);
        return child;
    }
    if (referenceElement == childElement)
        return child;
    Rml::ElementPtr taken = takeForInsertion(element, childElement, "insertBefore");
    if (!taken)
        return UIElement();
    return wrap(element->InsertBefore(std::move(taken), referenceElement));
}

UIElement UIElement::removeChild(const UIElement& child){
    HTMLUI_ELEMENT(UIElement())
    Rml::Element* childElement = child.get();
    if (!childElement || childElement->GetParentNode() != element){
        Log::error("[HTML UI] UIElement::removeChild: the element is not a child of this element (NotFoundError)");
        return UIElement();
    }
    Rml::ElementDocument* document = documentOf(element);
    return keepDetached(element->RemoveChild(childElement), document);
}

UIElement UIElement::replaceChild(const UIElement& newChild, const UIElement& oldChild){
    HTMLUI_ELEMENT(UIElement())
    Rml::Element* oldElement = oldChild.get();
    Rml::Element* newElement = newChild.get();
    if (!oldElement || oldElement->GetParentNode() != element){
        Log::error("[HTML UI] UIElement::replaceChild: the old element is not a child of this element (NotFoundError)");
        return UIElement();
    }
    if (!newElement){
        Log::error("[HTML UI] UIElement::replaceChild: the new child is not a valid element");
        return UIElement();
    }
    if (newElement == oldElement)
        return oldChild;
    Rml::ElementPtr taken = takeForInsertion(element, newElement, "replaceChild");
    if (!taken)
        return UIElement();
    Rml::ElementDocument* document = documentOf(element);
    return keepDetached(element->ReplaceChild(std::move(taken), oldElement), document);
}

void UIElement::remove(){
    HTMLUI_ELEMENT()
    Rml::Element* parent = element->GetParentNode();
    // a document is unloaded through its UIDocument, not removed
    if (!parent || rmlui_dynamic_cast<Rml::ElementDocument*>(element))
        return;
    Rml::ElementDocument* document = documentOf(element);
    keepDetached(parent->RemoveChild(element), document);
}

UIElement UIElement::cloneNode(bool deep) const{
    HTMLUI_ELEMENT(UIElement())

    Rml::ElementPtr clone;
    if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
        clone = Rml::Factory::InstanceElement(nullptr, "#text", "#text", Rml::XMLAttributes());
        if (Rml::ElementText* cloneText = rmlui_dynamic_cast<Rml::ElementText*>(clone.get()))
            cloneText->SetText(text->GetText());
    }else if (rmlui_dynamic_cast<Rml::ElementDocument*>(element)){
        Log::error("[HTML UI] UIElement::cloneNode: a document cannot be cloned");
        return UIElement();
    }else{
        clone = element->Clone();
        if (clone && !deep){
            while (clone->GetNumChildren() > 0)
                clone->RemoveChild(clone->GetFirstChild());
        }
    }
    if (!clone)
        return UIElement();
    return keepDetached(std::move(clone), documentOf(element));
}

// ---- attributes ----

std::map<std::string, std::string> UIElement::getAttributes() const{
    HTMLUI_ELEMENT((std::map<std::string, std::string>()))
    const bool compat = isCompatElement(element);
    std::map<std::string, std::string> attributes;
    for (const auto& pair : element->GetAttributes()){
        if (compat && LuaHooks::isHiddenAttribute(pair.first))
            continue;
        std::string value = pair.second.Get<Rml::String>();
        if (compat && pair.first == "class"){
            value = LuaHooks::visibleClassNames(value);
            if (value.empty())
                continue;
        }
        attributes[pair.first] = value;
    }
    return attributes;
}

// ---- classList / style / dataset ----

void UIElement::addClass(const std::string& name){
    HTMLUI_ELEMENT()
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return;
    element->SetClass(name, true);
}

void UIElement::removeClass(const std::string& name){
    HTMLUI_ELEMENT()
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return;
    element->SetClass(name, false);
}

bool UIElement::toggleClass(const std::string& name){
    HTMLUI_ELEMENT(false)
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return false;
    const bool active = !element->IsClassSet(name);
    element->SetClass(name, active);
    return active;
}

bool UIElement::toggleClass(const std::string& name, bool force){
    HTMLUI_ELEMENT(false)
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return false;
    element->SetClass(name, force);
    return force;
}

bool UIElement::containsClass(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    if (LuaHooks::isHiddenClass(name) && isCompatElement(element))
        return false;
    return element->IsClassSet(name);
}

std::string UIElement::getStyle(const std::string& property) const{
    HTMLUI_ELEMENT(std::string())
    // what Chromium's theme adjusts (a themed checkbox's box, a button's display): its value
    std::string themed;
    if (htmlui::compat::forms::themedComputedValue(element, Rml::StyleSheetSpecification::GetPropertyId(property), themed))
        return themed;
    // a line-clamp container computes to flow-root or inline-block, as Chromium's getComputedStyle (fork, P6-L1)
    if (property == "display" && element->GetComputedValues().line_clamp() > 0)
        return element->GetComputedValues().display() == Rml::Style::Display::InlineBlock ? "inline-block" : "flow-root";
    const Rml::Property* value = element->GetProperty(property);
    return value ? value->ToString() : std::string();
}

void UIElement::setStyle(const std::string& property, const std::string& value){
    HTMLUI_ELEMENT()
    if (value.empty()){
        removeStyle(property);
    }else if (isCompatElement(element)){
        // CSS: rewritten into the RCSS declarations it stands for (p2-html, hook H5)
        const auto declarations = LuaHooks::rewriteDeclarations(property + ": " + value);
        if (declarations.empty())
            Log::warn("[HTML UI] style '%s: %s' is not supported (dropped) on element %s", property.c_str(), value.c_str(), element->GetAddress().c_str());
        for (const auto& declaration : declarations)
            setStyleProperty(declaration.first, declaration.second);
    }else{
        setStyleProperty(property, value);
    }
}

void UIElement::removeStyle(const std::string& property){
    HTMLUI_ELEMENT()
    if (isCompatElement(element)){
        // every RCSS property the CSS property is written as
        for (const auto& declaration : LuaHooks::rewriteDeclarations(property + ": initial"))
            element->RemoveProperty(declaration.first);
    }
    element->RemoveProperty(property);
}

std::string UIElement::getDataset(const std::string& camelKey) const{
    HTMLUI_ELEMENT(std::string())
    const std::string name = "data-" + hyphenate(camelKey);
    if (hiddenAttribute(element, name))
        return std::string();
    return element->GetAttribute<Rml::String>(name, "");
}

void UIElement::setDataset(const std::string& camelKey, const std::string& value){
    HTMLUI_ELEMENT()
    const std::string name = "data-" + hyphenate(camelKey);
    if (hiddenAttribute(element, name))
        return;
    element->SetAttribute(name, value);
}

// ---- form controls ----

std::string UIElement::getValue() const{
    HTMLUI_ELEMENT(std::string())
    if (Rml::ElementFormControl* control = formControl(element))
        return control->GetValue();
    return element->GetAttribute<Rml::String>("value", "");
}

void UIElement::setValue(const std::string& value){
    HTMLUI_ELEMENT()
    if (Rml::ElementFormControl* control = formControl(element)){
        control->SetValue(value);
    }else{
        element->SetAttribute("value", value);
    }
}

bool UIElement::getChecked() const{
    HTMLUI_ELEMENT(false)
    return element->HasAttribute("checked");
}

void UIElement::setChecked(bool checked){
    HTMLUI_ELEMENT()
    if (checked){
        element->SetAttribute("checked", "");
    }else{
        element->RemoveAttribute("checked");
    }
}

bool UIElement::getDisabled() const{
    HTMLUI_ELEMENT(false)
    return element->HasAttribute("disabled");
}

void UIElement::setDisabled(bool disabled){
    HTMLUI_ELEMENT()
    if (disabled){
        element->SetAttribute("disabled", "");
    }else{
        element->RemoveAttribute("disabled");
    }
}

// ---- focus / interaction / scroll / geometry ----

void UIElement::focus(){
    HTMLUI_ELEMENT()
    element->Focus();
}

void UIElement::blur(){
    HTMLUI_ELEMENT()
    element->Blur();
}

void UIElement::click(){
    HTMLUI_ELEMENT()
    element->Click();
}

bool UIElement::setPointerCapture(int pointerId){
    HTMLUI_ELEMENT(false)
    return element->SetPointerCapture(pointerId);
}

bool UIElement::releasePointerCapture(int pointerId){
    HTMLUI_ELEMENT(false)
    return element->ReleasePointerCapture(pointerId);
}

bool UIElement::hasPointerCapture(int pointerId) const{
    HTMLUI_ELEMENT(false)
    return element->HasPointerCapture(pointerId);
}

void UIElement::scrollIntoView(bool alignToTop){
    HTMLUI_ELEMENT()
    element->ScrollIntoView(alignToTop);
}

void UIElement::scrollTo(float x, float y, bool smooth){
    HTMLUI_ELEMENT()
    element->ScrollTo(Rml::Vector2f(x, y), smooth ? Rml::ScrollBehavior::Smooth : Rml::ScrollBehavior::Instant);
}

void UIElement::scrollBy(float dx, float dy, bool smooth){
    HTMLUI_ELEMENT()
    const Rml::Vector2f target(element->GetScrollLeft() + dx, element->GetScrollTop() + dy);
    element->ScrollTo(target, smooth ? Rml::ScrollBehavior::Smooth : Rml::ScrollBehavior::Instant);
}

float UIElement::getScrollTop() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetScrollTop() : 0.0f;
}

void UIElement::setScrollTop(float value){
    HTMLUI_ELEMENT()
    element->SetScrollTop(value);
}

float UIElement::getScrollLeft() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetScrollLeft() : 0.0f;
}

void UIElement::setScrollLeft(float value){
    HTMLUI_ELEMENT()
    element->SetScrollLeft(value);
}

float UIElement::getScrollWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetScrollWidth() : 0.0f;
}

float UIElement::getScrollHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetScrollHeight() : 0.0f;
}

float UIElement::getClientWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetClientWidth() : 0.0f;
}

float UIElement::getClientHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetClientHeight() : 0.0f;
}

float UIElement::getOffsetWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetOffsetWidth() : 0.0f;
}

float UIElement::getOffsetHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetOffsetHeight() : 0.0f;
}

float UIElement::getOffsetLeft() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetOffsetLeft() : 0.0f;
}

float UIElement::getOffsetTop() const{
    HTMLUI_ELEMENT(0.0f)
    return htmlui::inspect::hasLayoutBox(element) ? element->GetOffsetTop() : 0.0f;
}

UIRect UIElement::getBoundingClientRect() const{
    HTMLUI_ELEMENT((UIRect{0, 0, 0, 0, 0, 0, 0, 0}))
    // an element without a box (display: none on it or an ancestor, display: contents, not in
    // a document): all zeros, as in a browser (RmlUi keeps the box it had last)
    if (!htmlui::inspect::hasLayoutBox(element))
        return UIRect{0, 0, 0, 0, 0, 0, 0, 0};

    // the border box in document px, through the element's transforms as in a browser
    const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);

    float left = position.x, top = position.y;
    float right = position.x + size.x, bottom = position.y + size.y;

    const Rml::TransformState* state = transformStateOf(element);
    if (const Rml::Matrix4f* transform = state ? state->GetTransform() : nullptr){
        const Rml::Vector2f corners[4] = {
            {position.x, position.y}, {position.x + size.x, position.y},
            {position.x, position.y + size.y}, {position.x + size.x, position.y + size.y}};
        left = top = INFINITY;
        right = bottom = -INFINITY;
        for (const Rml::Vector2f& corner : corners){
            Rml::Vector4f projected = (*transform) * Rml::Vector4f(corner.x, corner.y, 0.0f, 1.0f);
            if (projected.w != 0.0f){
                projected.x /= projected.w;
                projected.y /= projected.w;
            }
            left = std::min(left, projected.x);
            top = std::min(top, projected.y);
            right = std::max(right, projected.x);
            bottom = std::max(bottom, projected.y);
        }
    }

    UIRect rect;
    rect.x = rect.left = left;
    rect.y = rect.top = top;
    rect.width = right - left;
    rect.height = bottom - top;
    rect.right = right;
    rect.bottom = bottom;
    return rect;
}

// ---- events ----

uint64_t UIElement::addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture){
    HTMLUI_ELEMENT(0)
    if (!fn)
        return 0;
    std::string rmlType;
    if (!HtmlUIEventNames::toRml(type, rmlType, "UIElement::addEventListener"))
        return 0;
    HtmlUIEventListener::Callback callback;
    callback.fn = std::move(fn);
    HtmlUIEventListener* listener = HtmlUIEventListener::attach(element, type, rmlType, capture, false, std::move(callback));
    return listener ? listener->getId() : 0;
}

void UIElement::removeEventListener(uint64_t listenerId){
    HTMLUI_ELEMENT()
    HtmlUIEventListener* listener = HtmlUIEventListener::find(listenerId);
    if (listener && listener->getElement() == element)
        listener->remove();
}

bool UIElement::dispatchEvent(const std::string& type, const UIValue& detail, bool bubbles){
    HTMLUI_ELEMENT(true)
    std::string rmlType;
    if (!HtmlUIEventNames::toRml(type, rmlType, "UIElement::dispatchEvent"))
        return true;

    bool prevented = false;
    Rml::Dictionary parameters;
    parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
    parameters[HtmlUIEventNames::preventedParameter] = (void*)&prevented;
    // data-value bindings read the value of an input event
    if (rmlType == "change" || rmlType == HtmlUIEventNames::changeEvent)
        parameters["value"] = getValue();
    element->DispatchEvent(rmlType, parameters, true, bubbles);
    return !prevented;
}

// ---- phase 2 (p2-html, hook H5) ----

namespace {

    // the nodes of an HTML fragment (RML for RML documents), made in the element's document
    std::vector<Rml::ElementPtr> parseMarkup(Rml::Element* context, const std::string& markup, const char* what){
        std::vector<Rml::ElementPtr> nodes;
        Rml::ElementDocument* document = documentOf(context);
        if (!document)
            return nodes;
        const std::string rml = isCompatElement(context) ? LuaHooks::fragmentToRml(markup, document, what) : markup;
        Rml::ElementPtr container = document->CreateElement("div");
        container->SetInnerRML(rml);
        while (container->GetNumChildren() > 0)
            nodes.push_back(container->RemoveChild(container->GetChild(0)));
        return nodes;
    }

    // "name: value; name: value" (RCSS as written)
    std::vector<std::pair<std::string, std::string>> splitDeclarations(const std::string& css){
        std::vector<std::pair<std::string, std::string>> declarations;
        size_t start = 0;
        for (size_t i = 0; i <= css.size(); i++){
            if (i < css.size() && css[i] != ';')
                continue;
            const std::string part = css.substr(start, i - start);
            start = i + 1;
            const size_t colon = part.find(':');
            if (colon == std::string::npos)
                continue;
            auto trim = [](std::string text){
                const size_t begin = text.find_first_not_of(" \t\r\n");
                const size_t end = text.find_last_not_of(" \t\r\n");
                return (begin == std::string::npos) ? std::string() : text.substr(begin, end - begin + 1);
            };
            const std::string name = trim(part.substr(0, colon));
            const std::string value = trim(part.substr(colon + 1));
            if (!name.empty() && !value.empty())
                declarations.emplace_back(name, value);
        }
        return declarations;
    }

}

std::string UIElement::getLocalName() const{
    HTMLUI_ELEMENT(std::string())
    if (isCompatElement(element))
        return LuaHooks::localName(element);
    return element->GetTagName();
}

std::string UIElement::getOuterHTML() const{
    HTMLUI_ELEMENT(std::string())
    return LuaHooks::outerHTML(element);
}

void UIElement::setOuterHTML(const std::string& html){
    HTMLUI_ELEMENT()
    Rml::Element* parent = element->GetParentNode();
    if (!parent || rmlui_dynamic_cast<Rml::ElementDocument*>(element)){
        Log::error("[HTML UI] UIElement::setOuterHTML: the element has no parent (NoModificationAllowedError)");
        return;
    }
    Rml::ElementDocument* document = documentOf(element);
    for (Rml::ElementPtr& node : parseMarkup(element, html, "outerHTML"))
        parent->InsertBefore(std::move(node), element);
    keepDetached(parent->RemoveChild(element), document);
}

void UIElement::insertAdjacentHTML(const std::string& position, const std::string& html){
    HTMLUI_ELEMENT()
    std::string where = position;
    for (char& c : where)
        c = (char)std::tolower((unsigned char)c);
    Rml::Element* parent = element->GetParentNode();
    const bool isDocument = rmlui_dynamic_cast<Rml::ElementDocument*>(element) != nullptr;
    if ((where == "beforebegin" || where == "afterend") && (!parent || isDocument)){
        Log::error("[HTML UI] UIElement::insertAdjacentHTML('%s'): the element has no parent (NoModificationAllowedError)", position.c_str());
        return;
    }
    if (where != "beforebegin" && where != "afterbegin" && where != "beforeend" && where != "afterend"){
        Log::error("[HTML UI] UIElement::insertAdjacentHTML: '%s' is not beforebegin, afterbegin, beforeend or afterend (SyntaxError)", position.c_str());
        return;
    }
    std::vector<Rml::ElementPtr> nodes = parseMarkup(element, html, "insertAdjacentHTML");
    // the nodes keep their order: each goes before the same reference
    Rml::Element* parentOfNodes = (where == "beforebegin" || where == "afterend") ? parent : element;
    Rml::Element* reference = nullptr;
    if (where == "beforebegin")
        reference = element;
    else if (where == "afterend")
        reference = element->GetNextSibling();
    else if (where == "afterbegin")
        reference = element->GetFirstChild();
    for (Rml::ElementPtr& node : nodes){
        if (reference)
            parentOfNodes->InsertBefore(std::move(node), reference);
        else
            parentOfNodes->AppendChild(std::move(node));
    }
}

UIElement UIElement::getContent() const{
    HTMLUI_ELEMENT(UIElement())
    if (!isCompatElement(element) || element->GetTagName() != "htmltemplate")
        return UIElement();
    Rml::ElementDocument* document = documentOf(element);
    Rml::ElementPtr fragment = document->CreateElement(LuaHooks::kFragmentTag);
    fragment->SetInnerRML(LuaHooks::templateContent(element));
    return keepDetached(std::move(fragment), document);
}

void UIElement::show(){
    HTMLUI_ELEMENT()
    if (auto* dialog = rmlui_dynamic_cast<htmlui::compat::HtmlDialogElement*>(element)){
        dialog->show();
        return;
    }
    element->SetAttribute("open", "");
}

void UIElement::showModal(){
    HTMLUI_ELEMENT()
    // no modal backdrop or focus trap (HTMLUI-HTML-0025)
    if (auto* dialog = rmlui_dynamic_cast<htmlui::compat::HtmlDialogElement*>(element)){
        dialog->showModal();
        return;
    }
    show();
}

void UIElement::close(const std::string& returnValue){
    HTMLUI_ELEMENT()
    if (auto* dialog = rmlui_dynamic_cast<htmlui::compat::HtmlDialogElement*>(element)){
        dialog->close(returnValue);
        return;
    }
    if (!element->HasAttribute("open"))
        return;
    element->RemoveAttribute("open");
    Rml::Dictionary parameters;
    if (!returnValue.empty())
        parameters["returnValue"] = returnValue;
    element->DispatchEvent("close", parameters, false, false);
}

std::string UIElement::getCssText() const{
    HTMLUI_ELEMENT(std::string())
    std::string text;
    for (const auto& pair : element->GetLocalStyleProperties()){
        if (!text.empty())
            text += ' ';
        text += Rml::StyleSheetSpecification::GetPropertyName(pair.first) + ": " + pair.second.ToString() + ";";
    }
    return text;
}

std::vector<UIEffectInfo> UIElement::getEffects() const{
    HTMLUI_ELEMENT(std::vector<UIEffectInfo>())
    std::vector<UIEffectInfo> effects;
    std::map<std::string, int> ordinals;
    for (htmlui::EffectUse& use : htmlui::collectEffectUses(element)){
        UIEffectInfo info;
        info.property = use.property;
        info.index = use.index;
        info.name = use.name;
        info.form = use.filterForm ? "filter" : "paint";
        info.params = std::move(use.params);
        // phase 6: the simulation of a stateful effect (the n-th use of a name is its n-th state)
        const htmlui::EffectManifest* manifest = htmlui::EffectRegistry::get().find(use.name);
        if (manifest && manifest->stateful()){
            info.stateful = true;
            const htmlui::EffectSimulationInfo sim = htmlui::EffectRuntime::get().simulationInfo(element, use.name, ordinals[use.name]++);
            info.stateBytes = sim.bytes;
            info.steps = sim.steps;
            info.simTime = sim.time;
            info.paused = sim.paused;
        }
        effects.push_back(std::move(info));
    }
    return effects;
}

int UIElement::effectSplat(const UIEffectSplat& splat){
    HTMLUI_ELEMENT(0)
    htmlui::EffectSplatData data;
    data.position = Rml::Vector2f(splat.x, splat.y);
    data.radius = std::max(0.f, splat.radius);
    data.velocity = Rml::Vector2f(splat.dx, splat.dy);
    data.strength = splat.strength;
    htmlui::EffectParamDef colorDef;
    colorDef.type = htmlui::EffectParamType::Color;
    htmlui::EffectValue color;
    std::string error;
    if (!splat.color.empty()){
        if (htmlui::EffectValueParser::parse(colorDef, splat.color, color, error) && !color.isVar && !color.currentColor){
            data.color[0] = color.color.x;
            data.color[1] = color.color.y;
            data.color[2] = color.color.z;
            data.color[3] = color.color.w;
        }else{
            Log::warn("[HTML UI] effectSplat: '%s' is not a colour (%s); white is used", splat.color.c_str(), error.c_str());
        }
    }
    return htmlui::EffectRuntime::get().splat(element, data, splat.effect,
                                              splat.time >= 0.0 ? std::optional<double>(splat.time) : std::nullopt);
}

int UIElement::effectPointerPath(const std::vector<Vector3>& path, const std::string& effect){
    HTMLUI_ELEMENT(0)
    std::vector<Rml::Vector3f> points;
    points.reserve(path.size());
    for (const Vector3& p : path)
        points.emplace_back(p.x, p.y, p.z);
    return htmlui::EffectRuntime::get().setPointerPath(element, std::move(points), effect);
}

int UIElement::effectClearSplats(const std::string& effect){
    HTMLUI_ELEMENT(0)
    return htmlui::EffectRuntime::get().clearSplats(element, effect);
}

int UIElement::effectReset(const std::string& effect){
    HTMLUI_ELEMENT(0)
    return htmlui::EffectRuntime::get().resetSimulations(element, effect);
}

void UIElement::setCssText(const std::string& css){
    HTMLUI_ELEMENT()
    std::vector<Rml::PropertyId> ids;
    for (const auto& pair : element->GetLocalStyleProperties())
        ids.push_back(pair.first);
    for (Rml::PropertyId id : ids)
        element->RemoveProperty(id);
    const auto declarations = isCompatElement(element) ? LuaHooks::rewriteDeclarations(css) : splitDeclarations(css);
    for (const auto& declaration : declarations)
        setStyleProperty(declaration.first, declaration.second);
}
