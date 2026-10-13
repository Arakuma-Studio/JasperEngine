#include "ElementDefinition.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "Cascade.h"
#include "StyleSheetNode.h"

namespace Rml {

ElementDefinition::ElementDefinition(const Vector<const StyleSheetNode*>& style_sheet_nodes)
{
	// Initialises the element definition from the list of style sheet nodes.
	for (size_t i = 0; i < style_sheet_nodes.size(); ++i)
	{
		const PropertyDictionary& node_properties = style_sheet_nodes[i]->GetProperties();
		properties.Merge(node_properties);

		// The user-agent declarations by themselves, for 'revert': the most specific of each.
		if (const PropertyDictionary* node_user_agent_properties = style_sheet_nodes[i]->GetUserAgentProperties())
		{
			if (!user_agent_properties)
				user_agent_properties = MakeUnique<PropertyDictionary>();
			user_agent_properties->Merge(*node_user_agent_properties);
		}
	}

	for (auto& property : properties.GetProperties())
		property_ids.Insert(property.first);

	has_important = Cascade::HasImportantDeclarations(properties);
}

const Property* ElementDefinition::GetProperty(PropertyId id) const
{
	return properties.GetProperty(id);
}

const Property* ElementDefinition::GetCustomProperty(const String& name) const
{
	return properties.GetCustomProperty(name);
}

const Property* ElementDefinition::GetUserAgentProperty(PropertyId id) const
{
	return user_agent_properties ? user_agent_properties->GetProperty(id) : nullptr;
}

const Property* ElementDefinition::GetUserAgentCustomProperty(const String& name) const
{
	return user_agent_properties ? user_agent_properties->GetCustomProperty(name) : nullptr;
}

const PropertyIdSet& ElementDefinition::GetPropertyIds() const
{
	return property_ids;
}

} // namespace Rml
