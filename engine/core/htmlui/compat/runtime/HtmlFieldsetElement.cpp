// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlFieldsetElement.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/StyleTypes.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine::htmlui::compat;

RMLUI_RTTI_Define(HtmlFieldsetElement)

namespace {

    const char* const kLegendClass = "htmlui-legend";

    // the horizontal extent of what an element holds (text lines and element boxes), in px
    // from `origin`
    void contentExtent(Rml::Element* element, float originX, float& left, float& right){
        for (int i = 0; i < element->GetNumChildren(); i++){
            Rml::Element* child = element->GetChild(i);
            if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(child)){
                const float base = text->GetAbsoluteOffset(Rml::BoxArea::Border).x - originX;
                for (const Rml::ElementText::Line& line : text->GetLines()){
                    // (a line knows its width once it was drawn)
                    const float width = (line.width > 0.0f) ? line.width : (float)Rml::ElementUtilities::GetStringWidth(text, line.text);
                    left = std::min(left, base + line.position.x);
                    right = std::max(right, base + line.position.x + width);
                }
                continue;
            }
            if (child->GetDisplay() == Rml::Style::Display::None)
                continue;
            const float base = child->GetAbsoluteOffset(Rml::BoxArea::Border).x - originX;
            for (int box = 0; box < child->GetNumBoxes(); box++){
                Rml::Vector2f offset;
                const Rml::Box& area = child->GetBox(box, offset);
                const float width = area.GetSize(Rml::BoxArea::Border).x;
                if (width <= 0.0f)
                    continue;
                left = std::min(left, base + offset.x);
                right = std::max(right, base + offset.x + width);
            }
            // an inline element's own box may be empty (its text is laid out in its children)
            contentExtent(child, originX, left, right);
        }
    }

}

HtmlFieldsetElement::HtmlFieldsetElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlFieldsetElement::~HtmlFieldsetElement() = default;

Rml::Element* HtmlFieldsetElement::getRenderedLegend(){
    if (!IsClassSet(kLegendClass))
        return nullptr;
    // the first child, text of white space aside
    Rml::Element* first = nullptr;
    for (int i = 0; i < GetNumChildren() && !first; i++){
        Rml::Element* child = GetChild(i);
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(child)){
            const Rml::String& content = text->GetText();
            if (content.find_first_not_of(" \t\n\r\f") != Rml::String::npos)
                return nullptr;
            continue;
        }
        first = child;
    }
    if (!first || first->GetTagName() != "legend")
        return nullptr;
    const Rml::ComputedValues& computed = first->GetComputedValues();
    if (computed.display() == Rml::Style::Display::None || computed.position() == Rml::Style::Position::Absolute ||
        computed.position() == Rml::Style::Position::Fixed || computed.float_() != Rml::Style::Float::None)
        return nullptr;
    return first;
}

// The legend starts at the top of the fieldset's border box (its top margin takes it back over
// the fieldset's border and padding), and the fieldset's padding-top comes after it.
void HtmlFieldsetElement::placeLegend(){
    Rml::Element* legend = getRenderedLegend();
    if (placed && placed != legend){
        placed->RemoveProperty(Rml::PropertyId::MarginTop);
        placed->RemoveProperty(Rml::PropertyId::MarginBottom);
        placed = nullptr;
    }
    if (!legend)
        return;

    const Rml::ComputedValues& computed = GetComputedValues();
    const float border = (float)computed.border_top_width();
    // a percentage padding is resolved by the layout: taken from the box it made
    const float padding = (computed.padding_top().type == Rml::Style::Padding::Length)
        ? computed.padding_top().value : GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top);
    const float top = -(border + padding);
    const float bottom = padding;
    if (placed == legend && top == placedTop && bottom == placedBottom)
        return;
    placed = legend;
    placedTop = top;
    placedBottom = bottom;
    legend->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(top, Rml::Unit::PX));
    legend->SetProperty(Rml::PropertyId::MarginBottom, Rml::Property(bottom, Rml::Unit::PX));
}

void HtmlFieldsetElement::OnPropertyChange(const Rml::PropertyIdSet& changed_properties){
    Rml::Element::OnPropertyChange(changed_properties);
    if (changed_properties.Contains(Rml::PropertyId::BorderTopWidth) || changed_properties.Contains(Rml::PropertyId::PaddingTop) || !placed)
        placeLegend();
}

void HtmlFieldsetElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    Rml::Element::OnAttributeChange(changed_attributes);
    if (changed_attributes.count("class"))
        placeLegend();
}

void HtmlFieldsetElement::OnChildAdd(Rml::Element* child){
    Rml::Element::OnChildAdd(child);
    if (child && child->GetParentNode() == this)
        placeLegend();
}

void HtmlFieldsetElement::OnChildRemove(Rml::Element* child){
    Rml::Element::OnChildRemove(child);
    if (child == placed){
        // it leaves with its own margins
        placed->RemoveProperty(Rml::PropertyId::MarginTop);
        placed->RemoveProperty(Rml::PropertyId::MarginBottom);
        placed = nullptr;
    }
}

// The legend's box is known: the fieldset is painted again when its cut changed (the fieldset's
// own box may be the same, e.g. after the legend's text changed).
void HtmlFieldsetElement::OnLayout(){
    Rml::Element::OnLayout();
    if (placed != getRenderedLegend())
        placeLegend();
    BorderTopCut cut;
    const bool has = GetBorderTopCut(cut);
    if (has != hadCut || cut.inset != lastCut.inset || cut.gap_begin != lastCut.gap_begin || cut.gap_end != lastCut.gap_end){
        hadCut = has;
        lastCut = cut;
        DirtyBorderTopCut();
    }
}

bool HtmlFieldsetElement::GetBorderTopCut(BorderTopCut& cut){
    Rml::Element* legend = getRenderedLegend();
    if (!legend || legend != placed)
        return false;

    const Rml::Box& legendBox = legend->GetBox();
    const float legendHeight = legendBox.GetSize(Rml::BoxArea::Border).y;
    const float border = GetBox().GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top);
    // the border is centred on the legend
    cut.inset = std::max(0.0f, std::floor((legendHeight - border) * 0.5f));

    const float originX = GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float legendLeft = legend->GetAbsoluteOffset(Rml::BoxArea::Border).x - originX;
    const float legendRight = legendLeft + legendBox.GetSize(Rml::BoxArea::Border).x;
    cut.gap_begin = legendLeft;
    cut.gap_end = legendRight;
    if (legend->GetComputedValues().width().type == Rml::Style::Width::Auto){
        // an auto-width legend fills the fieldset (no fit-content width yet): the gap is as wide
        // as its content plus its own padding and border
        float left = legendRight, right = legendLeft;
        contentExtent(legend, originX, left, right);
        if (right <= left)
            return true;   // an empty legend: its whole box
        const float before = legendBox.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left) + legendBox.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Left);
        const float after = legendBox.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right) + legendBox.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right);
        cut.gap_begin = std::max(legendLeft, std::floor(left - before));
        cut.gap_end = std::min(legendRight, std::ceil(right + after));
    }
    return true;
}
