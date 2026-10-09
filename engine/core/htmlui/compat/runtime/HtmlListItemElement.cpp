// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlListItemElement.h"

#include "htmlui/compat/runtime/CompatProperties.h"
#include "htmlui/compat/runtime/ListMarkers.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/XMLParser.h>

#include <algorithm>
#include <cstdlib>

using namespace jasperengine::htmlui::compat;

namespace {

    // Bullets and disclosure triangles are drawn as shapes, as browsers draw them: the
    // built-in fonts have no glyphs for most of them (U+25E6, U+25AA, U+25B8, U+25BE).
    // An SVG in a 10 x 10 view box (currentColor: the marker's color) and its size in em.
    struct MarkerShape {
        const char* text;      // the ListMarkers text it replaces
        const char* markup;
        float size;
    };

    const MarkerShape kShapes[] = {
        {"\xE2\x80\xA2 ", "<circle cx='5' cy='5' r='5' fill='currentColor'/>", 0.35f},
        {"\xE2\x97\xA6 ", "<circle cx='5' cy='5' r='4.2' fill='none' stroke='currentColor' stroke-width='1.6'/>", 0.35f},
        {"\xE2\x96\xAA ", "<rect width='10' height='10' fill='currentColor'/>", 0.35f},
        {"\xE2\x96\xB8 ", "<path d='M1.5 0L9.5 5L1.5 10Z' fill='currentColor'/>", 0.55f},
        {"\xE2\x96\xBE ", "<path d='M0 1.5H10L5 9.5Z' fill='currentColor'/>", 0.55f},
    };

    const MarkerShape* shapeFor(const std::string& text){
        for (const MarkerShape& shape : kShapes){
            if (text == shape.text)
                return &shape;
        }
        return nullptr;
    }

    bool parseInt(const std::string& text, int& value){
        if (text.empty())
            return false;
        char* end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str())
            return false;
        value = (int)std::max(-2147483647L, std::min(2147483647L, parsed));
        return true;
    }

}

RMLUI_RTTI_Define(HtmlListItemElement)

HtmlListItemElement::HtmlListItemElement(const Rml::String& tag): Rml::Element(tag){
    Rml::ElementPtr markerElement = Rml::Factory::InstanceElement(this, "marker", "marker", Rml::XMLAttributes());
    if (!markerElement)
        return;
    // laid out by OnLayout, not by the item's formatting context (non-DOM children are not in it)
    markerElement->SetProperty("display", "none");
    markerElement->SetProperty("white-space", "nowrap");
    Rml::ElementPtr text = Rml::Factory::InstanceElement(markerElement.get(), "#text", "#text", Rml::XMLAttributes());
    if (text)
        markerTextElement = rmlui_dynamic_cast<Rml::ElementText*>(markerElement->AppendChild(std::move(text)));
    marker = AppendChild(std::move(markerElement), false);
}

HtmlListItemElement::~HtmlListItemElement() = default;

int HtmlListItemElement::ordinal(){
    Rml::Element* list = GetParentNode();
    if (!list)
        return 1;
    const bool reversed = list->GetTagName() == "ol" && list->HasAttribute("reversed");
    const int step = reversed ? -1 : 1;

    std::vector<Rml::Element*> items;
    for (int i = 0; i < list->GetNumChildren(); i++){
        Rml::Element* child = list->GetChild(i);
        if (child->GetTagName() == "li")
            items.push_back(child);
    }
    int counter = reversed ? (int)items.size() : 1;
    parseInt(list->GetAttribute<Rml::String>("start", ""), counter);
    for (Rml::Element* item : items){
        int value = 0;
        if (parseInt(item->GetAttribute<Rml::String>("value", ""), value))
            counter = value;
        if (item == this)
            return counter;
        counter += step;
    }
    return counter;
}

std::string HtmlListItemElement::markerTextFor(const std::string& listStyleType){
    return ListMarkers::text(listStyleType.empty() ? std::string("disc") : listStyleType, ordinal());
}

void HtmlListItemElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    // a new value renumbers this item and the ones after it
    if (changed_attributes.count("value")){
        if (Rml::Element* list = GetParentNode()){
            for (int i = 0; i < list->GetNumChildren(); i++){
                if (auto* item = rmlui_dynamic_cast<HtmlListItemElement*>(list->GetChild(i)))
                    item->DirtyLayout();
            }
        }
        DirtyLayout();
    }
}

void HtmlListItemElement::OnLayout(){
    Rml::Element::OnLayout();
    layoutMarker();
}

