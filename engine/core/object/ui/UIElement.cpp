// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIElement.h"
#include "UIElementInternal.h"
#include "UIEvent.h"
#include "HtmlUIEventListener.h"
#include "HtmlUIScriptInternal.h"

#include "Engine.h"
#include "Log.h"

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

    bool isElementNode(Rml::Element* element){
        return element && !rmlui_dynamic_cast<Rml::ElementText*>(element);
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
    return element->GetInnerRML();
}

void UIElement::setInnerHTML(const std::string& markup){
    HTMLUI_ELEMENT()
    element->SetInnerRML(markup);
}

bool UIElement::hasAttribute(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    return element->HasAttribute(name);
}

std::string UIElement::getAttribute(const std::string& name) const{
    HTMLUI_ELEMENT(std::string())
    return element->GetAttribute<Rml::String>(name, "");
}

void UIElement::setAttribute(const std::string& name, const std::string& value){
    HTMLUI_ELEMENT()
    element->SetAttribute(name, value);
}

void UIElement::removeAttribute(const std::string& name){
    HTMLUI_ELEMENT()
    element->RemoveAttribute(name);
}

bool UIElement::hasClass(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    return element->IsClassSet(name);
}

void UIElement::setClass(const std::string& name, bool active){
    HTMLUI_ELEMENT()
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
    return element->GetClassNames();
}

void UIElement::setClassName(const std::string& className){
    HTMLUI_ELEMENT()
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
    return wrap(element->QuerySelector(selector));
}

std::vector<UIElement> UIElement::querySelectorAll(const std::string& selector) const{
    HTMLUI_ELEMENT(std::vector<UIElement>())
    Rml::ElementList elements;
    element->QuerySelectorAll(elements, selector);
    std::vector<UIElement> result;
    result.reserve(elements.size());
    for (Rml::Element* found : elements){
        result.push_back(wrap(found));
    }
    return result;
}

UIElement UIElement::closest(const std::string& selector) const{
    HTMLUI_ELEMENT(UIElement())
    // the DOM's closest starts at the element itself, RmlUi's at its parent
    if (element->Matches(selector))
        return wrap(element);
    return wrap(element->Closest(selector));
}

bool UIElement::matches(const std::string& selector) const{
    HTMLUI_ELEMENT(false)
    return element->Matches(selector);
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
    std::map<std::string, std::string> attributes;
    for (const auto& pair : element->GetAttributes()){
        attributes[pair.first] = pair.second.Get<Rml::String>();
    }
    return attributes;
}

// ---- classList / style / dataset ----

void UIElement::addClass(const std::string& name){
    HTMLUI_ELEMENT()
    element->SetClass(name, true);
}

void UIElement::removeClass(const std::string& name){
    HTMLUI_ELEMENT()
    element->SetClass(name, false);
}

bool UIElement::toggleClass(const std::string& name){
    HTMLUI_ELEMENT(false)
    const bool active = !element->IsClassSet(name);
    element->SetClass(name, active);
    return active;
}

bool UIElement::toggleClass(const std::string& name, bool force){
    HTMLUI_ELEMENT(false)
    element->SetClass(name, force);
    return force;
}

bool UIElement::containsClass(const std::string& name) const{
    HTMLUI_ELEMENT(false)
    return element->IsClassSet(name);
}

std::string UIElement::getStyle(const std::string& property) const{
    HTMLUI_ELEMENT(std::string())
    const Rml::Property* value = element->GetProperty(property);
    return value ? value->ToString() : std::string();
}

void UIElement::setStyle(const std::string& property, const std::string& value){
    HTMLUI_ELEMENT()
    if (value.empty()){
        element->RemoveProperty(property);
    }else{
        setStyleProperty(property, value);
    }
}

void UIElement::removeStyle(const std::string& property){
    HTMLUI_ELEMENT()
    element->RemoveProperty(property);
}

std::string UIElement::getDataset(const std::string& camelKey) const{
    HTMLUI_ELEMENT(std::string())
    return element->GetAttribute<Rml::String>("data-" + hyphenate(camelKey), "");
}

void UIElement::setDataset(const std::string& camelKey, const std::string& value){
    HTMLUI_ELEMENT()
    element->SetAttribute("data-" + hyphenate(camelKey), value);
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
    return element->GetScrollTop();
}

void UIElement::setScrollTop(float value){
    HTMLUI_ELEMENT()
    element->SetScrollTop(value);
}

float UIElement::getScrollLeft() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetScrollLeft();
}

void UIElement::setScrollLeft(float value){
    HTMLUI_ELEMENT()
    element->SetScrollLeft(value);
}

float UIElement::getScrollWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetScrollWidth();
}

float UIElement::getScrollHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetScrollHeight();
}

float UIElement::getClientWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetClientWidth();
}

float UIElement::getClientHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetClientHeight();
}

float UIElement::getOffsetWidth() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetOffsetWidth();
}

float UIElement::getOffsetHeight() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetOffsetHeight();
}

float UIElement::getOffsetLeft() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetOffsetLeft();
}

float UIElement::getOffsetTop() const{
    HTMLUI_ELEMENT(0.0f)
    return element->GetOffsetTop();
}

UIRect UIElement::getBoundingClientRect() const{
    HTMLUI_ELEMENT((UIRect{0, 0, 0, 0, 0, 0, 0, 0}))

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
