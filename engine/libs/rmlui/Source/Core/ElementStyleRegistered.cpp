#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ComputeProperty.h"
#include "ElementDefinition.h"
#include "ElementStyle.h"
#include "ElementTransitions.h"
#include "PropertiesIterator.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Rml {

/*
    Registered custom properties (fork, phase 5; CSS Properties and Values API 1): the parts of ElementStyle for them, in
    a file of their own so that the code of ElementStyle.cpp (ComputeValues above all) is compiled as before for documents
    without registrations. FORK.md P5-1.
*/

namespace {
	// Registered custom properties (fork, phase 5): computed values and their serialization for var().

	// The length context of an element: its font size, its document's, its context's dp ratio and viewport.
	struct RegisteredLengthContext {
		float font_size = 16.f;
		float document_font_size = 16.f;
		float dp_ratio = 1.f;
		Vector2f vp_dimensions = Vector2f(1.f);
	};
	RegisteredLengthContext LengthContextOf(Element* element)
	{
		RegisteredLengthContext context;
		context.font_size = DefaultComputedValues().font_size();
		context.document_font_size = context.font_size;
		if (!element)
			return context;
		context.font_size = element->GetComputedValues().font_size();
		if (ElementDocument* document = element->GetOwnerDocument())
			context.document_font_size = document->GetComputedValues().font_size();
		if (Context* element_context = element->GetContext())
		{
			context.dp_ratio = element_context->GetDensityIndependentPixelRatio();
			context.vp_dimensions = Vector2f(element_context->GetDimensions());
		}
		return context;
	}

	// The shortest decimal text that reads back as the same float (no exponent but for very large or small numbers).
	String NumberText(float number)
	{
		if (number == 0.f)
			return "0";
		char buffer[64];
		if (std::fabs(number) < 1e9f)
		{
			for (int decimals = 0; decimals <= 9; decimals++)
			{
				snprintf(buffer, sizeof(buffer), "%.*f", decimals, double(number));
				if (strtof(buffer, nullptr) == number)
					return String(buffer);
			}
		}
		snprintf(buffer, sizeof(buffer), "%.9g", double(number));
		return String(buffer);
	}
} // namespace


Property ElementStyle::ComputedRegisteredValue(const Property& property, RegisteredPropertySyntax syntax, Element* element)
{
	Property result = property;
	switch (syntax)
	{
	case RegisteredPropertySyntax::Integer:
	{
		float number = 0.f;
		if (property.unit == Unit::NUMBER)
			number = property.Get<float>();
		else
		{
			const RegisteredLengthContext context = LengthContextOf(element);
			if (!ComputeDefiniteNumber(&property, context.font_size, context.document_font_size, context.dp_ratio, context.vp_dimensions, number))
				break;
		}
		// The nearest integer, halves towards positive infinity (CSS Values 4 §10.12, as interpolation rounds).
		result = Property(std::floor(number + 0.5f), Unit::NUMBER);
		break;
	}
	case RegisteredPropertySyntax::Number:
		if (property.unit != Unit::NUMBER)
		{
			const RegisteredLengthContext context = LengthContextOf(element);
			float number = 0.f;
			if (ComputeDefiniteNumber(&property, context.font_size, context.document_font_size, context.dp_ratio, context.vp_dimensions, number))
				result = Property(number, Unit::NUMBER);
		}
		break;
	case RegisteredPropertySyntax::Length:
	case RegisteredPropertySyntax::LengthPercentage:
		if (property.unit != Unit::PX && property.unit != Unit::PERCENT)
		{
			// Relative lengths (and expressions of lengths) in px, against the element the value is computed for.
			const RegisteredLengthContext context = LengthContextOf(element);
			float length = 0.f;
			if (ComputeDefiniteLength(&property, context.font_size, context.document_font_size, context.dp_ratio, context.vp_dimensions, length))
				result = Property(length, Unit::PX);
		}
		break;
	case RegisteredPropertySyntax::Angle:
		if (Any(property.unit & Unit::ANGLE) && property.unit != Unit::DEG)
			result = Property(ComputeAngle(property.GetNumericValue()) * (180.f / Math::RMLUI_PI), Unit::DEG);
		break;
	case RegisteredPropertySyntax::Universal:
	case RegisteredPropertySyntax::Percentage:
	case RegisteredPropertySyntax::Color:
	case RegisteredPropertySyntax::Time: break;
	}
	result.definition = property.definition;
	return result;
}

