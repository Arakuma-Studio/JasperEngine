// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIElement.h"
#include "UIElementInternal.h"

#include "Engine.h"
#include "Log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

using namespace jasperengine;

namespace {

    void appendText(Rml::Element* element, std::string& out){
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
            out += text->GetText();
            return;
        }
        for (int i = 0; i < element->GetNumChildren(); i++){
            appendText(element->GetChild(i), out);
        }
    }

}

UIElement::UIElement(){
}

UIElement UIElement::wrap(Rml::Element* element){
    UIElement result;
    if (element){
        result.handle = std::make_shared<UIElementHandle>();
        result.handle->element = element->GetObserverPtr();
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
    Rml::ElementDocument* document = element->GetOwnerDocument();
    if (!document || !document->GetParentNode() || !document->GetContext())
        return nullptr;

    return element;
}

bool UIElement::isValid() const{
    return get() != nullptr;
}

std::string UIElement::getId() const{
    Rml::Element* element = get();
    return element ? element->GetId() : std::string();
}

std::string UIElement::getTagName() const{
    Rml::Element* element = get();
    return element ? element->GetTagName() : std::string();
}

std::string UIElement::getTextContent() const{
    std::string text;
    if (Rml::Element* element = get()){
        appendText(element, text);
    }
    return text;
}

void UIElement::setTextContent(const std::string& text){
    Rml::Element* element = get();
    if (!element)
        return;

    while (element->GetNumChildren() > 0){
        element->RemoveChild(element->GetFirstChild());
    }

    // a text node takes the string as it is: no markup or entity is parsed
    if (!text.empty()){
        if (Rml::ElementPtr node = element->GetOwnerDocument()->CreateTextNode(text)){
            element->AppendChild(std::move(node));
        }
    }
}

std::string UIElement::getInnerHTML() const{
    Rml::Element* element = get();
    return element ? element->GetInnerRML() : std::string();
}

void UIElement::setInnerHTML(const std::string& markup){
    if (Rml::Element* element = get()){
        element->SetInnerRML(markup);
    }
}

bool UIElement::hasAttribute(const std::string& name) const{
    Rml::Element* element = get();
    return element && element->HasAttribute(name);
}

std::string UIElement::getAttribute(const std::string& name) const{
    Rml::Element* element = get();
    return element ? element->GetAttribute<Rml::String>(name, "") : std::string();
}

void UIElement::setAttribute(const std::string& name, const std::string& value){
    if (Rml::Element* element = get()){
        element->SetAttribute(name, value);
    }
}

void UIElement::removeAttribute(const std::string& name){
    if (Rml::Element* element = get()){
        element->RemoveAttribute(name);
    }
}

bool UIElement::hasClass(const std::string& name) const{
    Rml::Element* element = get();
    return element && element->IsClassSet(name);
}

void UIElement::setClass(const std::string& name, bool active){
    if (Rml::Element* element = get()){
        element->SetClass(name, active);
    }
}

bool UIElement::setStyleProperty(const std::string& name, const std::string& value){
    Rml::Element* element = get();
    if (!element)
        return false;

    // parsed here first, so a rejected declaration gives one warning naming the element
    Rml::PropertyDictionary properties;
    if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(properties, name, value) || !element->SetProperty(name, value)){
        Log::warn("[HTML UI] style '%s: %s' rejected on element %s", name.c_str(), value.c_str(), element->GetAddress().c_str());
        return false;
    }

    return true;
}

void UIElement::removeStyleProperty(const std::string& name){
    if (Rml::Element* element = get()){
        element->RemoveProperty(name);
    }
}

Rect UIElement::getBoundingBox() const{
    Rml::Element* element = get();
    if (!element)
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
    Rml::Element* element = get();
    return element && element->IsPseudoClassSet(pseudoClass);
}
