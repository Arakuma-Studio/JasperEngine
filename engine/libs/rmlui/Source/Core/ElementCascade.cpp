#include "ElementCascade.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "StyleSheetSelector.h"

namespace Rml {

static bool AnyNameWithPrefix(const StyleSheetIndex::HasDependencies& has, char prefix)
{
	for (const String& name : has.names)
	{
		if (!name.empty() && name[0] == prefix)
			return true;
	}
	return false;
}

static bool IsRelevant(const StyleSheetIndex::HasDependencies& has, ElementCascade::Change change, const String& name)
{
	using Change = ElementCascade::Change;
	switch (change)
	{
	case Change::Structure: return true;
	case Change::Class: return has.names.count("." + name) != 0;
	case Change::PseudoClass: return has.names.count(":" + name) != 0;
	case Change::Attribute:
		if (name == "class")
			return AnyNameWithPrefix(has, '.');
		if (name == "id")
			return AnyNameWithPrefix(has, '#') || has.names.count("[id") != 0;
		return has.names.count("[" + name) != 0;
	}
	return false;
}

void ElementCascade::MarkHasSubject(Element* element, bool in_non_subject)
{
	// A :has() in a compound that is not the subject of its selector affects other elements than the one it matches:
	// re-match the element's subtree and siblings. Otherwise only the element itself.
	if (in_non_subject)
		DirtyDefinition(element, true);
	else
		element->dirty_definition_self = true;
}

void ElementCascade::OnHasDependencyChange(Element* element, Change change, const String& name)
{
	const StyleSheet* style_sheet = element->GetStyleSheet();
	if (!style_sheet)
		return;
	const StyleSheetIndex::HasDependencies& has = style_sheet->GetHasDependencies();
	if (!has.used || !IsRelevant(has, change, name))
		return;

	for (Element* ancestor = element; ancestor; ancestor = ancestor->GetParentNode())
	{
		if (ancestor != element || change == Change::Structure)
			MarkHasSubject(ancestor, has.in_non_subject);

		if (!has.sibling_relative)
			continue;
		if (Element* parent = ancestor->GetParentNode())
		{
			const int num_children = parent->GetNumChildren();
			for (int i = 0; i < num_children; i++)
			{
				Element* sibling = parent->GetChild(i);
				if (sibling == ancestor)
					break;
				MarkHasSubject(sibling, has.in_non_subject);
			}
		}
	}
}

} // namespace Rml