String ElementStyle::SerializeRegisteredValue(const Property& computed, RegisteredPropertySyntax syntax)
{
	switch (computed.unit)
	{
	case Unit::STRING: return computed.Get<String>();
	case Unit::COLOUR:
	{
		const Colourb colour = computed.Get<Colourb>();
		char buffer[16];
		snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", colour.red, colour.green, colour.blue, colour.alpha);
		return String(buffer);
	}
	case Unit::NUMBER: return NumberText(computed.Get<float>()) + (syntax == RegisteredPropertySyntax::Time ? "s" : "");
	case Unit::PERCENT: return NumberText(computed.Get<float>()) + "%";
	case Unit::PX: return NumberText(computed.Get<float>()) + "px";
	case Unit::DEG: return NumberText(computed.Get<float>()) + "deg";
	default: break;
	}
	// 'currentcolor' (and color-mix() of it), expressions with percentages: as written.
	return computed.ToString();
}

bool ElementStyle::SubstituteRegisteredProperty(const PropertySources& sources, PropertyId id, SmallUnorderedSet<String>& variable_dependencies,
	SmallUnorderedSet<String>& cycle_chain, String& result)
{
	const RegisteredCustomProperty* registration = StyleSheetSpecification::GetRegisteredCustomProperty(id);
	const PropertyDefinition* definition = StyleSheetSpecification::GetProperty(id);
	if (!registration || !definition)
		return false;

	Property storage;
	PropertyElementPair pair = GetSpecifiedProperty(sources, id);
	// (Each turn moves up the tree or to the initial value.)
	while (pair.property)
	{
		if (!pair.element)
		{
			// The initial value.
			if (!registration->has_initial_value)
				return false;
			result = SerializeRegisteredValue(ComputedRegisteredValue(*pair.property, registration->syntax, sources.element), registration->syntax);
			return true;
		}

		const Property* value = pair.property;
		if (value->unit == Unit::VAR_EXPRESSION)
		{
			value = nullptr;
			const String& text = pair.property->Get<String>();
			if (text.find("var(") != String::npos)
			{
				// The element's own declarations as given (a starting style, a before-change style), an ancestor's as they are.
				const bool same_element = (pair.element == sources.element);
				const PropertySources element_sources = (same_element ? sources : GetLookupSources(pair.element));
				// (on another element the chain starts again, with the property itself as its first link)
				SmallUnorderedSet<String> new_cycle_chain;
				if (!same_element)
					new_cycle_chain.insert(StyleSheetSpecification::GetPropertyName(id));
				String substituted;
				if (SubstituteVariables(element_sources, text, variable_dependencies, same_element ? cycle_chain : new_cycle_chain, substituted) &&
					definition->ParseValue(storage, substituted))
					value = &storage;
				// A property in a var() cycle is invalid at computed-value time, also when its var() got a value.
				if (!registered_cycle_root.empty())
				{
					value = nullptr;
					if (registered_cycle_root == StyleSheetSpecification::GetPropertyName(id))
						registered_cycle_root.clear();
				}
			}
		}
		if (value)
		{
			result = SerializeRegisteredValue(ComputedRegisteredValue(*value, registration->syntax, pair.element), registration->syntax);
			return true;
		}

		// Invalid at computed-value time: the property is 'unset'.
		Element* parent = (registration->inherits ? pair.element->GetParentNode() : nullptr);
		if (parent)
			pair = GetSpecifiedProperty(GetLookupSources(parent), id);
		else
			pair = {definition->GetDefaultValue(), nullptr};
	}
	return false;
}