void HtmlListItemElement::layoutMarker(){
    if (!marker || !markerTextElement)
        return;

    // markers belong to list items: an item made flex, grid, inline or a table has none
    // (the compat rewrite turns display: list-item into block)
    const Rml::Style::Display display = GetComputedValues().display();
    const bool listItem = display == Rml::Style::Display::Block || display == Rml::Style::Display::FlowRoot;

    std::string image = CompatProperties::text(*this, CompatProperty::ListStyleImage);
    if (image == "none")
        image.clear();
    const std::string type = CompatProperties::text(*this, CompatProperty::ListStyleType);
    const bool inside = CompatProperties::text(*this, CompatProperty::ListStylePosition) == "inside";
    std::string text = listItem ? markerTextFor(type) : std::string();
    if (!listItem)
        image.clear();
    if (!image.empty())
        text = " ";
    markerText = text;
    // an image or a shape marker is the image or shape and a space
    const MarkerShape* shape = image.empty() ? shapeFor(text) : nullptr;
    const std::string shown = shape ? std::string(" ") : text;
    if (shown != shownText){
        shownText = shown;
        markerTextElement->SetText(shown);
    }
    const std::string shapeMarkup = shape ? shape->markup : "";
    if (shapeMarkup != markerShapeMarkup){
        markerShapeMarkup = shapeMarkup;
        if (markerShape){
            marker->RemoveChild(markerShape);
            markerShape = nullptr;
        }
        if (shape){
            Rml::XMLAttributes attributes;
            attributes["viewBox"] = Rml::Variant(Rml::String("0 0 10 10"));
            Rml::ElementPtr svg = Rml::Factory::InstanceElement(marker, "svg", "svg", attributes);
            if (svg){
                svg->SetProperty("display", "inline-block");
                svg->SetProperty("vertical-align", "middle");
                markerShape = marker->InsertBefore(std::move(svg), markerTextElement);
                markerShapeSize = shape->size;
                markerShape->SetInnerRML(shape->markup);
                markerShapeSide = -1.0f;
            }
        }
    }
    if (markerShape){
        // the size follows the item's font
        const float side = markerShapeSize * GetComputedValues().font_size();
        if (side != markerShapeSide){
            markerShapeSide = side;
            markerShape->SetProperty(Rml::PropertyId::Width, Rml::Property(side, Rml::Unit::PX));
            markerShape->SetProperty(Rml::PropertyId::Height, Rml::Property(side, Rml::Unit::PX));
        }
    }
    if (image != markerImageSource){
        markerImageSource = image;
        if (markerImage){
            marker->RemoveChild(markerImage);
            markerImage = nullptr;
        }
        if (!image.empty()){
            Rml::XMLAttributes attributes;
            attributes["src"] = Rml::Variant(image);
            Rml::ElementPtr img = Rml::Factory::InstanceElement(marker, "img", "img", attributes);
            if (img)
                markerImage = marker->InsertBefore(std::move(img), markerTextElement);
        }
    }

    const bool visible = !markerText.empty() && listItem && (markerText != " " || markerImage);
    if (visible != markerVisible){
        markerVisible = visible;
        marker->SetProperty("display", visible ? "block" : "none");
    }

    float width = 0.0f;
    if (visible){
        width = (float)Rml::ElementUtilities::GetStringWidth(markerTextElement, shownText);
        if (markerShape)
            width += markerShapeSize * GetComputedValues().font_size();
        if (markerImage){
            Rml::Vector2f size;
            float ratio = 0.0f;
            if (markerImage->GetIntrinsicDimensions(size, ratio))
                width += size.x;
        }
    }

    // list-style-position: inside moves the item's content right by the marker
    const float shift = (visible && inside) ? width : 0.0f;
    if (shift != insideShift){
        const float base = GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left) - insideShift;
        insideShift = shift;
        if (shift > 0.0f)
            SetProperty(Rml::PropertyId::PaddingLeft, Rml::Property(base + shift, Rml::Unit::PX));
        else
            RemoveProperty(Rml::PropertyId::PaddingLeft);
    }

    if (!visible)
        return;

    // formatted at its own width, its right edge at the content box, its top at the first line
    const Rml::Vector2f content = GetBox().GetSize(Rml::BoxArea::Content);
    Rml::ElementUtilities::FormatElement(marker, Rml::Vector2f(std::max(0.0f, width), std::max(0.0f, content.y)));
    const Rml::Vector2f contentOffset = GetBox().GetPosition(Rml::BoxArea::Content);
    marker->SetOffset(Rml::Vector2f(contentOffset.x - width, contentOffset.y), this);
}
