#include "ElementTransitions.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "ComputeProperty.h"
#include "ElementAnimation.h"
#include "ElementDefinition.h"
#include "ElementStyle.h"
#include "PropertiesIterator.h"
#include "PropertyParserAnimation.h"

namespace Rml {

/*
    Transitions on any change of a computed value (CSS Transitions 1 §3, phase-3 spec 3.13.4).

    The element keeps the after-change value of each property its transition list covers (TransitionSnapshot). At each
    style computation the changed properties are compared, in computed form, with these values: the before-change values.
    A value is taken from the element's declarations and, for inherited properties, its ancestors', without any animation
    layer, so that the values the element's or an ancestor's transitions and animations apply never start a transition;
    such changes are not style changes in CSS either. When a value differs, the CSS algorithm starts a transition, reverses
    the running one (the reversing shortening factor), or cancels it.

    An element styled for the first time, or no longer 'display: none' (it or an ancestor, at the previous update), has no
    before-change style: it transitions only from its starting style, given by @starting-style rules. The style sheet
    parser stores those rules with a pseudo-class on their subject that only the starting style sets.
*/

static bool starting_style_rules = false;

const char* GetStartingStyleSelectorSuffix()
{
	return ":-rmlui-starting-style";
}

const String& GetStartingStylePseudoClass()
{
	static const String name = "-rmlui-starting-style";
	return name;
}

void NoteStartingStyleRules()
{
	starting_style_rules = true;
}

bool HasStartingStyleRules()
{
	return starting_style_rules;
}

namespace {

	// The properties a transition never applies to: the transition and animation longhands.
	bool IsAnimationProperty(PropertyId id)
	{
		return GetTransitionLonghandIds().Contains(id) || GetAnimationLonghandIds().Contains(id);
	}

	bool IsNumeric(const Property& property)
	{
		return Any(property.unit & Unit::NUMERIC)
#ifdef RMLUI_MATH_EXPRESSIONS
			|| property.unit == Unit::CALCULATION
#endif
			;
	}

} // namespace

bool IsInterpolable(const Property& from, const Property& to, Element& element, const PropertyDefinition* definition)
{
	if (IsNumeric(from) && IsNumeric(to))
	{
		if (from.unit == to.unit)
			return true;
#ifdef RMLUI_MATH_EXPRESSIONS
		if (from.unit == Unit::CALCULATION || to.unit == Unit::CALCULATION)
			return true;
#endif
		if (Any(from.unit & Unit::NUMBER_LENGTH_PERCENT) && Any(to.unit & Unit::NUMBER_LENGTH_PERCENT) && definition)
			return true;
		if (Any(from.unit & Unit::LENGTH) && Any(to.unit & Unit::LENGTH))
			return true;
		return Any(from.unit & Unit::ANGLE) && Any(to.unit & Unit::ANGLE);
	}

	if (from.unit != to.unit)
		return false;

	switch (from.unit)
	{
	case Unit::KEYWORD:
		// 'visibility' interpolates when one of the values is 'visible' (CSS Transitions 1); other keywords are discrete.
		if (definition && definition->GetId() == PropertyId::Visibility)
			return from.Get<int>() == (int)Style::Visibility::Visible || to.Get<int>() == (int)Style::Visibility::Visible;
		return false;
	case Unit::COLOUR:
	case Unit::TRANSFORM:
	case Unit::DECORATOR:
	case Unit::FILTER:
	case Unit::COLORSTOPLIST:
	case Unit::BOXSHADOWLIST: return true;
	case Unit::STYLE_VALUE:
	{
		const StyleValuePtr v0 = from.value.Get<StyleValuePtr>();
		const StyleValuePtr v1 = to.value.Get<StyleValuePtr>();
		return v0 && v1 && v0->Interpolate(*v1, 0.5f, element) != nullptr;
	}
	default: break;
	}
	return false;
}

const Property* ElementStyle::GetDeclaredValue(PropertyId id, Property& storage) const
{
	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!property_definition)
		return nullptr;

	for (const ElementStyle* style = this; style;)
	{
		if (const Property* property = GetLocalProperty(id, style->inline_properties, style->definition.get()))
		{
			SmallUnorderedSet<String> variable_dependencies;
			Property resolved_storage;
			if (const Property* resolved = style->ResolveVariables(id, property, variable_dependencies, resolved_storage))
			{
				storage = *resolved;
				return &storage;
			}
			// An invalid substitution: the property is unset.
		}
		if (!property_definition->IsInherited())
			break;
		Element* parent = style->element->GetParentNode();
		style = (parent ? parent->GetStyle() : nullptr);
	}

