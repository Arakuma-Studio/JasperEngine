#include "ElementStyle.h"
#include "WillChange.h"
#include "ElementCascade.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Log.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyDictionary.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/TransformPrimitive.h"
#include "Cascade.h"
#include "ComputeProperty.h"
#include "ControlledLifetimeResource.h"
#include "ElementDefinition.h"
#include "LayoutProperties.h"
#include "ElementTransitions.h"
#include "BackgroundLayerValues.h"
#include "PaintProperties.h"
#include "PropertiesIterator.h"
#include "PropertyParserAnimation.h"
#include "PropertyShorthandDefinition.h"
#include "TextProperties.h"
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "Calculation.h"
#endif
#include <algorithm>
#include <optional>

namespace Rml {

// A background-image or mask-image list with an image (phase 3, p3-paint).
static bool HasImageLayers(const Property& property)
{
	if (property.unit != Unit::STYLE_VALUE)
		return false;
	const ImageLayerList* list = rmlui_dynamic_cast<const ImageLayerList*>(property.value.GetReference<StyleValuePtr>().get());
	return list && list->HasImages();
}

struct ElementStyleData {
	const PropertyDictionary empty_property_dictionary;
	Property returned_property_storage;
};
static ControlledLifetimeResource<ElementStyleData> element_style_data;

static PseudoClassState operator|(PseudoClassState lhs, PseudoClassState rhs)
{
	return PseudoClassState(int(lhs) | int(rhs));
}
static PseudoClassState operator&(PseudoClassState lhs, PseudoClassState rhs)
{
	return PseudoClassState(int(lhs) & int(rhs));
}

ElementStyle::ElementStyle(Element* _element)
{
	element = _element;
}

ElementStyle::~ElementStyle() = default;

// The context-relative units a StyleValue property depends on.
static Units GetStyleValueUnitDependencies(const Property& property)
{
	const StyleValuePtr value = property.value.Get<StyleValuePtr>();
	return value ? value->GetUnitDependencies() : Unit::UNKNOWN;
}

#ifdef RMLUI_MATH_EXPRESSIONS
static const Units mutable_unit_mask = Unit::EM | Unit::REM | Unit::VW | Unit::VH | Unit::VMIN | Unit::VMAX | Unit::CH | Unit::EX | Unit::DP_SCALABLE_LENGTH;

Units ElementStyle::GetMutableUnitDependencies(const Property& property) const
{
	if (property.unit == Unit::CALCULATION)
	{
		const CalculationPtr calculation = property.value.Get<CalculationPtr>();
		return calculation ? calculation->GetDependencyMask() & mutable_unit_mask : Unit::UNKNOWN;
	}
	if (property.unit == Unit::STYLE_VALUE)
		return GetStyleValueUnitDependencies(property) & mutable_unit_mask;
	return property.unit & mutable_unit_mask;
}

Units ElementStyle::GetResolvedUnitDependencies(PropertyId id) const
{
	if (!resolved_unit_dependencies)
		return Unit::UNKNOWN;
	const auto it = resolved_unit_dependencies->find(id);
	return it != resolved_unit_dependencies->end() ? it->second : Unit::UNKNOWN;
}

void ElementStyle::UpdateResolvedUnitDependencies(PropertyId id, const Property& specified_property, const Property* resolved_property)
{
	const bool requires_cache = specified_property.unit == Unit::VAR_EXPRESSION || specified_property.unit == Unit::SHORTHAND_PLACEHOLDER;
	const Units dependencies = resolved_property ? GetMutableUnitDependencies(*resolved_property) : Unit::UNKNOWN;
	if (requires_cache && Any(dependencies))
	{
		if (!resolved_unit_dependencies)
			resolved_unit_dependencies = MakeUnique<UnorderedMap<PropertyId, Units>>();
		(*resolved_unit_dependencies)[id] = dependencies;
	}
	else
	{
		EraseResolvedUnitDependencies(id);
	}
}

void ElementStyle::EraseResolvedUnitDependencies(PropertyId id)
{
	if (!resolved_unit_dependencies)
		return;
	resolved_unit_dependencies->erase(id);
	if (resolved_unit_dependencies->empty())
		resolved_unit_dependencies.reset();
}
#endif

const Property* ElementStyle::GetLocalProperty(PropertyId id, const PropertyDictionary& inline_properties, const ElementDefinition* definition,
	const AnimationLayers* animation_layers)
{
	// CSS Cascade 4, strongest first: transitions, important declarations, animations, normal declarations (Cascade.h).
	if (animation_layers)
	{
		if (const Property* property = animation_layers->transition_properties.GetProperty(id))
			return property;
	}

	// An inline declaration beats every normal declaration of the definition, so the definition is only consulted for
	// the property when the inline style has none, or when the definition has important declarations.
	const Property* declared = inline_properties.GetProperty(id);
	if (definition && (!declared || definition->HasImportantProperties()))
		declared = Cascade::Choose(declared, definition->GetProperty(id));

	if (animation_layers && !(declared && declared->important))
	{
		if (const Property* property = animation_layers->animated_properties.GetProperty(id))
			return property;
	}

	return declared;
}

const Property* ElementStyle::GetLocalCustomProperty(const String& name, const PropertyDictionary& inline_properties,
	const ElementDefinition* definition)
{
	const Property* declared = inline_properties.GetCustomProperty(name);
	if (definition && (!declared || definition->HasImportantProperties()))
		declared = Cascade::Choose(declared, definition->GetCustomProperty(name));
	return declared;
}

const Property* ElementStyle::GetLocalShorthand(ShorthandId id, const PropertyDictionary& inline_properties, const ElementDefinition* definition)
{
	const Property* declared = inline_properties.GetVarShorthand(id);
	if (definition && (!declared || definition->HasImportantProperties()))
		declared = Cascade::Choose(declared, definition->GetProperties().GetVarShorthand(id));
	return declared;
}

ElementStyle::PropertyElementPair ElementStyle::GetSpecifiedProperty(const PropertySources& sources, PropertyId id)
{
	const Property* local_property = GetLocalProperty(id, sources.inline_properties, sources.definition, sources.animation_layers);
	if (local_property)
	{
		if (local_property->unit == Unit::CSS_WIDE_KEYWORD)
			return ResolveSpecifiedKeyword(sources, id, *local_property);
		return {local_property, sources.element};
	}

	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return {};

	// If we can inherit this property, return our parent's property.
	if (property_definition->IsInherited())
	{
		Element* parent = sources.element->GetParentNode();
		while (parent)
		{
			const Property* parent_property = parent->GetStyle()->GetLocalProperty(id);
			if (parent_property)
			{
				if (parent_property->unit == Unit::CSS_WIDE_KEYWORD)
					return ResolveSpecifiedKeyword(parent->GetStyle()->GetPropertySources(), id, *parent_property);
				return {parent_property, parent};
			}

			parent = parent->GetParentNode();
		}
	}

	// No property available! Return the default value.
	return {property_definition->GetDefaultValue(), nullptr};
}

ElementStyle::PropertyElementPair ElementStyle::ResolveSpecifiedKeyword(const PropertySources& sources, PropertyId id, const Property& keyword_property)
{
	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return {};

	CssWideKeyword keyword = CssWideKeyword(keyword_property.Get<int>());
	if (keyword == CssWideKeyword::Revert)
	{
		// The user-agent origin's declaration, else as 'unset'.
		const Property* user_agent_property = (sources.definition ? sources.definition->GetUserAgentProperty(id) : nullptr);
		if (user_agent_property && user_agent_property->unit != Unit::CSS_WIDE_KEYWORD)
			return {user_agent_property, sources.element};
		keyword = (user_agent_property ? CssWideKeyword(user_agent_property->Get<int>()) : CssWideKeyword::Unset);
		if (keyword == CssWideKeyword::Revert)
			keyword = CssWideKeyword::Unset;
	}
	if (keyword == CssWideKeyword::Unset)
		keyword = (property_definition->IsInherited() ? CssWideKeyword::Inherit : CssWideKeyword::Initial);

	if (keyword == CssWideKeyword::Inherit)
	{
		if (Element* parent = sources.element->GetParentNode())
			return GetSpecifiedProperty(parent->GetStyle()->GetPropertySources(), id);
	}

	return {property_definition->GetDefaultValue(), nullptr};
}

ElementStyle::ComputedKeyword ElementStyle::ResolveComputedKeyword(PropertyId id, const Property& keyword_property) const
{
	ComputedKeyword result;
	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return result;
	const bool inherited = property_definition->IsInherited();

	const ElementStyle* style = this;
	const Property* declared = &keyword_property;
	while (true)
	{
		CssWideKeyword keyword = CssWideKeyword(declared->Get<int>());
		if (keyword == CssWideKeyword::Revert)
		{
			const Property* user_agent_property = (style->definition ? style->definition->GetUserAgentProperty(id) : nullptr);
			if (user_agent_property && user_agent_property->unit != Unit::CSS_WIDE_KEYWORD)
			{
				result.property = user_agent_property;
				result.element = style->element;
				return result;
			}
			keyword = (user_agent_property ? CssWideKeyword(user_agent_property->Get<int>()) : CssWideKeyword::Unset);
			if (keyword == CssWideKeyword::Revert)
				keyword = CssWideKeyword::Unset;
		}
		if (keyword == CssWideKeyword::Unset)
			keyword = (inherited ? CssWideKeyword::Inherit : CssWideKeyword::Initial);

		if (keyword == CssWideKeyword::Initial)
		{
			// A non-inherited property computes to its initial value when nothing is declared, an inherited one takes its
			// parent's value: only the latter needs the initial value computed.
			if (inherited && style == this)
				result.property = property_definition->GetDefaultValue();
			return result;
		}

		// 'inherit': an inherited property takes its parent's computed value anyway (ComputedValues::CopyInherited); a
		// non-inherited one takes the declaration its parent computed, computed with the parent's font size.
		if (inherited)
			return result;
		if (style == this)
			result.explicit_inherit = true;

		Element* parent = style->element->GetParentNode();
		if (!parent)
			return result;
		style = parent->GetStyle();
		declared = style->GetLocalProperty(id);
		if (!declared)
			return result;
		if (declared->unit != Unit::CSS_WIDE_KEYWORD)
		{
			result.property = declared;
			result.element = parent;
			return result;
		}
	}
}

ElementStyle::PropertyElementPair ElementStyle::GetSpecifiedCustomProperty(const PropertySources& sources, const String& name)
{
	// Custom properties are inherited: the nearest declaration up the tree, where CSS-wide keywords say 'inherit' (also
	// 'unset'), 'initial' (the guaranteed-invalid value: no property) and 'revert' (the user-agent declaration).
	Element* element = sources.element;
	const ElementDefinition* definition = sources.definition;
	const Property* property = GetLocalCustomProperty(name, sources.inline_properties, sources.definition);
	while (true)
	{
		if (property)
		{
			if (property->unit != Unit::CSS_WIDE_KEYWORD)
				return {property, element};

			CssWideKeyword keyword = CssWideKeyword(property->Get<int>());
			if (keyword == CssWideKeyword::Revert)
			{
				const Property* user_agent_property = (definition ? definition->GetUserAgentCustomProperty(name) : nullptr);
				if (user_agent_property && user_agent_property->unit != Unit::CSS_WIDE_KEYWORD)
					return {user_agent_property, element};
				keyword = CssWideKeyword::Unset;
			}
			if (keyword == CssWideKeyword::Initial)
				return {};
		}

		element = element->GetParentNode();
		if (!element)
			return {};
		ElementStyle* style = element->GetStyle();
		definition = style->definition.get();
		property = GetLocalCustomProperty(name, style->inline_properties, definition);
	}
}

ElementStyle::PropertyElementPair ElementStyle::GetSpecifiedShorthand(const PropertySources& sources, ShorthandId id)
{
	const Property* local_shorthand = GetLocalShorthand(id, sources.inline_properties, sources.definition);
	if (local_shorthand)
		return {local_shorthand, sources.element};

	const ShorthandDefinition* shorthand_definition = StyleSheetSpecification::GetShorthand(id);
	if (!shorthand_definition)
		return {};

	if (shorthand_definition->inherited)
	{
		Element* parent = sources.element->GetParentNode();
		while (parent)
		{
			const Property* parent_shorthand = parent->GetStyle()->GetLocalShorthand(id);
			if (parent_shorthand)
				return {parent_shorthand, parent};

			parent = parent->GetParentNode();
		}
	}

	return {};
}

ElementStyle::SubstitutionResult ElementStyle::SubstituteVariableOnce(const PropertySources& sources, const String& value,
	SmallUnorderedSet<String>& variable_dependencies, SmallUnorderedSet<String>& cycle_chain, String& result)
{
	const size_t begin = value.find("var(");
	const size_t end_name = (begin != String::npos ? value.find_first_of(",)", begin) : String::npos);
	if (begin == String::npos || end_name == String::npos)
		return SubstitutionResult::NoVariablesFound;

	const bool has_fallback = (value[end_name] == ',');

	size_t end_parenthesis = end_name;
	if (has_fallback)
	{
		int depth = 1;
		for (end_parenthesis = end_name + 1; end_parenthesis < value.size(); end_parenthesis++)
		{
			const char c = value[end_parenthesis];
			if (c == '(')
				depth += 1;
			else if (c == ')')
				depth -= 1;
			if (depth == 0)
				break;
		}
	}
	if (end_parenthesis >= value.size())
		return SubstitutionResult::NoVariablesFound;

	String var_name = StringUtilities::StripWhitespace(StringView{value, begin + 4, end_name - begin - 4});
	const bool cycle_detected = (!cycle_chain.insert(var_name).second);
	if (cycle_detected)
	{
		Log::Message(Log::LT_ERROR, "Invalid substitution, cycle detected with variable: %s. In element: %s", var_name.c_str(),
			sources.element->GetAddress().c_str());
		return SubstitutionResult::Error;
	}

	auto SubstituteResult = [&](String substitute_value) {
		cycle_chain.erase(var_name);
		variable_dependencies.insert(std::move(var_name));
		result = value.substr(0, begin) + std::move(substitute_value) + value.substr(end_parenthesis + 1);
		return SubstitutionResult::Substituted;
	};

	const auto [custom_property, property_element] = GetSpecifiedCustomProperty(sources, var_name);
	const bool valid_property = (custom_property && property_element);
	if (!valid_property && has_fallback)
	{
		return SubstituteResult(StringUtilities::StripWhitespace(StringView{value, end_name + 1, end_parenthesis - end_name - 1}));
	}

	if (!valid_property)
	{
		Log::Message(Log::LT_ERROR, "Invalid substitution, variable '%s' not defined. In element: %s", var_name.c_str(),
			sources.element->GetAddress().c_str());
		return SubstitutionResult::Error;
	}

	String substitution_value;
	if (custom_property->unit == Unit::VAR_EXPRESSION)
	{
		// Reset cycle chain if (and only if) moving up to a new element.
		SmallUnorderedSet<String> new_cycle_chain;
		SmallUnorderedSet<String>& next_cycle_chain = (property_element == sources.element ? cycle_chain : new_cycle_chain);

		// We cannot always use the definition or inline properties from the style of the property element. Particularly
		// for the starting style and the before-change values of transitions, these are not the current element's. Instead, use the passed-in
		// values when the property element is the passed-in element. Otherwise, for ancestor elements, we assume they
		// are fully determined here, so fetch them from their elements.
		const PropertySources& next_sources = (property_element == sources.element ? sources : property_element->GetStyle()->GetPropertySources());

		if (!SubstituteVariables(next_sources, custom_property->Get<String>(), variable_dependencies, next_cycle_chain, substitution_value))
			return SubstitutionResult::Error;
	}
	else
	{
		substitution_value = custom_property->Get<String>();
	}

	return SubstituteResult(std::move(substitution_value));
}

bool ElementStyle::SubstituteVariables(const PropertySources& sources, String value, SmallUnorderedSet<String>& variable_dependencies,
	SmallUnorderedSet<String>& cycle_chain, String& result)
{
	bool looping = true;
	for (int i = 0; looping; i++)
	{
		SubstitutionResult substitution = SubstituteVariableOnce(sources, value, variable_dependencies, cycle_chain, result);
		switch (substitution)
		{
		case SubstitutionResult::Substituted:
		{
			value = std::move(result);
		}
		break;
		case SubstitutionResult::NoVariablesFound:
		{
			if (i == 0)
			{
				Log::Message(Log::LT_ERROR, "Invalid substitution, expected 'var()': %s. In element: %s", value.c_str(),
					sources.element->GetAddress().c_str());
				return false;
			}
			looping = false;
		}
		break;
		case SubstitutionResult::Error: return false;
		}
	}

	result = std::move(value);
	return true;
}

const Property* ElementStyle::ResolveVariables(const PropertySources& sources, const PropertyDictionary& substituted_shorthands, PropertyId id,
	const Property* property, SmallUnorderedSet<String>& variable_dependencies, Property& property_storage)
{
	if (property->unit == Unit::SHORTHAND_PLACEHOLDER)
	{
		const Property* property_from_shorthand = substituted_shorthands.GetProperty(id);
		if (!property_from_shorthand)
		{
			Log::Message(Log::LT_ERROR, "Property '%s' could not be resolved: Pending substitution from a shorthand.",
				StyleSheetSpecification::GetPropertyName(id).c_str());
			return nullptr;
		}

		return property_from_shorthand;
	}

	if (property->unit != Unit::VAR_EXPRESSION)
		return property;

	String substitution_result;
	SmallUnorderedSet<String> cycle_chain;
	if (!SubstituteVariables(sources, property->Get<String>(), variable_dependencies, cycle_chain, substitution_result))
		return nullptr;

	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
	{
		Log::Message(Log::LT_ERROR, "Invalid substitution, property '%s' not defined.", StyleSheetSpecification::GetPropertyName(id).c_str());
		return nullptr;
	}

	if (!property_definition->ParseValue(property_storage, substitution_result))
	{
		Log::Message(Log::LT_ERROR, "Invalid substitution, property '%s' has invalid value: %s", StyleSheetSpecification::GetPropertyName(id).c_str(),
			substitution_result.c_str());
		return nullptr;
	}
	return &property_storage;
}

const Property* ElementStyle::ResolveVariablesWithShorthandExpansion(const PropertySources& sources, PropertyId id, const Property* property,
	std::optional<PropertyDictionary>& substituted_shorthands, SmallUnorderedSet<String>& variable_dependencies, Property& property_storage)
{
	if (!property)
		return nullptr;
	if (property->unit != Unit::SHORTHAND_PLACEHOLDER && property->unit != Unit::VAR_EXPRESSION)
		return property;
	if (property->unit == Unit::SHORTHAND_PLACEHOLDER && !substituted_shorthands.has_value())
	{
		// Lazy-compute shorthand expansion.
		substituted_shorthands.emplace();
		ExpandVarShorthands(*substituted_shorthands, sources, std::nullopt);
	}
	variable_dependencies.clear();
	return ResolveVariables(sources, (substituted_shorthands ? *substituted_shorthands : element_style_data->empty_property_dictionary), id, property,
		variable_dependencies, property_storage);
}

void ElementStyle::ExpandVarShorthands(PropertyDictionary& out_substituted_shorthands, const PropertySources& sources,
	std::optional<DirtyPropertiesRef> dirty_properties)
{
	out_substituted_shorthands.Clear();
	SmallUnorderedSet<String> variable_dependencies;
	SmallUnorderedSet<String> cycle_chain;

	for (const PropertyDictionary* properties : {sources.definition ? &sources.definition->GetProperties() : nullptr, &sources.inline_properties})
	{
		if (!properties)
			continue;

		for (const auto& [shorthand_id, property] : properties->GetVarShorthands())
		{
			RMLUI_ASSERT(property.unit == Unit::VAR_EXPRESSION);
			// The shorthand the cascade picks of the definition's and the inline one (an important declaration of the
			// definition beats a normal inline one) is expanded last, so its longhands are the ones kept.
			if (GetLocalShorthand(shorthand_id, sources.inline_properties, sources.definition) != &property)
				continue;
			variable_dependencies.clear();
			cycle_chain.clear();

			String substitution_result;
			if (!SubstituteVariables(sources, property.Get<String>(), variable_dependencies, cycle_chain, substitution_result))
				continue;

			if (!StyleSheetSpecification::GetPropertySpecification().ParseShorthandDeclaration(out_substituted_shorthands, shorthand_id,
					substitution_result))
				continue;

			if (dirty_properties.has_value())
			{
				const bool dirty_variables_used = std::any_of(variable_dependencies.begin(), variable_dependencies.end(),
					[&](const String& variable_name) { return dirty_properties->variables.count(variable_name) != 0; });

				// Dirty all properties of the current shorthand either if: (1) it uses a dirtied variable, or (2) the shorthand itself is dirty.
				if (dirty_variables_used || dirty_properties->var_shorthands.count(shorthand_id) != 0)
				{
					const PropertyIdSet ids = StyleSheetSpecification::GetShorthandUnderlyingProperties(shorthand_id);
					for (const PropertyId id : ids)
						dirty_properties->properties.Insert(id);
				}
			}
		}
	}
}

#ifdef RMLUI_TESTS_ENABLED
static int num_definition_updates = 0;
int ElementStyle::GetNumDefinitionUpdates()
{
	return num_definition_updates;
}
#endif

void ElementStyle::UpdateDefinition()
{
	RMLUI_ZoneScoped;
#ifdef RMLUI_TESTS_ENABLED
	num_definition_updates++;
#endif

	SharedPtr<const ElementDefinition> new_definition;

	const StyleSheet* style_sheet = element->GetStyleSheet();
	if (style_sheet)
	{
		// A pseudo-element's rules match through its originating element (phase 3).
		const PseudoElementType pseudo_element_type = element->GetPseudoElementType();
		if (pseudo_element_type == PseudoElementType::None)
			new_definition = style_sheet->GetElementDefinition(element);
		else if (Element* originating_element = element->GetParentNode())
			new_definition = style_sheet->GetElementDefinition(originating_element, pseudo_element_type);
	}
	// The element's pseudo-elements match through it: generate them again when it has some or the sheet has rules for
	// them (the other causes, display and list styles, are property changes: Element::OnPropertyChange).
	if (element->GetPseudoElementType() == PseudoElementType::None)
		ElementCascade::DirtyPseudoElementsAfterMatch(*element, style_sheet);

	// Switch the property definitions if the definition has changed.
	if (new_definition != definition)
	{
		PropertyIdSet changed_properties;
		UnorderedSet<String> changed_variables;
		UnorderedSet<ShorthandId> changed_shorthands;

		if (definition)
		{
			changed_properties = definition->GetPropertyIds();
			for (const auto& [name, _] : definition->GetProperties().GetCustomProperties())
				changed_variables.insert(name);
			for (const auto& [shorthand_id, _] : definition->GetProperties().GetVarShorthands())
				changed_shorthands.insert(shorthand_id);
		}

		if (new_definition)
		{
			changed_properties |= new_definition->GetPropertyIds();
			for (const auto& [name, _] : new_definition->GetProperties().GetCustomProperties())
				changed_variables.insert(name);
			for (const auto& [shorthand_id, _] : new_definition->GetProperties().GetVarShorthands())
				changed_shorthands.insert(shorthand_id);
		}

		if (definition && new_definition)
		{
			// Remove properties that compare equal from the changed list. Except if they can take variables: In this
			// case, the two properties may compare equal in value but represent different resolved properties depending
			// on their context. In that case we need to proceed to resolve them fully.
			const PropertyIdSet properties_in_both_definitions = (definition->GetPropertyIds() & new_definition->GetPropertyIds());

			for (PropertyId id : properties_in_both_definitions)
			{
				const Property* p0 = definition->GetProperty(id);
				const Property* p1 = new_definition->GetProperty(id);
				// The same holds for CSS-wide keywords: 'revert' depends on the user-agent declarations of the definition.
				if (p0 && p1 && *p0 == *p1 && p0->unit != Unit::VAR_EXPRESSION && p1->unit != Unit::SHORTHAND_PLACEHOLDER &&
					p0->unit != Unit::CSS_WIDE_KEYWORD && p0->important == p1->important)
					changed_properties.Erase(id);
			}
		}

		// Transitions (p3-animation): the before-change values of properties whose transition comes with the new definition.
		CaptureBeforeChangeValues(changed_properties, new_definition.get());

		definition = new_definition;

		DirtyProperties(changed_properties);
		dirty_variables.insert(std::make_move_iterator(changed_variables.begin()), std::make_move_iterator(changed_variables.end()));
		dirty_var_shorthands.insert(changed_shorthands.begin(), changed_shorthands.end());
	}
}

bool ElementStyle::SetPseudoClass(const String& pseudo_class, bool activate, bool override_class)
{
	bool changed = false;

	if (activate)
	{
		PseudoClassState& state = pseudo_classes[pseudo_class];
		changed = (state == PseudoClassState::Clear);
		state = (state | (override_class ? PseudoClassState::Override : PseudoClassState::Set));
	}
	else
	{
		auto it = pseudo_classes.find(pseudo_class);
		if (it != pseudo_classes.end())
		{
			PseudoClassState& state = it->second;
			state = (state & (override_class ? PseudoClassState::Set : PseudoClassState::Override));
			if (state == PseudoClassState::Clear)
			{
				pseudo_classes.erase(it);
				changed = true;
			}
		}
	}

	return changed;
}

bool ElementStyle::IsPseudoClassSet(const String& pseudo_class) const
{
	return (pseudo_classes.count(pseudo_class) == 1);
}

const PseudoClassMap& ElementStyle::GetActivePseudoClasses() const
{
	return pseudo_classes;
}

bool ElementStyle::SetClass(const String& class_name, bool activate)
{
	const auto class_location = std::find(classes.begin(), classes.end(), class_name);

	bool changed = false;
	if (activate)
	{
		if (class_location == classes.end())
		{
			classes.push_back(class_name);
			changed = true;
		}
	}
	else
	{
		if (class_location != classes.end())
		{
			classes.erase(class_location);
			changed = true;
		}
	}

	return changed;
}

bool ElementStyle::IsClassSet(const String& class_name) const
{
	return std::find(classes.begin(), classes.end(), class_name) != classes.end();
}

void ElementStyle::SetClassNames(const String& class_names)
{
	classes.clear();
	StringUtilities::ExpandString(classes, class_names, ' ');
}

String ElementStyle::GetClassNames() const
{
	String class_names;
	for (size_t i = 0; i < classes.size(); i++)
	{
		if (i != 0)
		{
			class_names += " ";
		}
		class_names += classes[i];
	}

	return class_names;
}

const StringList& ElementStyle::GetClassNameList() const
{
	return classes;
}

bool ElementStyle::SetProperty(PropertyId id, const Property& property)
{
	Property new_property = property;

	new_property.definition = StyleSheetSpecification::GetProperty(id);
	if (!new_property.definition)
		return false;

	inline_properties.SetProperty(id, new_property);
	inline_important |= new_property.important;
	DirtyProperty(id);

	return true;
}

void ElementStyle::SetCustomProperty(const String& name, const Property& property)
{
	inline_properties.SetCustomProperty(name, property);
	inline_important |= property.important;
	dirty_variables.insert(name);
}

void ElementStyle::SetVarShorthand(ShorthandId id, const Property& property)
{
	inline_properties.SetVarShorthand(id, property);
	inline_important |= property.important;
	dirty_var_shorthands.insert(id);
}

void ElementStyle::RemoveProperty(PropertyId id)
{
	int size_before = inline_properties.GetNumProperties();
	inline_properties.RemoveProperty(id);

	if (inline_properties.GetNumProperties() != size_before)
		DirtyProperty(id);
}

void ElementStyle::RemoveCustomProperty(const String& name)
{
	if (inline_properties.RemoveCustomProperty(name))
		dirty_variables.insert(name);
}

void ElementStyle::RemoveVarShorthand(ShorthandId id)
{
	if (inline_properties.RemoveVarShorthand(id))
		dirty_var_shorthands.insert(id);
}

void ElementStyle::SetStyleAttribute(const PropertyDictionary& properties)
{
	if (style_attribute)
	{
		for (const PropertyId id : style_attribute->properties)
		{
			if (!properties.GetProperty(id))
				RemoveProperty(id);
		}
		for (const String& name : style_attribute->custom_properties)
		{
			if (!properties.GetCustomProperty(name))
				RemoveCustomProperty(name);
		}
		for (const ShorthandId id : style_attribute->var_shorthands)
		{
			if (!properties.GetVarShorthand(id))
				RemoveVarShorthand(id);
		}
	}
	else if (properties.Empty())
	{
		return;
	}
	else
	{
		style_attribute = MakeUnique<StyleAttributeDeclarations>();
	}

	style_attribute->properties.Clear();
	style_attribute->custom_properties.clear();
	style_attribute->var_shorthands.clear();

	for (const auto& name_value : properties.GetProperties())
	{
		SetProperty(name_value.first, name_value.second);
		style_attribute->properties.Insert(name_value.first);
	}
	for (const auto& name_value : properties.GetCustomProperties())
	{
		SetCustomProperty(name_value.first, name_value.second);
		style_attribute->custom_properties.push_back(name_value.first);
	}
	for (const auto& [shorthand_id, property] : properties.GetVarShorthands())
	{
		SetVarShorthand(shorthand_id, property);
		style_attribute->var_shorthands.push_back(shorthand_id);
	}
}

const Property* ElementStyle::GetProperty(PropertyId id) const
{
	const PropertyElementPair result = GetSpecifiedProperty(GetPropertySources(), id);
	if (!result.element)
		return result.property;

	SmallUnorderedSet<String> variable_dependencies;
	const Property* resolved =
		result.element->GetStyle()->ResolveVariables(id, result.property, variable_dependencies, element_style_data->returned_property_storage);
	if (resolved)
		return resolved;

	// An invalid substitution makes the property behave as unset (CSS Variables 1 §3): an inherited property takes the
	// parent's value, as ComputeValues does for the eagerly computed ones, others their initial value.
	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return nullptr;
	if (property_definition->IsInherited())
	{
		if (Element* parent = result.element->GetParentNode())
			return parent->GetStyle()->GetProperty(id);
	}
	return property_definition->GetDefaultValue();
}

const Property* ElementStyle::GetUnderlyingProperty(PropertyId id) const
{
	// GetProperty, without this element's animation layers.
	PropertySources sources = GetPropertySources();
	sources.animation_layers = nullptr;
	const PropertyElementPair result = GetSpecifiedProperty(sources, id);
	if (!result.element)
		return result.property;

	SmallUnorderedSet<String> variable_dependencies;
	const Property* resolved =
		result.element->GetStyle()->ResolveVariables(id, result.property, variable_dependencies, element_style_data->returned_property_storage);
	if (resolved)
		return resolved;

	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return nullptr;
	if (property_definition->IsInherited())
	{
		if (Element* parent = result.element->GetParentNode())
			return parent->GetStyle()->GetProperty(id);
	}
	return property_definition->GetDefaultValue();
}

const Property* ElementStyle::GetCustomProperty(const String& name) const
{
	const PropertyElementPair result = GetSpecifiedCustomProperty(GetPropertySources(), name);
	if (!result.element || !result.property || result.property->unit != Unit::VAR_EXPRESSION)
		return result.property;

	SmallUnorderedSet<String> variable_dependencies;
	SmallUnorderedSet<String> cycle_chain;
	String substitution_result;
	if (!SubstituteVariables(result.element->GetStyle()->GetPropertySources(), result.property->Get<String>(), variable_dependencies, cycle_chain,
			substitution_result))
		return nullptr;

	element_style_data->returned_property_storage = Property{substitution_result, Unit::STRING};
	return &element_style_data->returned_property_storage;
}

const Property* ElementStyle::GetLocalProperty(PropertyId id) const
{
	return GetLocalProperty(id, inline_properties, definition.get(), animation_layers.get());
}

const Property* ElementStyle::GetLocalPropertyForCompute(PropertyId id) const
{
	const Property* property = GetLocalProperty(id);
	if (!property || property->unit != Unit::CSS_WIDE_KEYWORD)
		return property;
	// The pre-passes compute font-size and line-height, both inherited: the keyword gives the initial or user-agent
	// value of this element, or nothing (the parent's value).
	const ComputedKeyword keyword = ResolveComputedKeyword(id, *property);
	return keyword.property;
}

const Property* ElementStyle::GetLocalPropertyWithResolvedVariables(PropertyId id) const
{
	const Property* property = GetLocalProperty(id, inline_properties, definition.get(), animation_layers.get());
	if (!property)
		return nullptr;

	SmallUnorderedSet<String> variable_dependencies;
	if (property->unit == Unit::CSS_WIDE_KEYWORD)
	{
		// The declaration the keyword stands for, resolved where it is declared.
		const PropertyElementPair resolved = ResolveSpecifiedKeyword(GetPropertySources(), id, *property);
		if (!resolved.property || !resolved.element)
			return resolved.property;
		return resolved.element->GetStyle()->ResolveVariables(id, resolved.property, variable_dependencies,
			element_style_data->returned_property_storage);
	}
	return ResolveVariables(id, property, variable_dependencies, element_style_data->returned_property_storage);
}

const Property* ElementStyle::GetLocalCustomProperty(const String& name) const
{
	return GetLocalCustomProperty(name, inline_properties, definition.get());
}

const Property* ElementStyle::GetLocalShorthand(ShorthandId id) const
{
	return GetLocalShorthand(id, inline_properties, definition.get());
}

const PropertyDictionary& ElementStyle::GetLocalStyleProperties() const
{
	return inline_properties;
}

static float ComputeLength(NumericValue value, Element* element)
{
	LengthContext context;
	context.vp_dimensions = Vector2f(1.0f);

	if (Any(value.unit & Unit::DP_SCALABLE_LENGTH))
	{
		if (Context* element_context = element->GetContext())
			context.dp_ratio = element_context->GetDensityIndependentPixelRatio();
	}

	switch (value.unit)
	{
	case Unit::EM: context.font_size = element->GetComputedValues().font_size(); break;
	case Unit::REM:
		if (ElementDocument* document = element->GetOwnerDocument())
			context.root_font_size = document->GetComputedValues().font_size();
		else
			context.root_font_size = DefaultComputedValues().font_size();
		break;
	case Unit::VW:
	case Unit::VH:
	case Unit::VMIN:
	case Unit::VMAX:
		if (Context* element_context = element->GetContext())
			context.vp_dimensions = Vector2f(element_context->GetDimensions());
		break;
	case Unit::CH:
	case Unit::EX:
		context.font_size = element->GetComputedValues().font_size();
		context.font_face = element->GetFontFaceHandle();
		break;
	default: break;
	}

	const float result = ComputeLength(value, context);
	return result;
}

float ElementStyle::ResolveNumericValue(NumericValue value, float base_value) const
{
	if (value.unit == Unit::PX)
		return value.number;
	else if (Any(value.unit & Unit::LENGTH))
		return ComputeLength(value, element);

	switch (value.unit)
	{
	case Unit::NUMBER: return value.number * base_value;
	case Unit::PERCENT: return value.number * base_value * 0.01f;
	case Unit::X: return value.number;
	case Unit::DEG:
	case Unit::RAD:
	case Unit::TURN:
	case Unit::GRAD: return ComputeAngle(value);
	default: break;
	}

	RMLUI_ERROR;
	return 0.f;
}

float ElementStyle::ResolveRelativeLength(NumericValue value, RelativeTarget relative_target) const
{
	// There is an exception on font-size properties, as 'em' units here refer to parent font size instead
	if (Any(value.unit & Unit::LENGTH) && !(value.unit == Unit::EM && relative_target == RelativeTarget::ParentFontSize))
	{
		const float result = ComputeLength(value, element);
		return result;
	}

	float base_value = 0.0f;

	switch (relative_target)
	{
	case RelativeTarget::None: base_value = 1.0f; break;
	case RelativeTarget::ContainingBlockWidth: base_value = element->GetContainingBlock().x; break;
	case RelativeTarget::ContainingBlockHeight: base_value = element->GetContainingBlock().y; break;
	case RelativeTarget::FontSize: base_value = element->GetComputedValues().font_size(); break;
	case RelativeTarget::ParentFontSize:
	{
		auto p = element->GetParentNode();
		base_value = (p ? p->GetComputedValues().font_size() : DefaultComputedValues().font_size());
	}
	break;
	case RelativeTarget::LineHeight: base_value = element->GetLineHeight(); break;
	}

	float scale_value = 0.0f;

	switch (value.unit)
	{
	case Unit::EM:
	case Unit::NUMBER: scale_value = value.number; break;
	case Unit::PERCENT: scale_value = value.number * 0.01f; break;
	default: break;
	}

	return base_value * scale_value;
}

void ElementStyle::DirtyInheritedProperties()
{
	dirty_properties |= StyleSheetSpecification::GetRegisteredInheritedProperties();
}

void ElementStyle::DirtyPropertiesWithUnits(Units units)
{
	// Dirty all the properties of this element that use the unit(s).
	for (auto it = Iterate(); !it.AtEnd(); ++it)
	{
		auto name_property_pair = *it;
		PropertyId id = name_property_pair.first;
		const Property& property = name_property_pair.second;
		Units dependencies = property.unit;
		if (property.unit == Unit::STYLE_VALUE)
			dependencies = GetStyleValueUnitDependencies(property);
#ifdef RMLUI_MATH_EXPRESSIONS
		if (property.unit == Unit::CALCULATION)
			dependencies = GetMutableUnitDependencies(property);
		else if (property.unit == Unit::VAR_EXPRESSION || property.unit == Unit::SHORTHAND_PLACEHOLDER)
			dependencies = GetResolvedUnitDependencies(id);
#endif
		if (Any(dependencies & units))
			DirtyProperty(id);
	}
}

void ElementStyle::DirtyPropertiesWithUnitsRecursive(Units units)
{
	DirtyPropertiesWithUnits(units);

	// Now dirty all of our descendant's properties that use the unit(s).
	int num_children = element->GetNumChildren(true);
	for (int i = 0; i < num_children; ++i)
		element->GetChild(i)->GetStyle()->DirtyPropertiesWithUnitsRecursive(units);
}

bool ElementStyle::AnyPropertiesDirty() const
{
	return !dirty_properties.Empty() || !dirty_variables.empty() || !dirty_var_shorthands.empty();
}

const Property* ElementStyle::ResolveKeyFrameProperty(PropertyId id, const Property* property, const PropertyDictionary& block_properties,
	std::optional<PropertyDictionary>& block_substituted_shorthands, SmallUnorderedSet<String>& variable_dependencies, Property& property_storage)
{
	variable_dependencies.clear();
	PropertySources sources{element, definition.get(), block_properties};
	return ResolveVariablesWithShorthandExpansion(sources, id, property, block_substituted_shorthands, variable_dependencies, property_storage);
}

PlainPropertiesIterator ElementStyle::Iterate() const
{
	return PlainPropertiesIterator{
		inline_properties.GetProperties(),
		definition ? &definition->GetProperties().GetProperties() : nullptr,
		animation_layers ? &animation_layers->transition_properties.GetProperties() : nullptr,
		animation_layers ? &animation_layers->animated_properties.GetProperties() : nullptr,
		inline_important || (definition && definition->HasImportantProperties()),
	};
}

UniquePtr<AllPropertiesIterator> ElementStyle::IterateAll(Element* filter_inherited_by) const
{
	return MakeUnique<AllPropertiesIterator>(inline_properties, definition ? &definition->GetProperties() : nullptr,
		filter_inherited_by ? filter_inherited_by->GetStyle() : nullptr);
}

void ElementStyle::Initialize()
{
	element_style_data.Initialize();
}
void ElementStyle::Shutdown()
{
	element_style_data.Shutdown();
}

void ElementStyle::DirtyProperty(PropertyId id)
{
	dirty_properties.Insert(id);
}

PropertyIdSet ElementStyle::ComputeValues(Style::ComputedValues& values, const Style::ComputedValues* parent_values,
	const Style::ComputedValues* document_values, bool values_are_default_initialized, float dp_ratio, Vector2f vp_dimensions)
{
	if (!AnyPropertiesDirty())
		return PropertyIdSet();

	RMLUI_ZoneScopedC(0xFF7F50);

	// Fast path: only the inherited visibility changed (a document or a subtree is shown or hidden) and this element does
	// not declare its own, so it takes its parent's without every other value being computed again.
	if (parent_values && !values_are_default_initialized && dirty_variables.empty() && dirty_var_shorthands.empty() && dirty_properties.Size() == 1 &&
		dirty_properties.Contains(PropertyId::Visibility) && !GetLocalProperty(PropertyId::Visibility))
	{
		values.visibility(parent_values->visibility());
		for (int i = 0; i < element->GetNumChildren(true); i++)
			element->GetChild(i)->GetStyle()->dirty_properties.Insert(PropertyId::Visibility);
		PropertyIdSet result(std::move(dirty_properties));
		dirty_properties.Clear();
		return result;
	}

	// We're a bit conservative here and recompute shorthand substitutions also if any variables have been dirtied at
	// all. @performance We could be more strict and only recompute shorthands if any variables they *use* have been dirtied.
	if (!dirty_variables.empty() || !dirty_var_shorthands.empty())
	{
		ExpandVarShorthands(expanded_shorthands, GetPropertySources(), GetDirtyPropertiesRef());
	}

	// The transition and animation lists follow their longhands (p3-animation); transitions below use the new list.
	UpdateAnimationLists(dirty_properties);
	const ComputeContext transition_context{values, parent_values, document_values, dp_ratio, vp_dimensions};

	// Generally, this is how it works:
	//   1. Assign default values (clears any removed properties)
	//   2. Inherit inheritable values from parent
	//   3. Assign any local properties (from inline style or stylesheet)
	//   4. Dirty properties in children that are inherited

	Property property_storage;
	const FontFaceHandle font_face_before = values.font_face_handle();
	const float font_size_before = values.font_size();
	const Style::LineHeight line_height_before = values.line_height();

	// The next flag is just a small optimization, if the element was just created we don't need to copy all the default values.
	if (!values_are_default_initialized)
	{
		// This needs to be done in case some properties were removed and thus not in our local style anymore.
		// If we skipped this, the old dirty value would be unmodified, instead, now it is set to its default value.
		// Strictly speaking, we only really need to do this for the dirty, non-inherited values. However, in most
		// cases it seems simply assigning all non-inherited values is faster than iterating the dirty properties.
		values.CopyNonInherited(DefaultComputedValues());
	}

	if (parent_values)
		values.CopyInherited(*parent_values);
	else if (!values_are_default_initialized)
		values.CopyInherited(DefaultComputedValues());

	// Values (p3-values): 'ch' and 'ex' resolve against the inherited font face until this element's own is known.
	const FontFaceHandle loop_font_face = values.font_face_handle();
	FontUnitScope font_unit_scope(loop_font_face);

	SmallUnorderedSet<String> variable_dependencies;
	bool dirty_em_properties = false;

#ifdef RMLUI_MATH_EXPRESSIONS
	// ComputeValues already visits every specified longhand below, so rebuild the sparse cache from
	// those resolved values. This also drops entries for removed properties without adding any
	// substitution work to unit-dirty scans.
	if (resolved_unit_dependencies)
		resolved_unit_dependencies.reset();
#endif

	// The non-inherited properties declared 'inherit' are collected again below (CSS-wide keywords).
	if (explicit_inherit)
		explicit_inherit->Clear();

	// Always do font-size first if dirty, because of em-relative values
	if (dirty_properties.Contains(PropertyId::FontSize))
	{
		if (const Property* property = GetLocalPropertyForCompute(PropertyId::FontSize))
		{
			variable_dependencies.clear();
			property = ResolveVariables(PropertyId::FontSize, property, variable_dependencies, property_storage);
			if (property)
				values.font_size(ComputeFontsize(property, values, parent_values, document_values, dp_ratio, vp_dimensions));
		}
		else if (parent_values)
			values.font_size(parent_values->font_size());
		if (const Property* transitioned = TransitionPrePass(PropertyId::FontSize, transition_context))
			values.font_size(ComputeFontsize(transitioned, values, parent_values, document_values, dp_ratio, vp_dimensions));

		if (font_size_before != values.font_size())
		{
			dirty_em_properties = true;
			dirty_properties.Insert(PropertyId::LineHeight);
		}
	}
	else
	{
		values.font_size(font_size_before);
	}

	const float font_size = values.font_size();
	const float document_font_size = (document_values ? document_values->font_size() : DefaultComputedValues().font_size());

	// Since vertical-align depends on line-height we compute this before iteration
	if (dirty_properties.Contains(PropertyId::LineHeight))
	{
		if (const Property* property = GetLocalPropertyForCompute(PropertyId::LineHeight))
		{
			variable_dependencies.clear();
			property = ResolveVariables(PropertyId::LineHeight, property, variable_dependencies, property_storage);
			if (property)
				values.line_height(ComputeLineHeight(property, font_size, document_font_size, dp_ratio, vp_dimensions));
		}
		else if (parent_values)
		{
			// Line height has a special inheritance case for numbers/percent: they inherit them directly instead of computed length, but for lengths,
			// they inherit the length. See CSS specs for details. Percent is already converted to number.
			if (parent_values->line_height().inherit_type == Style::LineHeight::Number)
				values.line_height(Style::LineHeight(font_size * parent_values->line_height().inherit_value, Style::LineHeight::Number,
					parent_values->line_height().inherit_value));
			else
				values.line_height(parent_values->line_height());
		}
		if (const Property* transitioned = TransitionPrePass(PropertyId::LineHeight, transition_context))
			values.line_height(ComputeLineHeight(transitioned, font_size, document_font_size, dp_ratio, vp_dimensions));

		if (line_height_before.value != values.line_height().value || line_height_before.inherit_value != values.line_height().inherit_value)
			dirty_properties.Insert(PropertyId::VerticalAlign);
	}
	else
	{
		values.line_height(line_height_before);
	}

	bool dirty_font_face_handle = false;

	// Values (p3-values): 'currentcolor' reads the color, computed at its first use.
	bool color_computed = false;
	Property current_color_storage;

	// The border colors this element declares; the others are 'currentcolor', their initial value.
	int declared_border_colors = 0;

	for (auto it = Iterate(); !it.AtEnd(); ++it)
	{
		variable_dependencies.clear();
		auto id_property_pair = *it;
		const PropertyId id = id_property_pair.first;
		const Property* property = ResolveVariables(id, &id_property_pair.second, variable_dependencies, property_storage);
#ifdef RMLUI_MATH_EXPRESSIONS
		// The cache is rebuilt from scratch above and only exists to retain dependencies hidden behind
		// variable/shorthand substitution. Ordinary specified values expose their units directly and
		// need no cache entry or per-property maintenance on this hot path.
		if (id_property_pair.second.unit == Unit::VAR_EXPRESSION || id_property_pair.second.unit == Unit::SHORTHAND_PLACEHOLDER)
			UpdateResolvedUnitDependencies(id, id_property_pair.second, property);
#endif
		if (!property)
			continue;

		// A CSS-wide keyword: the declaration it stands for, computed with the font size of the element that declares it.
		float property_font_size = font_size;
		if (property->unit == Unit::CSS_WIDE_KEYWORD)
		{
			const ComputedKeyword keyword = ResolveComputedKeyword(id, *property);
			if (keyword.explicit_inherit)
			{
				if (!explicit_inherit)
					explicit_inherit = MakeUnique<PropertyIdSet>();
				explicit_inherit->Insert(id);
			}
			if (!keyword.property)
				continue;
			property = keyword.property;
			if (keyword.element)
			{
				property = keyword.element->GetStyle()->ResolveVariables(id, property, variable_dependencies, property_storage);
				if (!property)
					continue;
				property_font_size = keyword.element->GetComputedValues().font_size();
			}
		}

		for (const String& variable_name : variable_dependencies)
		{
			if (dirty_variables.count(variable_name) != 0)
				dirty_properties.Insert(id);
		}

		if (dirty_em_properties
#ifdef RMLUI_MATH_EXPRESSIONS
			&& Any(GetMutableUnitDependencies(*property) & Unit::EM)
#else
			&& property->unit == Unit::EM
#endif
		)
			dirty_properties.Insert(id);

		if (id >= PropertyId::BorderTopColor && id <= PropertyId::BorderLeftColor)
			declared_border_colors |= 1 << (int(id) - int(PropertyId::BorderTopColor));

		// Values (p3-values): 'currentcolor', relative font weights.
		property = ResolveValueKeywords(id, property, values, parent_values, color_computed, current_color_storage);

		ComputeValue(values, dp_ratio, vp_dimensions, property_font_size, document_font_size, dirty_font_face_handle, id, property);
	}

	// 'overflow' computes from both axes (p3-layout).
	ApplyOverflowFixUp(values);
	// Transitions start on any change of a computed value (p3-animation).
	TransitionChangedProperties(transition_context, dirty_properties, font_size, document_font_size, dirty_font_face_handle);

	// 'currentcolor' border colors follow the color, also when only the color changed.
	static_assert(int(PropertyId::BorderLeftColor) - int(PropertyId::BorderTopColor) == 3, "The border colors must be consecutive ids.");
	if (declared_border_colors != 0xf)
	{
		const bool color_changed = dirty_properties.Contains(PropertyId::Color);
		const Colourb color = values.color();
		for (int side = 0; side < 4; side++)
		{
			if (declared_border_colors & (1 << side))
				continue;
			const PropertyId id = PropertyId(int(PropertyId::BorderTopColor) + side);
			switch (id)
			{
			case PropertyId::BorderTopColor: values.border_top_color(color); break;
			case PropertyId::BorderRightColor: values.border_right_color(color); break;
			case PropertyId::BorderBottomColor: values.border_bottom_color(color); break;
			default: values.border_left_color(color); break;
			}
			if (color_changed)
				dirty_properties.Insert(id);
		}
	}

	// The font-face handle is nulled when local font properties are set. In that case we need to retrieve a new handle.
	if (dirty_font_face_handle)
	{
		RMLUI_ZoneScopedN("FontFaceHandle");
		values.font_face_handle(
			GetFontEngineInterface()->GetFontFaceHandle(values.font_family(), values.font_style(), values.font_weight(), Math::RoundToInteger(values.font_size())));
	}

	// Values (p3-values): the values that depend on the font face.
	ComputeFontRelativeValues(values, font_unit_scope, values_are_default_initialized ? FontFaceHandle(0) : font_face_before, loop_font_face, dp_ratio,
		vp_dimensions, font_size, document_font_size, property_storage);

	// Next, pass inheritable dirty properties onto our children
	PropertyIdSet dirty_inherited_properties = (dirty_properties & StyleSheetSpecification::GetRegisteredInheritedProperties());

	// Special case for text-overflow: It's not really inherited, but the value is used by the element's children. Insert
	// it here so they are notified of a change.
	if (dirty_properties.Contains(PropertyId::TextOverflow))
		dirty_inherited_properties.Insert(PropertyId::TextOverflow);

	// Propagate dirty inherited properties to children. Var shorthands are local to the element and never inherited.
	// Children that declare a non-inherited property 'inherit' follow it too.
	if (!dirty_inherited_properties.Empty() || !dirty_variables.empty() || !dirty_properties.Empty())
	{
		for (int i = 0; i < element->GetNumChildren(true); i++)
		{
			ElementStyle* child_style = element->GetChild(i)->GetStyle();
			child_style->dirty_properties |= dirty_inherited_properties;
			if (child_style->explicit_inherit)
				child_style->dirty_properties |= (dirty_properties & *child_style->explicit_inherit);
			child_style->dirty_variables.insert(dirty_variables.begin(), dirty_variables.end());
		}
	}

	PropertyIdSet result(std::move(dirty_properties));
	dirty_properties.Clear();
	dirty_variables.clear();
	dirty_var_shorthands.clear();
	return result;
}

ElementStyle::PropertySources ElementStyle::GetPropertySources() const
{
	return {element, definition.get(), inline_properties, animation_layers.get()};
}

void ElementStyle::SetTransitionProperty(PropertyId id, const Property& property)
{
	if (!animation_layers)
		animation_layers = MakeUnique<AnimationLayers>();
	animation_layers->transition_properties.SetProperty(id, property);
	DirtyProperty(id);
}

void ElementStyle::RemoveTransitionProperty(PropertyId id)
{
	if (!animation_layers || !animation_layers->transition_properties.GetProperty(id))
		return;
	animation_layers->transition_properties.RemoveProperty(id);
	DirtyProperty(id);
}

void ElementStyle::SetAnimatedProperty(PropertyId id, const Property& property)
{
	if (!animation_layers)
		animation_layers = MakeUnique<AnimationLayers>();
	animation_layers->animated_properties.SetProperty(id, property);
	DirtyProperty(id);
}

void ElementStyle::RemoveAnimatedProperty(PropertyId id)
{
	if (!animation_layers || !animation_layers->animated_properties.GetProperty(id))
		return;
	animation_layers->animated_properties.RemoveProperty(id);
	DirtyProperty(id);
}

const TransitionList* ElementStyle::GetTransitionList() const
{
	return (animation_style && !animation_style->transitions.none) ? &animation_style->transitions : nullptr;
}

const AnimationList* ElementStyle::GetAnimationList() const
{
	return (animation_style && !animation_style->animations.empty()) ? &animation_style->animations : nullptr;
}

void ElementStyle::UpdateAnimationLists(const PropertyIdSet& dirty)
{
	const bool transitions_dirty = !(dirty & GetTransitionLonghandIds()).Empty();
	const bool animations_dirty = !(dirty & GetAnimationLonghandIds()).Empty();
	if (!transitions_dirty && !animations_dirty)
		return;

	if (!animation_style)
		animation_style = MakeUnique<ElementAnimationStyle>();
	if (transitions_dirty)
		animation_style->transitions = BuildTransitionList(*this);
	if (animations_dirty)
		animation_style->animations = BuildAnimationList(*this);
	if (animation_style->transitions.none && animation_style->animations.empty())
		animation_style.reset();
}

const PropertyDictionary* ElementStyle::GetTransitionProperties() const
{
	return animation_layers ? &animation_layers->transition_properties : nullptr;
}

const PropertyDictionary* ElementStyle::GetAnimatedProperties() const
{
	return animation_layers ? &animation_layers->animated_properties : nullptr;
}

ElementStyle::DirtyPropertiesRef ElementStyle::GetDirtyPropertiesRef()
{
	return {dirty_properties, dirty_variables, dirty_var_shorthands};
}

void ElementStyle::DirtyProperties(const PropertyIdSet& properties)
{
	dirty_properties |= properties;
}

const Property* ElementStyle::ResolveVariables(PropertyId id, const Property* property, SmallUnorderedSet<String>& variable_dependencies,
	Property& property_storage) const
{
	return ResolveVariables(GetPropertySources(), expanded_shorthands, id, property, variable_dependencies, property_storage);
}

void ElementStyle::ComputeValue(Style::ComputedValues& values, float dp_ratio, Vector2f vp_dimensions, const float font_size,
	const float document_font_size, bool& dirty_font_face_handle, const PropertyId id, const Property* p)
{
	using namespace Style;

	// clang-format off
	switch (id)
	{
	case PropertyId::MarginTop:
		values.margin_top(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MarginRight:
		values.margin_right(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MarginBottom:
		values.margin_bottom(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MarginLeft:
		values.margin_left(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::PaddingTop:
		values.padding_top(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::PaddingRight:
		values.padding_right(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::PaddingBottom:
		values.padding_bottom(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::PaddingLeft:
		values.padding_left(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::BorderTopWidth:
		values.border_top_width(ComputeLineWidth(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::BorderRightWidth:
		values.border_right_width(ComputeLineWidth(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::BorderBottomWidth:
		values.border_bottom_width(ComputeLineWidth(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::BorderLeftWidth:
		values.border_left_width(ComputeLineWidth(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::BorderTopColor:
		values.border_top_color(p->Get<Colourb>());
		break;
	case PropertyId::BorderRightColor:
		values.border_right_color(p->Get<Colourb>());
		break;
	case PropertyId::BorderBottomColor:
		values.border_bottom_color(p->Get<Colourb>());
		break;
	case PropertyId::BorderLeftColor:
		values.border_left_color(p->Get<Colourb>());
		break;

	case PropertyId::BorderTopLeftRadius:
	case PropertyId::BorderTopRightRadius:
	case PropertyId::BorderBottomRightRadius:
	case PropertyId::BorderBottomLeftRadius:
		// Not registered: each corner is a shorthand of its horizontal and vertical radius (p3-paint section below).
		break;

	case PropertyId::Display:
		values.display((Display)p->Get<int>());
		break;
	case PropertyId::Position:
		values.position((Position)p->Get<int>());
		break;

	case PropertyId::Top:
		values.top(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::Right:
		values.right(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::Bottom:
		values.bottom(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::Left:
		values.left(ComputeLengthPercentageAuto(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::Float:
		values.float_((Float)p->Get<int>());
		break;
	case PropertyId::Clear:
		values.clear((Clear)p->Get<int>());
		break;
	case PropertyId::BoxSizing:
		values.box_sizing((BoxSizing)p->Get<int>());
		break;

	case PropertyId::ZIndex:
		if (p->unit == Unit::KEYWORD) values.z_index(ZIndex(ZIndex::Auto));
		else { float value; if (ComputeDefiniteNumber(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.z_index(ZIndex(ZIndex::Number, value)); }
		break;

	case PropertyId::Width:
		values.width(ComputeSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MinWidth:
		values.min_width(ComputeSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MaxWidth:
		values.max_width(ComputeMaxSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::Height:
		values.height(ComputeSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MinHeight:
		values.min_height(ComputeSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::MaxHeight:
		values.max_height(ComputeMaxSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::LineHeight:
		// (Line-height computed above)
		break;
	case PropertyId::VerticalAlign:
		values.vertical_align(ComputeVerticalAlign(p, values.line_height().value, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::OverflowX:
		values.overflow_x((Overflow)p->Get< int >());
		break;
	case PropertyId::OverflowY:
		values.overflow_y((Overflow)p->Get< int >());
		break;
	case PropertyId::Clip:
		values.clip(ComputeClip(p));
		break;
	case PropertyId::Visibility:
		values.visibility((Visibility)p->Get< int >());
		break;
	case PropertyId::TextOverflow:
		values.text_overflow(p->unit == Unit::KEYWORD ? p->Get<TextOverflow>() : TextOverflow::String);
		break;

	case PropertyId::BackgroundColor:
		values.background_color(p->Get<Colourb>());
		break;
	case PropertyId::Color:
		values.color(p->Get<Colourb>());
		break;
	case PropertyId::ImageColor:
		values.image_color(p->Get<Colourb>());
		break;
	case PropertyId::Opacity:
		// Not inherited: below 1 the element and its descendants are drawn as a group (phase 3, p3-paint).
		{ float value; if (ComputeDefiniteNumber(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.opacity(Math::Clamp(value, 0.f, 1.f)); }
		break;

	case PropertyId::FontFamily:
		// Fetched from element's properties.
		dirty_font_face_handle = true;
		break;
	case PropertyId::FontStyle:
		values.font_style((FontStyle)p->Get< int >());
		dirty_font_face_handle = true;
		break;
	case PropertyId::FontWeight:
		if (p->unit == Unit::KEYWORD)
			values.font_weight((FontWeight)p->Get<int>());
		else
		{
			float value;
			if (ComputeDefiniteNumber(p, font_size, document_font_size, dp_ratio, vp_dimensions, value))
				values.font_weight((FontWeight)static_cast<int>(value));
		}
		dirty_font_face_handle = true;
		break;
	case PropertyId::FontSize:
		// (font-size computed above)
		dirty_font_face_handle = true;
		break;
	case PropertyId::FontKerning:
		values.font_kerning((FontKerning)p->Get<int>());
		dirty_font_face_handle = true;
		break;
	case PropertyId::LetterSpacing:
		values.has_letter_spacing(p->unit != Unit::KEYWORD);
		values.letter_spacing(ComputeTextSpacing(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		dirty_font_face_handle = true;
		break;

	case PropertyId::TextAlign:
		values.text_align(ComputeTextAlign(p, element));
		values.text_align_legacy(ComputeTextAlignLegacy(p, element));
		break;
	case PropertyId::TextDecoration:
		values.text_decoration((TextDecoration)p->Get<int>());
		break;
	case PropertyId::TextTransform:
		values.text_transform((TextTransform)p->Get<int>());
		break;
	case PropertyId::WhiteSpace:
		// Not registered: 'white-space' is the shorthand of the text section's longhands.
		break;
	case PropertyId::WordBreak:
		values.word_break((WordBreak)p->Get<int>());
		break;

	case PropertyId::RowGap:
		values.row_gap(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::ColumnGap:
		values.column_gap(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::Drag:
		values.drag((Drag)p->Get< int >());
		break;
	case PropertyId::TabIndex:
		values.tab_index((TabIndex)p->Get< int >());
		break;
	case PropertyId::Focus:
		values.focus((Focus)p->Get<int>());
		break;
	case PropertyId::ScrollbarMargin:
		{ float value; if (ComputeDefiniteLength(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.scrollbar_margin(value); }
		break;
	case PropertyId::OverscrollBehavior:
		values.overscroll_behavior((OverscrollBehavior)p->Get<int>());
		break;
	case PropertyId::PointerEvents:
		values.pointer_events((PointerEvents)p->Get<int>());
		break;

	case PropertyId::Perspective:
		if (p->unit == Unit::KEYWORD) values.perspective(0.f);
		else { float value; if (ComputeDefiniteLength(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.perspective(value); }
		values.has_local_perspective(values.perspective() > 0.f);
		break;
	case PropertyId::PerspectiveOriginX:
		values.perspective_origin_x(ComputeOrigin(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::PerspectiveOriginY:
		values.perspective_origin_y(ComputeOrigin(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::Transform:
		values.has_local_transform(p->Get<TransformPtr>() != nullptr);
		break;
	case PropertyId::TransformOriginX:
		values.transform_origin_x(ComputeOrigin(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::TransformOriginY:
		values.transform_origin_y(ComputeOrigin(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::TransformOriginZ:
		{ float value; if (ComputeDefiniteLength(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.transform_origin_z(value); }
		break;

	case PropertyId::Decorator:
		values.has_decorator(p->unit == Unit::DECORATOR && p->value.GetType() == Variant::DECORATORSPTR && p->value.GetReference<DecoratorsPtr>());
		break;
	case PropertyId::MaskImage:
		values.has_mask_image(HasImageLayers(*p));
		break;
	case PropertyId::FontEffect:
		values.has_font_effect(p->unit == Unit::FONTEFFECT && p->value.GetType() == Variant::FONTEFFECTSPTR && p->value.GetReference<FontEffectsPtr>());
		break;
	case PropertyId::Filter:
		values.has_filter(p->unit == Unit::FILTER && p->value.GetType() == Variant::FILTERSPTR && p->value.GetReference<FiltersPtr>());
		break;
	case PropertyId::BackdropFilter:
		values.has_backdrop_filter(p->unit == Unit::FILTER && p->value.GetType() == Variant::FILTERSPTR && p->value.GetReference<FiltersPtr>());
		break;
	case PropertyId::BoxShadow:
		values.has_box_shadow(p->unit == Unit::BOXSHADOWLIST && p->value.GetType() == Variant::BOXSHADOWLIST && !p->value.GetReference<BoxShadowList>().empty());
		break;

	case PropertyId::FlexBasis:
		values.flex_basis(ComputeSizingProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;

	case PropertyId::RmlUi_Language:
		values.language(p->Get<String>());
		break;
	case PropertyId::RmlUi_Direction:
		values.direction(p->Get<Direction>());
		break;

	// Fetched from element's properties.
	case PropertyId::Cursor:
	case PropertyId::Transition:
	case PropertyId::Animation:
	case PropertyId::AlignContent:
	case PropertyId::AlignItems:
	case PropertyId::AlignSelf:
	case PropertyId::FlexDirection:
	case PropertyId::FlexGrow:
	case PropertyId::FlexShrink:
	case PropertyId::FlexWrap:
	case PropertyId::JustifyContent:
		break;
	// Navigation properties. Must be manually retrieved with 'GetProperty()'.
	case PropertyId::NavUp:
	case PropertyId::NavDown:
	case PropertyId::NavLeft:
	case PropertyId::NavRight:
		break;
	// Unhandled properties. Must be manually retrieved with 'GetProperty()'.
	case PropertyId::FillImage:
	case PropertyId::CaretColor:
		break;

	/*
	  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3). Every id of ID.h has its case in the section
	  of the package that implements it, so -Wswitch stays clean; a package edits its own section only. Ids that are
	  not registered yet never reach this function.
	*/

	// -- Groundwork (p3-base) --
	case PropertyId::Order:
		values.order(p->Get<int>());
		break;
	case PropertyId::JustifyItems:
	case PropertyId::JustifySelf:
		break;
	case PropertyId::WillChange:
		values.will_change(p->unit == Unit::STRING ? WillChange::ParseHints(p->Get<String>()) : uint8_t(0));
		break;

	// -- Values (p3-values) --

	// -- Cascade (p3-cascade) --
	case PropertyId::Content:
	case PropertyId::Quotes:
	case PropertyId::CounterReset:
	case PropertyId::CounterIncrement:
	case PropertyId::CounterSet:
	case PropertyId::ListStyleType:
	case PropertyId::ListStylePosition:
	case PropertyId::ListStyleImage:
		break;

	// -- Paint (p3-paint) --
	case PropertyId::BorderTopStyle:
		values.border_top_style((BorderStyle)p->Get<int>());
		break;
	case PropertyId::BorderRightStyle:
		values.border_right_style((BorderStyle)p->Get<int>());
		break;
	case PropertyId::BorderBottomStyle:
		values.border_bottom_style((BorderStyle)p->Get<int>());
		break;
	case PropertyId::BorderLeftStyle:
		values.border_left_style((BorderStyle)p->Get<int>());
		break;
	case PropertyId::RmlUi_BorderTopLeftRadiusX:
	case PropertyId::RmlUi_BorderTopLeftRadiusY:
	case PropertyId::RmlUi_BorderTopRightRadiusX:
	case PropertyId::RmlUi_BorderTopRightRadiusY:
	case PropertyId::RmlUi_BorderBottomRightRadiusX:
	case PropertyId::RmlUi_BorderBottomRightRadiusY:
	case PropertyId::RmlUi_BorderBottomLeftRadiusX:
	case PropertyId::RmlUi_BorderBottomLeftRadiusY:
		{
			const int index = int(id) - int(PropertyId::RmlUi_BorderTopLeftRadiusX);
			values.border_radius(index / 2, index % 2, ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		}
		break;
	case PropertyId::OutlineWidth:
		values.outline_width(ComputeLineWidth(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::OutlineStyle:
		values.outline_style((BorderStyle)p->Get<int>());
		break;
	case PropertyId::OutlineColor:
		if (p->unit == Unit::KEYWORD)
			values.outline_color_current();
		else
			values.outline_color(p->Get<Colourb>());
		break;
	case PropertyId::OutlineOffset:
		{ float value; if (ComputeDefiniteLength(p, font_size, document_font_size, dp_ratio, vp_dimensions, value)) values.outline_offset(value); }
		break;
	case PropertyId::Isolation:
		values.isolation((Isolation)p->Get<int>());
		break;
	case PropertyId::BackgroundImage:
		values.has_background_image(HasImageLayers(*p));
		break;
	case PropertyId::BackgroundPositionX:
	case PropertyId::BackgroundPositionY:
	case PropertyId::BackgroundSize:
	case PropertyId::BackgroundRepeat:
	case PropertyId::BackgroundOrigin:
	case PropertyId::BackgroundClip:
	case PropertyId::BackgroundAttachment:
	case PropertyId::MaskSize:
	case PropertyId::MaskPositionX:
	case PropertyId::MaskPositionY:
	case PropertyId::MaskRepeat:
		// Read by the layers from the element's properties.
		break;
	case PropertyId::ObjectFit:
		values.object_fit((ObjectFit)p->Get<int>());
		break;
	case PropertyId::ObjectPositionX:
		values.object_position_x(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::ObjectPositionY:
		values.object_position_y(ComputeLengthPercentage(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::BorderImage:
		break;

	// -- Text (p3-text) --
	case PropertyId::WordSpacing:
		values.word_spacing(ComputeTextSpacing(p, font_size, document_font_size, dp_ratio, vp_dimensions));
		break;
	case PropertyId::OverflowWrap:
		values.overflow_wrap((OverflowWrap)p->Get<int>());
		break;
	case PropertyId::WhiteSpaceCollapse:
		values.white_space_collapse((WhiteSpaceCollapse)p->Get<int>());
		break;
	case PropertyId::TextWrapMode:
		values.text_wrap_mode((TextWrapMode)p->Get<int>());
		break;
	case PropertyId::TextShadow:
		values.has_text_shadow(p->unit == Unit::STYLE_VALUE);
		break;
	// Fetched from the element's properties (ElementText, WidgetTextInput).
	case PropertyId::TextDecorationColor:
	case PropertyId::TextDecorationStyle:
	case PropertyId::TextDecorationThickness:
	case PropertyId::TextUnderlineOffset:
	case PropertyId::UserSelect:
	case PropertyId::AccentColor:
		break;

	// -- Layout (p3-layout) --
	case PropertyId::OverflowClipMargin:
	case PropertyId::AspectRatio:
	case PropertyId::ScrollbarGutter:
		ComputeLayoutProperty(id, p, values, font_size, document_font_size, dp_ratio, vp_dimensions);
		break;
	case PropertyId::FlexLineCount:
		break;

	// -- Animation (p3-animation) --
	case PropertyId::TransitionProperty:
	case PropertyId::TransitionDuration:
	case PropertyId::TransitionTimingFunction:
	case PropertyId::TransitionDelay:
	case PropertyId::TransitionBehavior:
	case PropertyId::AnimationName:
	case PropertyId::AnimationDuration:
	case PropertyId::AnimationTimingFunction:
	case PropertyId::AnimationDelay:
	case PropertyId::AnimationIterationCount:
	case PropertyId::AnimationDirection:
	case PropertyId::AnimationFillMode:
	case PropertyId::AnimationPlayState:
	case PropertyId::AnimationComposition:
		break;
	case PropertyId::Translate:
		values.has_translate(p->unit == Unit::TRANSFORM && p->Get<TransformPtr>() != nullptr);
		break;
	case PropertyId::Rotate:
		values.has_rotate(p->unit == Unit::TRANSFORM && p->Get<TransformPtr>() != nullptr);
		break;
	case PropertyId::Scale:
		values.has_scale(p->unit == Unit::TRANSFORM && p->Get<TransformPtr>() != nullptr);
		break;

	// -- Grid (p3-grid) --
	case PropertyId::GridTemplateColumns:
	case PropertyId::GridTemplateRows:
	case PropertyId::GridTemplateAreas:
	case PropertyId::GridAutoColumns:
	case PropertyId::GridAutoRows:
	case PropertyId::GridAutoFlow:
	case PropertyId::GridRowStart:
	case PropertyId::GridRowEnd:
	case PropertyId::GridColumnStart:
	case PropertyId::GridColumnEnd:
		break;

	// Invalid properties
	case PropertyId::Invalid:
	case PropertyId::NumDefinedIds:
	case PropertyId::MaxNumIds:
		break;
	}
	// clang-format on
}
} // namespace Rml