// The work of ComputeValues for registered custom properties, out of line so that its common path stays as it is for
// documents without registrations.
#if defined(_MSC_VER)
	#define RMLUI_REGISTERED_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
	#define RMLUI_REGISTERED_NOINLINE __attribute__((noinline))
#else
	#define RMLUI_REGISTERED_NOINLINE
#endif

RMLUI_REGISTERED_NOINLINE void ElementStyle::NoteDirtyRegisteredProperties()
{
	const PropertyIdSet registered_dirty = dirty_properties & StyleSheetSpecification::GetRegisteredCustomProperties();
	for (const PropertyId id : registered_dirty)
		dirty_variables.insert(StyleSheetSpecification::GetPropertyName(id));
}

RMLUI_REGISTERED_NOINLINE void ElementStyle::NoteEmRegisteredProperties()
{
	for (const PropertyId id : StyleSheetSpecification::GetRegisteredCustomProperties())
	{
		const Property* local = GetLocalProperty(id);
#ifdef RMLUI_MATH_EXPRESSIONS
		const bool em = local && Any(GetMutableUnitDependencies(*local) & Unit::EM);
#else
		const bool em = local && local->unit == Unit::EM;
#endif
		if (!em)
			continue;
		dirty_properties.Insert(id);
		dirty_variables.insert(StyleSheetSpecification::GetPropertyName(id));
	}
}

RMLUI_REGISTERED_NOINLINE void ElementStyle::RecomputeRegisteredDependents(const ComputeContext& context, float font_size,
	float document_font_size, bool& dirty_font_face_handle)
{
	Style::ComputedValues& values = context.values;
	// (after the main loop: 'currentcolor' is the computed color)
	bool color_computed = true;
	Property current_color_storage;
	SmallUnorderedSet<String> applied_names;
	for (const PropertyId id : transition_snapshot->registered_applied)
	{
		const String& name = StyleSheetSpecification::GetPropertyName(id);
		applied_names.insert(name);
		dirty_variables.insert(name);
	}
	if (!expanded_shorthands.GetProperties().empty() || !inline_properties.GetVarShorthands().empty() ||
		(definition && !definition->GetProperties().GetVarShorthands().empty()))
		ExpandVarShorthands(expanded_shorthands, GetPropertySources(), GetDirtyPropertiesRef());
	SmallUnorderedSet<String> variable_dependencies;
	Property property_storage;
	for (auto it = Iterate(); !it.AtEnd(); ++it)
	{
		const auto id_property_pair = *it;
		const PropertyId id = id_property_pair.first;
		if (id_property_pair.second.unit != Unit::VAR_EXPRESSION && id_property_pair.second.unit != Unit::SHORTHAND_PLACEHOLDER)
			continue;
		variable_dependencies.clear();
		const Property* property = ResolveVariables(id, &id_property_pair.second, variable_dependencies, property_storage);
		if (!property || property->unit == Unit::CSS_WIDE_KEYWORD)
			continue;
		const bool reads_applied = std::any_of(variable_dependencies.begin(), variable_dependencies.end(),
			[&](const String& name) { return applied_names.count(name) != 0; });
		if (!reads_applied)
			continue;
		dirty_properties.Insert(id);
		if (property->unit == Unit::KEYWORD || property->unit == Unit::STYLE_VALUE || property->unit == Unit::BOXSHADOWLIST ||
			property->unit == Unit::DECORATOR)
			property = ResolveValueKeywords(id, property, values, context.parent_values, color_computed, current_color_storage);
		ComputeValue(values, context.dp_ratio, context.vp_dimensions, font_size, document_font_size, dirty_font_face_handle, id, property);
	}
}

RMLUI_REGISTERED_NOINLINE void ElementStyle::ForgetRegisteredDirtyVariables()
{
	for (auto it = dirty_variables.begin(); it != dirty_variables.end();)
	{
		if (StyleSheetSpecification::IsRegisteredCustomProperty(StyleSheetSpecification::GetPropertyId(*it)))
			it = dirty_variables.erase(it);
		else
			++it;
	}
}

} // namespace Rml
