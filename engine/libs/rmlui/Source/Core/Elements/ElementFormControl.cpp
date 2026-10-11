#include "../../../Include/RmlUi/Core/Elements/ElementFormControl.h"
#include "../../../Include/RmlUi/Core/ComputedValues.h"

namespace Rml {

RMLUI_RTTI_Define(ElementFormControl)

ElementFormControl::ElementFormControl(const String& tag) : Element(tag)
{
	SetProperty(PropertyId::TabIndex, Property(Style::TabIndex::Auto));
}

ElementFormControl::~ElementFormControl() {}

String ElementFormControl::GetName() const
{
	return GetAttribute<String>("name", "");
}

void ElementFormControl::SetName(const String& name)
{
	SetAttribute("name", name);
}

bool ElementFormControl::IsSubmitted()
{
	return true;
}

bool ElementFormControl::IsDisabled() const
{
	return HasAttribute("disabled");
}

void ElementFormControl::SetDisabled(bool disable)
{
	if (disable)
		SetAttribute("disabled", "");
	else
		RemoveAttribute("disabled");
}

void ElementFormControl::OnAttributeChange(const ElementAttributes& changed_attributes)
{
	Element::OnAttributeChange(changed_attributes);

	if (changed_attributes.find("disabled") != changed_attributes.end())
	{
		bool is_disabled = IsDisabled();
		SetPseudoClass("disabled", is_disabled);

		// A disabled control cannot take the focus, which also keeps click events (when originating from user inputs,
		// see Context) from reaching it. Its inline style is left alone (phase 3): the focus code reads :disabled.
		if (is_disabled)
			Blur();
	}
}

} // namespace Rml