	storage = *property_definition->GetDefaultValue();
	return &storage;
}

float ElementStyle::GetDeclaredFontSize(const ComputeContext& context) const
{
	// The font size the declarations give, without any font-size transition or animation of the element or its ancestors:
	// em lengths are compared in it, so that an animated font size does not restart the transitions of em lengths.
	float factor = 1.f;
	for (const ElementStyle* style = this; style;)
	{
		if (const Property* property = GetLocalProperty(PropertyId::FontSize, style->inline_properties, style->definition.get()))
		{
			SmallUnorderedSet<String> variable_dependencies;
			Property storage;
			const Property* resolved = style->ResolveVariables(PropertyId::FontSize, property, variable_dependencies, storage);
			if (resolved && resolved->unit == Unit::EM)
				factor *= resolved->Get<float>();
			else if (resolved && resolved->unit == Unit::PERCENT)
				factor *= resolved->Get<float>() * 0.01f;
			else if (resolved && Any(resolved->unit & Unit::LENGTH) && resolved->unit != Unit::REM)
				return factor * ComputeLength(resolved->GetNumericValue(), 0.f, 0.f, context.dp_ratio, context.vp_dimensions);
			else
				// rem, keywords and expressions: their computed size.
				return factor * style->element->GetComputedValues().font_size();
		}
		Element* parent = style->element->GetParentNode();
		style = (parent ? parent->GetStyle() : nullptr);
	}
	return factor * DefaultComputedValues().font_size();
}

Property ElementStyle::ComputedForm(PropertyId id, const Property& property, const ComputeContext& context) const
{
	Property result = property;
	if (id == PropertyId::FontSize)
	{
		// Relative to the parent's declared font size (not its animated one).
		Element* parent = element->GetParentNode();
		float size = 0.f;
		if (parent && (property.unit == Unit::EM || property.unit == Unit::PERCENT))
			size = property.Get<float>() * (property.unit == Unit::PERCENT ? 0.01f : 1.f) * parent->GetStyle()->GetDeclaredFontSize(context);
		else
			size = ComputeFontsize(&property, context.values, context.parent_values, context.document_values, context.dp_ratio, context.vp_dimensions);
		result.value = size;
		result.unit = Unit::PX;
	}
	else if (Any(property.unit & Unit::LENGTH) && property.unit != Unit::PX)
	{
		const float document_font_size =
			(context.document_values ? context.document_values->font_size() : DefaultComputedValues().font_size());
		const float font_size = (property.unit == Unit::EM ? GetDeclaredFontSize(context) : context.values.font_size());
		result.value = ComputeLength(property.GetNumericValue(), font_size, document_font_size, context.dp_ratio, context.vp_dimensions);
		result.unit = Unit::PX;
	}
	if (!result.definition)
		result.definition = StyleSheetSpecification::GetProperty(id);
	return result;
}

const Property* ElementStyle::GetStartingStyleValue(PropertyId id, SharedPtr<const ElementDefinition>& starting_definition, bool& looked_up,
	Property& storage)
{
	if (!HasStartingStyleRules())
		return nullptr;
	if (!looked_up)
	{
		looked_up = true;
		const StyleSheet* style_sheet = element->GetStyleSheet();
		if (!style_sheet)
			return nullptr;
		// The definition with the starting-style pseudo-class set: the element's rules and its @starting-style rules.
		const String& pseudo_class = GetStartingStylePseudoClass();
		pseudo_classes[pseudo_class] = PseudoClassState::Set;
		starting_definition = style_sheet->GetElementDefinition(element);
		pseudo_classes.erase(pseudo_class);
		if (starting_definition == definition)
			starting_definition.reset();
	}
	if (!starting_definition)
		return nullptr;

	const Property* property = GetLocalProperty(id, inline_properties, starting_definition.get());
	if (!property || property == GetLocalProperty(id, inline_properties, definition.get()))
		return nullptr;

	SmallUnorderedSet<String> variable_dependencies;
	const PropertySources sources{element, starting_definition.get(), inline_properties, nullptr};
	const Property* resolved = ResolveVariables(sources, expanded_shorthands, id, property, variable_dependencies, storage);
	if (resolved && resolved != &storage)
		storage = *resolved;
	return resolved ? &storage : nullptr;
}

