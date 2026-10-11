#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/ID.h"
#include "Cascade.h"

namespace Rml {

PropertyDictionary::PropertyDictionary() {}

void PropertyDictionary::SetProperty(PropertyId id, const Property& property)
{
	RMLUI_ASSERT(id != PropertyId::Invalid);
	properties[id] = property;
	has_important |= property.important;
}

void PropertyDictionary::RemoveProperty(PropertyId id)
{
	RMLUI_ASSERT(id != PropertyId::Invalid);
	properties.erase(id);
}

const Property* PropertyDictionary::GetProperty(PropertyId id) const
{
	PropertyMap::const_iterator iterator = properties.find(id);
	if (iterator == properties.end())
		return nullptr;

	return &(*iterator).second;
}

void PropertyDictionary::SetCustomProperty(String name, Property property)
{
	has_important |= property.important;
	custom_properties.insert_or_assign(std::move(name), std::move(property));
}

bool PropertyDictionary::RemoveCustomProperty(const String& name)
{
	return custom_properties.erase(name) > 0;
}

const Property* PropertyDictionary::GetCustomProperty(const String& name) const
{
	auto it = custom_properties.find(name);
	if (it == custom_properties.end())
		return nullptr;

	return &it->second;
}

void PropertyDictionary::SetVarShorthand(ShorthandId id, Property property)
{
	RMLUI_ASSERT(property.unit == Unit::VAR_EXPRESSION);
	has_important |= property.important;
	var_shorthands.insert_or_assign(id, std::move(property));
}

bool PropertyDictionary::RemoveVarShorthand(ShorthandId id)
{
	return var_shorthands.erase(id) > 0;
}

const Property* PropertyDictionary::GetVarShorthand(ShorthandId id) const
{
	auto it = var_shorthands.find(id);
	if (it == var_shorthands.end())
		return nullptr;

	return &it->second;
}

const UnorderedMap<ShorthandId, Property>& PropertyDictionary::GetVarShorthands() const
{
	return var_shorthands;
}

bool PropertyDictionary::Empty() const
{
	return properties.empty() && custom_properties.empty() && var_shorthands.empty();
}

int PropertyDictionary::GetNumProperties() const
{
	return (int)properties.size();
}

const PropertyMap& PropertyDictionary::GetProperties() const
{
	return properties;
}

const UnorderedMap<String, Property>& PropertyDictionary::GetCustomProperties() const
{
	return custom_properties;
}

void PropertyDictionary::Import(const PropertyDictionary& other, int property_specificity)
{
	// A forced specificity is the selector's: important declarations rank above every normal one (Cascade.h).
	auto Specificity = [property_specificity](const Property& property) {
		if (property_specificity <= 0)
			return property.specificity;
		return property_specificity + (property.important ? Cascade::ImportantOffset : 0);
	};

	for (const auto& [id, property] : other.properties)
		SetProperty(id, property, Specificity(property));

	for (const auto& [name, property] : other.custom_properties)
		SetCustomProperty(name, property, Specificity(property));

	for (const auto& [id, property] : other.var_shorthands)
		SetVarShorthand(id, property, Specificity(property));
}

void PropertyDictionary::Merge(const PropertyDictionary& other, int specificity_offset)
{
	for (const auto& [id, property] : other.properties)
		SetProperty(id, property, property.specificity + specificity_offset);

	for (const auto& [name, property] : other.custom_properties)
		SetCustomProperty(name, property, property.specificity + specificity_offset);

	for (const auto& [id, property] : other.var_shorthands)
		SetVarShorthand(id, property, property.specificity + specificity_offset);
}

void PropertyDictionary::Clear()
{
	properties.clear();
	custom_properties.clear();
	var_shorthands.clear();
	has_important = false;
}

void PropertyDictionary::SetSourceOfAllProperties(const SharedPtr<const PropertySource>& property_source)
{
	for (auto& p : properties)
		p.second.source = property_source;
	for (auto& p : custom_properties)
		p.second.source = property_source;
	for (auto& p : var_shorthands)
		p.second.source = property_source;
}

void PropertyDictionary::SetOriginOfAllProperties(StyleOrigin origin)
{
	for (auto& p : properties)
		p.second.origin = origin;
	for (auto& p : custom_properties)
		p.second.origin = origin;
	for (auto& p : var_shorthands)
		p.second.origin = origin;
}

void PropertyDictionary::SetProperty(PropertyId id, const Property& property, int specificity)
{
	PropertyMap::iterator iterator = properties.find(id);
	if (iterator != properties.end() && iterator->second.specificity > specificity)
		return;

	Property& new_property = (properties[id] = property);
	new_property.specificity = specificity;
	has_important |= property.important;
}

void PropertyDictionary::SetCustomProperty(const String& name, const Property& property, int specificity)
{
	const auto it = custom_properties.find(name);
	if (it != custom_properties.end() && it->second.specificity > specificity)
		return;

	Property& new_property = custom_properties.insert_or_assign(name, property).first->second;
	new_property.specificity = specificity;
	has_important |= property.important;
}

void PropertyDictionary::SetVarShorthand(ShorthandId id, const Property& property, int specificity)
{
	RMLUI_ASSERT(id != ShorthandId::Invalid);
	const auto it = var_shorthands.find(id);
	if (it != var_shorthands.end() && it->second.specificity > specificity)
		return;

	Property& new_property = var_shorthands.insert_or_assign(id, property).first->second;
	new_property.specificity = specificity;
	has_important |= property.important;
}

} // namespace Rml
