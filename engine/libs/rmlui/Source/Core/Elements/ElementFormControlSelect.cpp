#include "../../../Include/RmlUi/Core/Elements/ElementFormControlSelect.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"
#include "../../../Include/RmlUi/Core/ElementText.h"
#include "../../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../../Include/RmlUi/Core/Core.h"
#include "../../../Include/RmlUi/Core/Event.h"
#include "../../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../../Include/RmlUi/Core/FontMetrics.h"
#include "../../../Include/RmlUi/Core/Math.h"
#include "../../../Include/RmlUi/Core/StringUtilities.h"
#include "WidgetDropDown.h"

namespace Rml {

RMLUI_RTTI_Define(ElementFormControlSelect)

ElementFormControlSelect::ElementFormControlSelect(const String& tag) : ElementFormControl(tag), widget(nullptr)
{
	widget = new WidgetDropDown(this);
}

ElementFormControlSelect::~ElementFormControlSelect()
{
	delete widget;
}

String ElementFormControlSelect::GetValue() const
{
	return GetAttribute("value", String());
}

void ElementFormControlSelect::SetValue(const String& value)
{
	MoveChildren();

	SetAttribute("value", value);
}

void ElementFormControlSelect::SetSelection(int selection)
{
	MoveChildren();

	widget->SetSelection(widget->GetOption(selection));
}

int ElementFormControlSelect::GetSelection() const
{
	return widget->GetSelection();
}

Element* ElementFormControlSelect::GetOption(int index)
{
	MoveChildren();

	return widget->GetOption(index);
}

int ElementFormControlSelect::GetNumOptions()
{
	MoveChildren();

	return widget->GetNumOptions();
}

int ElementFormControlSelect::Add(const String& rml, const String& value, int before, bool selectable)
{
	MoveChildren();

	return widget->AddOption(rml, value, before, false, selectable);
}

int ElementFormControlSelect::Add(ElementPtr element, int before)
{
	MoveChildren();

	return widget->AddOption(std::move(element), before);
}

void ElementFormControlSelect::Remove(int index)
{
	MoveChildren();

	widget->RemoveOption(index);
}

void ElementFormControlSelect::RemoveAll()
{
	MoveChildren();

	widget->ClearOptions();
}

void ElementFormControlSelect::ShowSelectBox()
{
	widget->ShowSelectBox();
}

void ElementFormControlSelect::HideSelectBox()
{
	widget->HideSelectBox();
}
void ElementFormControlSelect::CancelSelectBox()
{
	widget->CancelSelectBox();
}

bool ElementFormControlSelect::IsSelectBoxVisible()
{
	return widget->IsSelectBoxVisible();
}

void ElementFormControlSelect::OnUpdate()
{
	ElementFormControl::OnUpdate();

	MoveChildren();

	widget->OnUpdate();
}

void ElementFormControlSelect::OnRender()
{
	ElementFormControl::OnRender();

	widget->OnRender();
}

void ElementFormControlSelect::OnLayout()
{
	widget->OnLayout();
}

void ElementFormControlSelect::OnChildAdd(Element* child)
{
	if (widget)
		widget->OnChildAdd(child);
}

void ElementFormControlSelect::OnChildRemove(Element* child)
{
	if (widget)
		widget->OnChildRemove(child);
}

void ElementFormControlSelect::MoveChildren()
{
	// Move any child elements into the widget (except for the three functional elements).
	while (Element* raw_child = GetFirstChild())
	{
		ElementPtr child = RemoveChild(raw_child);
		widget->AddOption(std::move(child), -1);
	}
}

bool ElementFormControlSelect::GetIntrinsicDimensions(Vector2f& intrinsic_dimensions, float& /*ratio*/)
{
	// As wide as its widest option's text plus the arrow and the value's horizontal edges, as browsers size a select (128
	// dp without options).
	float widest = -1.f;
	const int num_options = GetNumOptions();
	for (int i = 0; i < num_options; i++)
	{
		Element* option = GetOption(i);
		if (!option)
			continue;
		String text;
		option->GetInnerRML(text);
		widest = Math::Max(widest, (float)ElementUtilities::GetStringWidth(this, StringUtilities::DecodeRml(text)));
	}
	if (widest < 0.f)
	{
		intrinsic_dimensions.x = 128 * ElementUtilities::GetDensityIndependentPixelRatio(this);
	}
	else
	{
		float edges = 0.f;
		for (int i = 0; i < GetNumChildren(true); i++)
		{
			Element* child = GetChild(i);
			const ComputedValues& computed = child->GetComputedValues();
			const String& tag = child->GetTagName();
			if (tag != "selectarrow" && tag != "selectvalue")
				continue;
			edges += computed.padding_left().value + computed.padding_right().value + computed.border_left_width() + computed.border_right_width() +
				computed.margin_left().value + computed.margin_right().value;
			if (tag == "selectarrow" && computed.width().type == Style::Width::Length)
				edges += computed.width().value;
		}
		intrinsic_dimensions.x = Math::RoundUp(widest + edges);
	}
	intrinsic_dimensions.y = GetLineHeight();

	return true;
}

float ElementFormControlSelect::GetBaseline() const
{
	ElementFormControlSelect* self = const_cast<ElementFormControlSelect*>(this);
	const FontFaceHandle font = GetFontFaceHandle();
	if (!font)
		return ElementFormControl::GetBaseline();
	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(font);
	const Box& box = self->GetBox();
	const float content = box.GetSize(BoxArea::Content).y;
	const float line = self->GetLineHeight();
	const float baseline_from_top = (content - line) * 0.5f + (line - (metrics.ascent + metrics.descent)) * 0.5f + metrics.ascent;
	return box.GetEdge(BoxArea::Margin, BoxEdge::Bottom) + box.GetEdge(BoxArea::Border, BoxEdge::Bottom) +
		box.GetEdge(BoxArea::Padding, BoxEdge::Bottom) + (content - baseline_from_top);
}

void ElementFormControlSelect::OnAttributeChange(const ElementAttributes& changed_attributes)
{
	RMLUI_ASSERT(widget);

	ElementFormControl::OnAttributeChange(changed_attributes);

	auto it = changed_attributes.find("value");
	if (it != changed_attributes.end())
		widget->OnValueChange(it->second.Get<String>());
}

} // namespace Rml