void ElementStyle::CaptureBeforeChangeValues(const PropertyIdSet& changed_properties, const ElementDefinition* new_definition)
{
	// Only elements styled before have before-change values; only definitions with transitions need them.
	if (!definition || changed_properties.Empty())
		return;
	const PropertyIdSet& transition_ids = GetTransitionLonghandIds();
	bool transitions = (transition_snapshot != nullptr) || !(definition->GetPropertyIds() & transition_ids).Empty() ||
		(new_definition && !(new_definition->GetPropertyIds() & transition_ids).Empty());
	for (auto it = transition_ids.begin(); !transitions && it != transition_ids.end(); ++it)
		transitions = (inline_properties.GetProperty(*it) != nullptr);
	if (!transitions)
		return;

	if (!transition_snapshot)
	{
		transition_snapshot = MakeUnique<TransitionSnapshot>();
		transition_snapshot->initialized = true;
	}
	TransitionSnapshot& snapshot = *transition_snapshot;
	for (const PropertyId id : changed_properties)
	{
		if (IsAnimationProperty(id) || snapshot.values.GetProperty(id) || snapshot.captured.GetProperty(id))
			continue;
		Property storage;
		if (const Property* value = GetDeclaredValue(id, storage))
			snapshot.captured.SetProperty(id, *value);
	}
}

const Property* ElementStyle::TransitionPrePass(PropertyId id, const ComputeContext& context)
{
	if (!transition_snapshot && !GetTransitionList())
		return nullptr;
	const TransitionList* list = GetTransitionList();
	if (!list || !list->Find(id))
		return nullptr;
	const bool first_style = (!transition_snapshot || !transition_snapshot->initialized || !element->animation_displayed);
	if (!transition_snapshot)
		transition_snapshot = MakeUnique<TransitionSnapshot>();
	const bool displayed = (context.values.display() != Style::Display::None) && (!element->parent || element->parent->animation_displayed);
	Property applied;
	if (!TransitionProperty(id, first_style, displayed, context, applied))
		return nullptr;
	transition_snapshot->applied = std::move(applied);
	return &transition_snapshot->applied;
}

void ElementStyle::TransitionChangedProperties(const ComputeContext& context, const PropertyIdSet& dirty, float font_size,
	float document_font_size, bool& dirty_font_face_handle)
{
	const TransitionList* list = GetTransitionList();
	if (!list)
	{
		// Without transitions there is nothing to keep (running ones are cancelled by Element::HandleTransitionProperty).
		transition_snapshot.reset();
		return;
	}

	const bool first_style = (!transition_snapshot || !transition_snapshot->initialized || !element->animation_displayed);
	if (!transition_snapshot)
		transition_snapshot = MakeUnique<TransitionSnapshot>();
	TransitionSnapshot& snapshot = *transition_snapshot;

	// The properties to look at: the changed ones the list covers; at the first style, every one the list covers.
	PropertyIdSet candidates;
	const bool make_complete = (list->all && !snapshot.complete);
	if (!snapshot.initialized || make_complete)
	{
		if (list->all)
		{
			for (auto it = Iterate(); !it.AtEnd(); ++it)
				candidates.Insert((*it).first);
			candidates |= StyleSheetSpecification::GetRegisteredInheritedProperties();
		}
		else
		{
			for (const Transition& transition : list->transitions)
				candidates.Insert(transition.id);
		}
		if (snapshot.initialized)
			candidates |= dirty;
	}
	else if (list->all)
		candidates = dirty;
	else
	{
		for (const Transition& transition : list->transitions)
			if (dirty.Contains(transition.id))
				candidates.Insert(transition.id);
	}
	// 'font-size' and 'line-height' transition in their pre-passes; here they only get their first values.
	for (PropertyId id : {PropertyId::FontSize, PropertyId::LineHeight})
	{
		if (!candidates.Contains(id))
			continue;
		candidates.Erase(id);
		Property storage;
		if (!snapshot.values.GetProperty(id))
			if (const Property* declared = GetDeclaredValue(id, storage))
				snapshot.values.SetProperty(id, ComputedForm(id, *declared, context));
	}

	auto Apply = [&](PropertyId id, const Property& applied) {
		ComputeValue(context.values, context.dp_ratio, context.vp_dimensions, font_size, document_font_size, dirty_font_face_handle, id, &applied);
	};

	// 'display' first: whether the element is displayed now depends on its transition (allow-discrete keeps it shown).
	if (candidates.Contains(PropertyId::Display))
	{
		candidates.Erase(PropertyId::Display);
		Property applied;
		if (TransitionProperty(PropertyId::Display, first_style, true, context, applied))
			Apply(PropertyId::Display, applied);
	}
	const bool displayed = (context.values.display() != Style::Display::None) && (!element->parent || element->parent->animation_displayed);

	for (const PropertyId id : candidates)
	{
		if (IsAnimationProperty(id) || !StyleSheetSpecification::GetProperty(id))
			continue;
		Property applied;
		if (TransitionProperty(id, first_style, displayed, context, applied))
			Apply(id, applied);
	}

	snapshot.captured.Clear();
	snapshot.initialized = true;
	if (make_complete)
		snapshot.complete = true;
}

bool ElementStyle::TransitionProperty(PropertyId id, bool first_style, bool displayed, const ComputeContext& context, Property& applied)
{
	TransitionSnapshot& snapshot = *transition_snapshot;

	Property storage;
	const Property* declared = GetDeclaredValue(id, storage);
	if (!declared)
		return false;
	const Property after = ComputedForm(id, *declared, context);

	// The before-change value: a captured one, the last after-change value, or (for an element styled for the first time
	// or shown again) its starting style.
	Property before;
	bool has_before = false;
	if (first_style)
	{
		SharedPtr<const ElementDefinition> starting_definition;
		bool looked_up = false;
		Property starting_storage;
		if (const Property* starting = GetStartingStyleValue(id, starting_definition, looked_up, starting_storage))
		{
			before = ComputedForm(id, *starting, context);
			has_before = true;
		}
	}
	else if (const Property* captured = snapshot.captured.GetProperty(id))
	{
		before = ComputedForm(id, *captured, context);
		has_before = true;
	}
	else if (const Property* previous = snapshot.values.GetProperty(id))
	{
		before = *previous;
		has_before = true;
	}
	else if (snapshot.complete)
	{
		// Not declared nor inherited before: it had its initial value.
		const PropertyDefinition* definition_of_id = StyleSheetSpecification::GetProperty(id);
		if (definition_of_id && !definition_of_id->IsInherited())
		{
			before = ComputedForm(id, *definition_of_id->GetDefaultValue(), context);
			has_before = true;
		}
	}
	snapshot.values.SetProperty(id, after);

	const ElementAnimation* running = element->GetTransition(id);
	const bool changed = has_before && !(before == after);
	if (!changed && !running)
		return false;
	if (!changed && running)
	{
		// The value did not change; a running transition keeps running.
		return false;
	}

	// A CSS animation of the property overrides the transition (both before- and after-change values are animated).
	if (const PropertyDictionary* animated = GetAnimatedProperties())
		if (animated->GetProperty(id))
			return false;

	const TransitionList* list = GetTransitionList();
	const Transition* found = (list ? list->Find(id) : nullptr);
	const PropertyDefinition* property_definition = StyleSheetSpecification::GetProperty(id);
	if (!found || !displayed)
	{
		if (running)
		{
			element->CancelTransition(id);
			applied = after;
			return true;
		}
		return false;
	}

	Transition transition = *found;
	transition.id = id;
	const bool combined_duration = (Math::Max(transition.duration, 0.f) + transition.delay > 0.f);
	auto Transitionable = [&](const Property& from, const Property& to) {
		return transition.allow_discrete || IsInterpolable(from, to, *element, property_definition);
	};

	if (!running)
	{
		if (!combined_duration || !Transitionable(before, after))
			return false;
		if (!element->StartTransition(transition, before, after, before, 1.f))
			return false;
		applied = before;
		return true;
	}

	// A running transition (CSS Transitions 1 §3, steps 3 to 6).
	const PropertyDictionary* layer = GetTransitionProperties();
	const Property* current_in_layer = (layer ? layer->GetProperty(id) : nullptr);
	Property current = (current_in_layer ? *current_in_layer : (running->GetStartValue() ? *running->GetStartValue() : before));
	if (!current.definition)
		current.definition = property_definition;
	const Property* end = running->GetEndValue();

	if (!combined_duration)
	{
		element->CancelTransition(id);
		applied = after;
		return true;
	}
	if (end && *end == after)
		return false;
	if (current == after || !Transitionable(current, after))
	{
		element->CancelTransition(id);
		applied = after;
		return true;
	}

	if (running->GetReversingAdjustedStart() == after && end)
	{
		// Reversing: shortened by how far the old transition got.
		const float old_factor = running->GetReversingShorteningFactor();
		const float factor = Math::Clamp(Math::Absolute(running->GetInterpolationFactor() * old_factor + 1.f - old_factor), 0.f, 1.f);
		Transition reversed = transition;
		reversed.duration *= factor;
		if (reversed.delay < 0.f)
			reversed.delay *= factor;
		const Property reversing_adjusted_start = *end;
		if (!element->StartTransition(reversed, current, after, reversing_adjusted_start, factor))
			return false;
	}
	else if (!element->StartTransition(transition, current, after, current, 1.f))
		return false;

	applied = current;
	return true;
}

} // namespace Rml
