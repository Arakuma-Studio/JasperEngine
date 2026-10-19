#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "Cascade.h"
#include "ContentValue.h"
#include "GridStyleValues.h"
#include "IdNameMap.h"
#include "BackgroundLayerValues.h"
#include "PaintProperties.h"
#include "LayoutProperties.h"
#include "PropertyParserAlignment.h"
#include "PropertyParserAspectRatio.h"
#include "PropertyParserAnimation.h"
#include "PropertyParserBoxShadow.h"
#include "PropertyParserColorStopList.h"
#include "PropertyParserColour.h"
#include "PropertyParserCursor.h"
#include "PropertyParserDecorator.h"
#include "PropertyParserFilter.h"
#include "PropertyParserFontFamily.h"
#include "PropertyParserFontEffect.h"
#include "PropertyParserKeyword.h"
#include "PropertyParserNumber.h"
#include "PropertyParserRatio.h"
#include "PropertyParserString.h"
#include "PropertyParserTransform.h"
#include "PropertyShorthandDefinition.h"
#include "TextProperties.h"
#include "TextShadow.h"
#include "ValuesShorthands.h"

#include <algorithm>

namespace Rml {

static StyleSheetSpecification* instance = nullptr;
bool StyleSheetSpecification::any_registered_custom_properties = false;

struct DefaultStyleSheetParsers : NonCopyMoveable {
#ifdef RMLUI_MATH_EXPRESSIONS
	PropertyParserNumber number = PropertyParserNumber(Unit::NUMBER, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Number));
	PropertyParserNumber length = PropertyParserNumber(Unit::LENGTH, Unit::PX, MakeCalculationParseTarget(CalculationFinalType::Length));
	PropertyParserNumber length_percent = PropertyParserNumber(Unit::LENGTH_PERCENT, Unit::PX,
		MakeCalculationParseTarget(CalculationFinalType::Length, CalculationPercentageHint::Length));
	PropertyParserNumber number_percent = PropertyParserNumber(Unit::NUMBER_PERCENT, Unit::UNKNOWN,
		{CalculationFinalType::Number | CalculationFinalType::Percent, CalculationPercentageHint::None});
	PropertyParserNumber number_length_percent = PropertyParserNumber(Unit::NUMBER_LENGTH_PERCENT, Unit::PX);
	PropertyParserNumber line_height = PropertyParserNumber(Unit::NUMBER_LENGTH_PERCENT, Unit::PX,
		{CalculationFinalType::Number | CalculationFinalType::Length, CalculationPercentageHint::Length});
	PropertyParserNumber angle = PropertyParserNumber(Unit::ANGLE, Unit::RAD, MakeCalculationParseTarget(CalculationFinalType::Angle));
#else
	PropertyParserNumber number = PropertyParserNumber(Unit::NUMBER);
	PropertyParserNumber length = PropertyParserNumber(Unit::LENGTH, Unit::PX);
	PropertyParserNumber length_percent = PropertyParserNumber(Unit::LENGTH_PERCENT, Unit::PX);
	PropertyParserNumber number_percent = PropertyParserNumber(Unit::NUMBER_PERCENT);
	PropertyParserNumber number_length_percent = PropertyParserNumber(Unit::NUMBER_LENGTH_PERCENT, Unit::PX);
	PropertyParserNumber angle = PropertyParserNumber(Unit::ANGLE, Unit::RAD);
#endif
	PropertyParserKeyword keyword = PropertyParserKeyword();
	PropertyParserAlignment alignment = PropertyParserAlignment();
	PropertyParserString string = PropertyParserString();
	PropertyParserColour color = PropertyParserColour();
	PropertyParserColorStopList color_stop_list = PropertyParserColorStopList(&color);
	PropertyParserDecorator decorator = PropertyParserDecorator();
	PropertyParserFilter filter = PropertyParserFilter();
	PropertyParserFontEffect font_effect = PropertyParserFontEffect();
	PropertyParserTransform transform = PropertyParserTransform();
	PropertyParserRatio ratio = PropertyParserRatio();
#ifdef RMLUI_MATH_EXPRESSIONS
	PropertyParserNumber resolution = PropertyParserNumber(Unit::X, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Resolution));
#else
	PropertyParserNumber resolution = PropertyParserNumber(Unit::X);
#endif
	PropertyParserBoxShadow box_shadow = PropertyParserBoxShadow(&color, &length);
};

namespace {
	// The parser of a registered custom property's syntax. Each syntax reads what CSS's type does: no unitless zero for an
	// angle or a time (only for a length), percentages only for '<percentage>' and '<length-percentage>', integers only for
	// '<integer>' (an expression is rounded when it is computed), a time in seconds.
	class PropertyParserRegistered final : public PropertyParser {
	public:
		explicit PropertyParserRegistered(RegisteredPropertySyntax syntax) :
			syntax(syntax),
#ifdef RMLUI_MATH_EXPRESSIONS
			number(Unit::NUMBER, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Number)),
			length(Unit::LENGTH, Unit::PX, MakeCalculationParseTarget(CalculationFinalType::Length)),
			percentage(Unit::PERCENT, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Percent)),
			length_percentage(Unit::LENGTH_PERCENT, Unit::PX, MakeCalculationParseTarget(CalculationFinalType::Length, CalculationPercentageHint::Length)),
			angle(Unit::ANGLE, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Angle))
#else
			number(Unit::NUMBER), length(Unit::LENGTH, Unit::PX), percentage(Unit::PERCENT), length_percentage(Unit::LENGTH_PERCENT, Unit::PX),
			angle(Unit::ANGLE)
#endif
		{}

		bool ParseValue(Property& property, const String& value, const ParameterMap& parameters) const override
		{
			// One numeric token or one math function: the number parser reads a number prefix and stops ('1px 2px', '1p2px').
			if (syntax != RegisteredPropertySyntax::Universal && syntax != RegisteredPropertySyntax::Color && syntax != RegisteredPropertySyntax::Time &&
				!IsNumericToken(value) && !IsMathFunction(value))
				return false;
			switch (syntax)
			{
			case RegisteredPropertySyntax::Universal:
				property.value = Variant(value);
				property.unit = Unit::STRING;
				return true;
			case RegisteredPropertySyntax::Number: return number.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::Integer:
				if (!number.ParseValue(property, value, parameters))
					return false;
				// An integer literal; an expression is rounded to the nearest integer when computed.
				return property.unit != Unit::NUMBER || property.Get<float>() == float(int(property.Get<float>()));
			case RegisteredPropertySyntax::Length: return length.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::Percentage: return percentage.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::LengthPercentage: return length_percentage.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::Color: return colour.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::Angle: return angle.ParseValue(property, value, parameters);
			case RegisteredPropertySyntax::Time:
			{
				float seconds = 0.f;
				if (!ParseAnimationTime(StringUtilities::StripWhitespace(value), seconds))
					return false;
				property.value = seconds;
				property.unit = Unit::NUMBER;
				return true;
			}
			}
			return false;
		}

	private:
		// [+-]?(digits[.digits]|.digits)([eE][+-]?digits)? followed by a unit or '%' (letters), nothing else.
		static bool IsNumericToken(const String& value)
		{
			size_t i = 0;
			const size_t n = value.size();
			auto digits = [&]() {
				const size_t start = i;
				while (i < n && value[i] >= '0' && value[i] <= '9')
					i++;
				return i > start;
			};
			if (i < n && (value[i] == '+' || value[i] == '-'))
				i++;
			const bool integer_part = digits();
			bool fraction_part = false;
			if (i < n && value[i] == '.')
			{
				i++;
				fraction_part = digits();
			}
			if (!integer_part && !fraction_part)
				return false;
			if (i + 1 < n && (value[i] == 'e' || value[i] == 'E'))
			{
				size_t exponent = i + 1;
				if (exponent < n && (value[exponent] == '+' || value[exponent] == '-'))
					exponent++;
				if (exponent < n && value[exponent] >= '0' && value[exponent] <= '9')
				{
					i = exponent;
					digits();
				}
			}
			if (i < n && value[i] == '%')
				return i + 1 == n;
			for (; i < n; i++)
				if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z')))
					return false;
			return true;
		}
		static bool IsMathFunction(const String& value)
		{
			const String lower = StringUtilities::ToLower(value.substr(0, 6));
			return (StringUtilities::StartsWith(lower, "calc(") || StringUtilities::StartsWith(lower, "min(") ||
					   StringUtilities::StartsWith(lower, "max(") || StringUtilities::StartsWith(lower, "clamp(")) &&
				!value.empty() && value.back() == ')';
		}

		RegisteredPropertySyntax syntax;
		PropertyParserNumber number, length, percentage, length_percentage, angle;
		PropertyParserColour colour;
	};

	const char* RegisteredParserName(RegisteredPropertySyntax syntax)
	{
		switch (syntax)
		{
		case RegisteredPropertySyntax::Universal: return "registered_universal";
		case RegisteredPropertySyntax::Number: return "registered_number";
		case RegisteredPropertySyntax::Integer: return "registered_integer";
		case RegisteredPropertySyntax::Length: return "registered_length";
		case RegisteredPropertySyntax::Percentage: return "registered_percentage";
		case RegisteredPropertySyntax::LengthPercentage: return "registered_length_percentage";
		case RegisteredPropertySyntax::Color: return "registered_color";
		case RegisteredPropertySyntax::Angle: return "registered_angle";
		case RegisteredPropertySyntax::Time: return "registered_time";
		}
		return "";
	}
} // namespace

// Registered custom properties (fork, phase 5; CSS Properties and Values API 1).
struct RegisteredCustomProperties : NonCopyMoveable {
	static constexpr size_t NumSyntaxes = size_t(RegisteredPropertySyntax::Time) + 1;
	PropertyIdSet ids;
	UnorderedMap<PropertyId, RegisteredCustomProperty> registrations;
	UniquePtr<PropertyParserRegistered> parsers[NumSyntaxes];
	// Unregistered: their ids, oldest first (taken again by a name only when no fresh id is left, or by the name they
	// had), the id each name had last, and their definitions (elements may still hold values that point to them).
	Vector<PropertyId> free_ids;
	UnorderedMap<String, PropertyId> last_ids;
	Vector<UniquePtr<PropertyDefinition>> retired;
};

StyleSheetSpecification::StyleSheetSpecification() :
	// Reserve space for all defined ids and some more for custom properties
	properties((size_t)PropertyId::NumDefinedIds + 128, 2 * (size_t)ShorthandId::NumDefinedIds)
{
	RMLUI_ASSERT(instance == nullptr);
	instance = this;

	default_parsers.reset(new DefaultStyleSheetParsers);
	registered_custom_properties.reset(new RegisteredCustomProperties);
}

StyleSheetSpecification::~StyleSheetSpecification()
{
	RMLUI_ASSERT(instance == this);
	instance = nullptr;
	any_registered_custom_properties = false;
}

PropertyDefinition& StyleSheetSpecification::RegisterProperty(PropertyId id, const String& property_name, const String& default_value, bool inherited,
	bool forces_layout)
{
	return properties.RegisterProperty(property_name, default_value, inherited, forces_layout, id);
}

ShorthandId StyleSheetSpecification::RegisterShorthand(ShorthandId id, const String& shorthand_name, const String& property_names, ShorthandType type)
{
	return properties.RegisterShorthand(shorthand_name, property_names, type, id);
}

void StyleSheetSpecification::Initialise()
{
	RMLUI_ASSERT(!instance);

	PropertyParserAnimation::Initialize();
	PropertyParserColour::Initialize();
	PropertyParserDecorator::Initialize();
	PropertyParserNumber::Initialize();

	new StyleSheetSpecification();

	instance->RegisterDefaultParsers();
	instance->RegisterDefaultProperties();
}

void StyleSheetSpecification::Shutdown()
{
	RMLUI_ASSERT(instance);

	delete instance;

	PropertyParserAnimation::Shutdown();
	PropertyParserColour::Shutdown();
	PropertyParserDecorator::Shutdown();
	PropertyParserNumber::Shutdown();
}

bool StyleSheetSpecification::RegisterParser(const String& parser_name, PropertyParser* parser)
{
	ParserMap::iterator iterator = instance->parsers.find(parser_name);
	if (iterator != instance->parsers.end())
	{
		Log::Message(Log::LT_WARNING, "Parser with name %s already exists!", parser_name.c_str());
		return false;
	}

	instance->parsers[parser_name] = parser;
	return true;
}

PropertyParser* StyleSheetSpecification::GetParser(const String& parser_name)
{
	ParserMap::iterator iterator = instance->parsers.find(parser_name);
	if (iterator == instance->parsers.end())
		return nullptr;

	return (*iterator).second;
}

PropertyDefinition& StyleSheetSpecification::RegisterProperty(const String& property_name, const String& default_value, bool inherited,
	bool forces_layout)
{
	RMLUI_ASSERTMSG((size_t)instance->properties.property_map->GetId(property_name) < (size_t)PropertyId::FirstCustomId,
		"Custom property name matches an internal property, please make a unique name for the given property.");
	return instance->RegisterProperty(PropertyId::Invalid, property_name, default_value, inherited, forces_layout);
}

PropertyId StyleSheetSpecification::GetTextFillPropertyId()
{
	return instance ? instance->text_fill_id : PropertyId::Invalid;
}

const PropertyDefinition* StyleSheetSpecification::GetProperty(const String& property_name)
{
	return instance->properties.GetProperty(property_name);
}

const PropertyDefinition* StyleSheetSpecification::GetProperty(PropertyId id)
{
	return instance->properties.GetProperty(id);
}

const PropertyIdSet& StyleSheetSpecification::GetRegisteredProperties()
{
	return instance->properties.GetRegisteredProperties();
}

const PropertyIdSet& StyleSheetSpecification::GetRegisteredInheritedProperties()
{
	return instance->properties.GetRegisteredInheritedProperties();
}

const PropertyIdSet& StyleSheetSpecification::GetRegisteredPropertiesForcingLayout()
{
	return instance->properties.GetRegisteredPropertiesForcingLayout();
}

ShorthandId StyleSheetSpecification::RegisterShorthand(const String& shorthand_name, const String& property_names, ShorthandType type)
{
	RMLUI_ASSERTMSG(instance->properties.property_map->GetId(shorthand_name) == PropertyId::Invalid,
		"Custom shorthand name matches a property name, please make a unique name.");
	RMLUI_ASSERTMSG((size_t)instance->properties.shorthand_map->GetId(shorthand_name) < (size_t)ShorthandId::FirstCustomId,
		"Custom shorthand name matches an internal shorthand, please make a unique name for the given shorthand property.");
	return instance->properties.RegisterShorthand(shorthand_name, property_names, type);
}

bool StyleSheetSpecification::RegisterShorthandParser(ShorthandId id, ShorthandParserFunction parser)
{
	return instance->properties.SetShorthandParser(id, parser);
}

const ShorthandDefinition* StyleSheetSpecification::GetShorthand(const String& shorthand_name)
{
	return instance->properties.GetShorthand(shorthand_name);
}

const ShorthandDefinition* StyleSheetSpecification::GetShorthand(ShorthandId id)
{
	return instance->properties.GetShorthand(id);
}

bool StyleSheetSpecification::ParsePropertyDeclaration(PropertyDictionary& dictionary, const String& property_name, const String& property_value)
{
	return instance->properties.ParsePropertyDeclaration(dictionary, property_name, property_value);
}

PropertyId StyleSheetSpecification::GetPropertyId(const String& property_name)
{
	return instance->properties.property_map->GetId(property_name);
}

ShorthandId StyleSheetSpecification::GetShorthandId(const String& shorthand_name)
{
	return instance->properties.shorthand_map->GetId(shorthand_name);
}

const String& StyleSheetSpecification::GetPropertyName(PropertyId id)
{
	return instance->properties.property_map->GetName(id);
}

const String& StyleSheetSpecification::GetShorthandName(ShorthandId id)
{
	return instance->properties.shorthand_map->GetName(id);
}

PropertyIdSet StyleSheetSpecification::GetShorthandUnderlyingProperties(ShorthandId id)
{
	PropertyIdSet result;
	const ShorthandDefinition* shorthand = instance->properties.GetShorthand(id);
	if (!shorthand)
		return result;

	for (auto& item : shorthand->items)
	{
		if (item.type == ShorthandItemType::Property)
		{
			result.Insert(item.property_id);
		}
		else if (item.type == ShorthandItemType::Shorthand)
		{
			// When we have a shorthand pointing to another shorthands, call us recursively. Add the union of the previous result and new properties.
			result |= GetShorthandUnderlyingProperties(item.shorthand_id);
		}
	}
	return result;
}

const PropertySpecification& StyleSheetSpecification::GetPropertySpecification()
{
	return instance->properties;
}

PropertyId StyleSheetSpecification::RegisterCustomProperty(const String& name, RegisteredPropertySyntax syntax, bool inherits,
	const String& initial_value)
{
	if (name.size() < 3 || !StringUtilities::StartsWith(name, "--"))
		return PropertyId::Invalid;
	const PropertyId existing = GetPropertyId(name);
	if (existing != PropertyId::Invalid)
		return IsRegisteredCustomProperty(existing) ? existing : PropertyId::Invalid;
	if (GetNumFreePropertyIds() <= 0)
		return PropertyId::Invalid;

	// The initial value must match the syntax (CSS: else the rule is invalid); none is allowed for the universal syntax only.
	const bool has_initial_value = !StringUtilities::StripWhitespace(initial_value).empty();
	PropertyParser* parser = GetParser(RegisteredParserName(syntax));
	if (!parser)
		return PropertyId::Invalid;
	if (has_initial_value)
	{
		Property parsed;
		if (!parser->ParseValue(parsed, StringUtilities::StripWhitespace(initial_value), ParameterMap()))
			return PropertyId::Invalid;
	}
	else if (syntax != RegisteredPropertySyntax::Universal)
		return PropertyId::Invalid;

	// The id: the one the name had last if it is free (a document registering again after a reload keeps its ids), else
	// a fresh one, else the oldest free one.
	RegisteredCustomProperties& registered = *instance->registered_custom_properties;
	PropertySpecification& specification = instance->properties;
	size_t free_index = registered.free_ids.size();
	const auto last = registered.last_ids.find(name);
	if (last != registered.last_ids.end())
		free_index = size_t(std::find(registered.free_ids.begin(), registered.free_ids.end(), last->second) - registered.free_ids.begin());
	if (free_index == registered.free_ids.size() && specification.property_map->GetNumIds() >= (size_t)PropertyId::MaxNumIds)
		free_index = 0;
	PropertyDefinition* new_definition = nullptr;
	if (free_index < registered.free_ids.size())
	{
		const PropertyId id = registered.free_ids[free_index];
		if (!specification.property_map->AssignName(id, name))
			return PropertyId::Invalid;
		registered.free_ids.erase(registered.free_ids.begin() + free_index);
		// (the old definition stays alive: values parsed with it may remain on elements)
		registered.retired.push_back(std::move(specification.properties[(size_t)id]));
		specification.properties[(size_t)id] = MakeUnique<PropertyDefinition>(id, StringUtilities::StripWhitespace(initial_value), inherits, false);
		specification.property_ids.Insert(id);
		if (inherits)
			specification.property_ids_inherited.Insert(id);
		new_definition = specification.properties[(size_t)id].get();
	}
	else
		new_definition = &instance->RegisterProperty(PropertyId::Invalid, name, StringUtilities::StripWhitespace(initial_value), inherits, false);
	PropertyDefinition& definition = *new_definition;
	if (definition.GetId() == PropertyId::Invalid)
		return PropertyId::Invalid;
	if (syntax == RegisteredPropertySyntax::Universal)
		definition.SetValueAsWritten();
	definition.AddParser(RegisteredParserName(syntax));

	RegisteredCustomProperty registration;
	registration.syntax = syntax;
	registration.inherits = inherits;
	registration.has_initial_value = has_initial_value;
	registration.initial_value = StringUtilities::StripWhitespace(initial_value);
	MarkRegisteredCustomProperty(definition.GetId(), registration);
	return definition.GetId();
}

void StyleSheetSpecification::MarkRegisteredCustomProperty(PropertyId id, const RegisteredCustomProperty& registration)
{
	if (id == PropertyId::Invalid || !GetProperty(id))
		return;
	RegisteredCustomProperties& registered = *instance->registered_custom_properties;
	registered.ids.Insert(id);
	registered.registrations[id] = registration;
	any_registered_custom_properties = true;
}

bool StyleSheetSpecification::UnregisterCustomProperty(PropertyId id)
{
	if (!IsRegisteredCustomProperty(id))
		return false;
	RegisteredCustomProperties& registered = *instance->registered_custom_properties;
	PropertySpecification& specification = instance->properties;
	registered.ids.Erase(id);
	registered.registrations.erase(id);
	specification.property_ids.Erase(id);
	specification.property_ids_inherited.Erase(id);
	specification.property_ids_forcing_layout.Erase(id);
	const String name = specification.property_map->GetName(id);
	specification.property_map->ReleaseName(id);
	registered.free_ids.push_back(id);
	registered.last_ids[name] = id;
	any_registered_custom_properties = !registered.registrations.empty();
	return true;
}

bool StyleSheetSpecification::IsRegisteredCustomProperty(PropertyId id)
{
	return instance && instance->registered_custom_properties->ids.Contains(id);
}

const RegisteredCustomProperty* StyleSheetSpecification::GetRegisteredCustomProperty(PropertyId id)
{
	const RegisteredCustomProperties& registered = *instance->registered_custom_properties;
	if (!registered.ids.Contains(id))
		return nullptr;
	const auto it = registered.registrations.find(id);
	return it != registered.registrations.end() ? &it->second : nullptr;
}

const PropertyIdSet& StyleSheetSpecification::GetRegisteredCustomProperties()
{
	return instance->registered_custom_properties->ids;
}


int StyleSheetSpecification::GetNumFreePropertyIds()
{
	// The next id GetOrCreateId gives is one past the last name (reserved ids included).
	const size_t next = (size_t)instance->properties.property_map->GetNumIds();
	const size_t fresh = (next >= (size_t)PropertyId::MaxNumIds ? 0 : (size_t)PropertyId::MaxNumIds - next);
	return int(fresh + instance->registered_custom_properties->free_ids.size());
}

void StyleSheetSpecification::RegisterDefaultParsers()
{
	RegisterParser("number", &default_parsers->number);
	RegisterParser("length", &default_parsers->length);
	RegisterParser("length_percent", &default_parsers->length_percent);
	RegisterParser("number_percent", &default_parsers->number_percent);
	RegisterParser("number_length_percent", &default_parsers->number_length_percent);
#ifdef RMLUI_MATH_EXPRESSIONS
	RegisterParser("line_height", &default_parsers->line_height);
#endif
	RegisterParser("angle", &default_parsers->angle);
	RegisterParser("keyword", &default_parsers->keyword);
	RegisterParser("string", &default_parsers->string);
	RegisterParser("color", &default_parsers->color);
	RegisterParser("color_stop_list", &default_parsers->color_stop_list);
	RegisterParser("decorator", &default_parsers->decorator);
	RegisterParser("filter", &default_parsers->filter);
	RegisterParser("font_effect", &default_parsers->font_effect);
	RegisterParser("transform", &default_parsers->transform);
	RegisterParser("ratio", &default_parsers->ratio);
	RegisterParser("resolution", &default_parsers->resolution);
	RegisterParser("box_shadow", &default_parsers->box_shadow);

	/*
	  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3): each package registers its parsers in its own
	  section below, and only there.
	*/

	// -- Groundwork (p3-base) --
	RegisterParser("alignment", &default_parsers->alignment);

	// -- Values (p3-values) --
	static PropertyParserFontFamily font_family_parser;
	RegisterParser("font_family", &font_family_parser);
	static PropertyParserCursor cursor_parser;
	RegisterParser("cursor", &cursor_parser);
#ifdef RMLUI_MATH_EXPRESSIONS
	static PropertyParserNumber alpha_value_parser(Unit::NUMBER_PERCENT, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Number));
	static PropertyParserNumber non_negative_number_parser(Unit::NUMBER, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Number));
	static PropertyParserNumber integer_parser(Unit::NUMBER, Unit::UNKNOWN, MakeCalculationParseTarget(CalculationFinalType::Number));
#else
	static PropertyParserNumber alpha_value_parser(Unit::NUMBER_PERCENT);
	static PropertyParserNumber non_negative_number_parser(Unit::NUMBER);
	static PropertyParserNumber integer_parser(Unit::NUMBER);
#endif
	RegisterParser("alpha_value", &alpha_value_parser.SetRestriction(PropertyParserNumber::Restriction::AlphaValue));
	RegisterParser("non_negative_number", &non_negative_number_parser.SetRestriction(PropertyParserNumber::Restriction::NonNegative));
	RegisterParser("integer", &integer_parser.SetRestriction(PropertyParserNumber::Restriction::Integer));
	static PropertyParserAspectRatio aspect_ratio_parser;
	RegisterParser("aspect_ratio", &aspect_ratio_parser);

	// -- Cascade (p3-cascade) --
	{
		// Generated content (ContentValue.h); the instances live as long as the program, as the default parsers do.
		static PropertyParserContent content_parser;
		static PropertyParserQuotes quotes_parser;
		static PropertyParserCounterList counter_list_parser;
		static PropertyParserListStyleType list_style_type_parser;
		static PropertyParserUrl url_parser;
		RegisterParser("content", &content_parser);
		RegisterParser("quotes", &quotes_parser);
		RegisterParser("counter_list", &counter_list_parser);
		RegisterParser("list_style_type", &list_style_type_parser);
		RegisterParser("url", &url_parser);
	}

	// -- Paint (p3-paint) --
	RegisterParser("line_width", GetLineWidthParser());
	RegisterBackgroundLayerParsers();

	// -- Text (p3-text) --
	static PropertyParserTextDecorationLine text_decoration_line;
	RegisterParser("text_decoration_line", &text_decoration_line);
	RegisterParser("text_shadow", &GetTextShadowParser());

	// -- Layout (p3-layout) --
	RegisterLayoutParsers();

	// -- Animation (p3-animation) --
	PropertyParserAnimation::RegisterParsers();

	// -- Registered custom properties (fork, phase 5) --
	for (size_t i = 0; i < RegisteredCustomProperties::NumSyntaxes; i++)
	{
		const RegisteredPropertySyntax syntax = RegisteredPropertySyntax(i);
		registered_custom_properties->parsers[i] = MakeUnique<PropertyParserRegistered>(syntax);
		RegisterParser(RegisteredParserName(syntax), registered_custom_properties->parsers[i].get());
	}

	// -- Grid (p3-grid) --
	RegisterGridParsers();
}

void StyleSheetSpecification::RegisterDefaultProperties()
{
	/*
	    Style property specifications (ala RCSS).

	    Note: Whenever keywords or default values are changed, make sure its computed value is
	    changed correspondingly, see `ComputedValues.h`.

	    The defaults are the CSS initial values (phase 3, docs/html-ui/FORK.md P3-4): font-size 16px,
	    color black, border colors 'currentcolor' (computed as the element's color), min-width/min-height
	    auto, opacity not inherited, visibility inherited.

	    When adding new properties, it may be desirable to add it to the computed values as well.
	    Then, make sure to resolve it as appropriate in `ElementStyle.cpp`.

	*/

	// clang-format off
	RegisterProperty(PropertyId::MarginTop, "margin-top", "0px", false, true)
		.AddParser("keyword", "auto")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MarginRight, "margin-right", "0px", false, true)
		.AddParser("keyword", "auto")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MarginBottom, "margin-bottom", "0px", false, true)
		.AddParser("keyword", "auto")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MarginLeft, "margin-left", "0px", false, true)
		.AddParser("keyword", "auto")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterShorthand(ShorthandId::Margin, "margin", "margin-top, margin-right, margin-bottom, margin-left", ShorthandType::Box);

	RegisterProperty(PropertyId::PaddingTop, "padding-top", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::PaddingRight, "padding-right", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::PaddingBottom, "padding-bottom", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::PaddingLeft, "padding-left", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterShorthand(ShorthandId::Padding, "padding", "padding-top, padding-right, padding-bottom, padding-left", ShorthandType::Box);


	RegisterProperty(PropertyId::BorderTopColor, "border-top-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderRightColor, "border-right-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderBottomColor, "border-bottom-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderLeftColor, "border-left-color", "currentcolor", false, false).AddParser("color");
	RegisterShorthand(ShorthandId::BorderColor, "border-color", "border-top-color, border-right-color, border-bottom-color, border-left-color", ShorthandType::Box);

	RegisterProperty(PropertyId::Display, "display", "inline", false, true)
		.AddParser("keyword", "none, block, inline, inline-block, flow-root, flex, inline-flex, table, inline-table, table-row, table-row-group, table-column, table-column-group, table-cell, grid, inline-grid, list-item, contents");
	RegisterProperty(PropertyId::Position, "position", "static", false, true).AddParser("keyword", "static, relative, absolute, fixed, sticky");
	RegisterProperty(PropertyId::Top, "top", "auto", false, false).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::Right, "right", "auto", false, false).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::Bottom, "bottom", "auto", false, false).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::Left, "left", "auto", false, false).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterShorthand(ShorthandId::Inset, "inset", "top, right, bottom, left", ShorthandType::Box);

	RegisterProperty(PropertyId::Float, "float", "none", false, true).AddParser("keyword", "none, left, right");
	RegisterProperty(PropertyId::Clear, "clear", "none", false, true).AddParser("keyword", "none, left, right, both");

	RegisterProperty(PropertyId::BoxSizing, "box-sizing", "content-box", false, true).AddParser("keyword", "content-box, border-box");

	RegisterProperty(PropertyId::ZIndex, "z-index", "auto", false, false).AddParser("keyword", "auto").AddParser("integer");

	RegisterProperty(PropertyId::Width, "width", "auto", false, true).AddParser("keyword", "auto, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").AddParser("fit_content").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MinWidth, "min-width", "auto", false, true).AddParser("keyword", "auto, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MaxWidth, "max-width", "none", false, true).AddParser("keyword", "none, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);

	RegisterProperty(PropertyId::Height, "height", "auto", false, true).AddParser("keyword", "auto, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").AddParser("fit_content").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::MinHeight, "min-height", "auto", false, true).AddParser("keyword", "auto, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::MaxHeight, "max-height", "none", false, true).AddParser("keyword", "none, min-content, max-content, fit-content, stretch, -webkit-fill-available=4").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);

#ifdef RMLUI_MATH_EXPRESSIONS
	RegisterProperty(PropertyId::LineHeight, "line-height", "normal", true, true).AddParser("keyword", "normal").AddParser("line_height").SetRelativeTarget(RelativeTarget::FontSize);
#else
	RegisterProperty(PropertyId::LineHeight, "line-height", "normal", true, true).AddParser("keyword", "normal").AddParser("number_length_percent").SetRelativeTarget(RelativeTarget::FontSize);
#endif
	RegisterProperty(PropertyId::VerticalAlign, "vertical-align", "baseline", false, true)
		.AddParser("keyword", "baseline, middle, sub, super, text-top, text-bottom, top, center, bottom")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::LineHeight);

	RegisterProperty(PropertyId::OverflowX, "overflow-x", "visible", false, true).AddParser("keyword", "visible, hidden, auto, scroll, clip");
	RegisterProperty(PropertyId::OverflowY, "overflow-y", "visible", false, true).AddParser("keyword", "visible, hidden, auto, scroll, clip");
	RegisterShorthand(ShorthandId::Overflow, "overflow", "overflow-x, overflow-y", ShorthandType::Replicate);
	RegisterProperty(PropertyId::Clip, "clip", "auto", false, false).AddParser("keyword", "auto, none, always").AddParser("number");
	RegisterProperty(PropertyId::Visibility, "visibility", "visible", true, false).AddParser("keyword", "visible, hidden, collapse=1"); // 'collapse' is 'hidden' (no table collapsing, phase 3 p3-paint)
	RegisterProperty(PropertyId::TextOverflow, "text-overflow", "clip", false, false).AddParser("keyword", "clip, ellipsis").AddParser("string");

	// Need some work on this if we are to include images.
	RegisterProperty(PropertyId::BackgroundColor, "background-color", "transparent", false, false).AddParser("color");

	RegisterProperty(PropertyId::Color, "color", "black", true, false).AddParser("color");

	RegisterProperty(PropertyId::CaretColor, "caret-color", "auto", true, false).AddParser("keyword", "auto").AddParser("color");

	RegisterProperty(PropertyId::ImageColor, "image-color", "white", false, false).AddParser("color");
	RegisterProperty(PropertyId::Opacity, "opacity", "1", false, false).AddParser("alpha_value");

	RegisterProperty(PropertyId::FontFamily, "font-family", "", true, true).AddParser("font_family").SetValueAsWritten();
	RegisterProperty(PropertyId::FontStyle, "font-style", "normal", true, true).AddParser("keyword", "normal, italic");
	RegisterProperty(PropertyId::FontWeight, "font-weight", "normal", true, true).AddParser("keyword", "normal=400, bold=700, bolder=1001, lighter=1002").AddParser("number");
	RegisterProperty(PropertyId::FontSize, "font-size", "16px", true, true)
		.AddParser("keyword", "xx-small, x-small, small, medium, large, x-large, xx-large, xxx-large, smaller, larger, math")
		.AddParser("length").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ParentFontSize);
	RegisterProperty(PropertyId::FontKerning, "font-kerning", "auto", true, true).AddParser("keyword", "auto, normal, none");
	RegisterProperty(PropertyId::LetterSpacing, "letter-spacing", "normal", true, true).AddParser("keyword", "normal").AddParser("length_percent").SetRelativeTarget(RelativeTarget::FontSize);
	RegisterShorthand(ShorthandId::Font, "font", "font-style, font-weight, font-size, line-height, font-family", ShorthandType::Custom);

	// 'start' and 'end' resolve against the direction, 'match-parent' to the parent's value (ComputeTextAlign, p3-text).
	RegisterProperty(PropertyId::TextAlign, "text-align", "start", true, true).AddParser("keyword", "left, right, center, justify, start, end, match-parent, -webkit-center, -webkit-left, -webkit-right");
	// The lines of the 'text-decoration' shorthand (p3-text); inherited, so that the decoration reaches the text inside.
	RegisterProperty(PropertyId::TextDecoration, "text-decoration-line", "none", true, false).AddParser("text_decoration_line");
	RegisterProperty(PropertyId::TextTransform, "text-transform", "none", true, true).AddParser("keyword", "none, capitalize, uppercase, lowercase");
	// 'white-space' is the shorthand of 'white-space-collapse' and 'text-wrap-mode' (p3-text): PropertyId::WhiteSpace is
	// not registered.
	RegisterProperty(PropertyId::WordBreak, "word-break", "normal", true, true).AddParser("keyword", "normal, break-all, break-word, keep-all");

	// 'normal' is 0 in flex and grid layout (the 'gap' parser, p3-grid); a column gap is a fraction of the width.
	RegisterProperty(PropertyId::RowGap, "row-gap", "0px", false, true).AddParser("gap").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::ColumnGap, "column-gap", "0px", false, true).AddParser("gap").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterShorthand(ShorthandId::Gap, "gap", "row-gap, column-gap", ShorthandType::Replicate);

	RegisterProperty(PropertyId::Cursor, "cursor", "", true, false).AddParser("cursor").SetValueAsWritten();

	// Functional property specifications.
	RegisterProperty(PropertyId::Drag, "drag", "none", false, false).AddParser("keyword", "none, drag, drag-drop, block, clone");
	RegisterProperty(PropertyId::TabIndex, "tab-index", "none", false, false).AddParser("keyword", "none, auto");
	RegisterProperty(PropertyId::Focus, "focus", "auto", true, false).AddParser("keyword", "none, auto");

	RegisterProperty(PropertyId::NavUp, "nav-up", "none", false, false).AddParser("keyword", "none, auto, horizontal, vertical, tree-order").AddParser("string");
	RegisterProperty(PropertyId::NavRight, "nav-right", "none", false, false).AddParser("keyword", "none, auto, horizontal, vertical, tree-order").AddParser("string");
	RegisterProperty(PropertyId::NavDown, "nav-down", "none", false, false).AddParser("keyword", "none, auto, horizontal, vertical, tree-order").AddParser("string");
	RegisterProperty(PropertyId::NavLeft, "nav-left", "none", false, false).AddParser("keyword", "none, auto, horizontal, vertical, tree-order").AddParser("string");
	RegisterShorthand(ShorthandId::Nav, "nav", "nav-up, nav-right, nav-down, nav-left", ShorthandType::Box);

	RegisterProperty(PropertyId::ScrollbarMargin, "scrollbar-margin", "0", false, false).AddParser("length");
	RegisterProperty(PropertyId::OverscrollBehavior, "overscroll-behavior", "auto", false, false).AddParser("keyword", "auto, contain");
	RegisterProperty(PropertyId::PointerEvents, "pointer-events", "auto", true, false).AddParser("keyword", "none, auto");

	// Perspective and Transform specifications
	RegisterProperty(PropertyId::Perspective, "perspective", "none", false, false).AddParser("keyword", "none").AddParser("length");
	RegisterProperty(PropertyId::PerspectiveOriginX, "perspective-origin-x", "50%", false, false).AddParser("keyword", "left, center, right").AddParser("length_percent");
	RegisterProperty(PropertyId::PerspectiveOriginY, "perspective-origin-y", "50%", false, false).AddParser("keyword", "top, center, bottom").AddParser("length_percent");
	RegisterShorthand(ShorthandId::PerspectiveOrigin, "perspective-origin", "perspective-origin-x, perspective-origin-y", ShorthandType::Custom);
	RegisterProperty(PropertyId::Transform, "transform", "none", false, false).AddParser("transform");
	RegisterProperty(PropertyId::TransformOriginX, "transform-origin-x", "50%", false, false).AddParser("keyword", "left, center, right").AddParser("length_percent");
	RegisterProperty(PropertyId::TransformOriginY, "transform-origin-y", "50%", false, false).AddParser("keyword", "top, center, bottom").AddParser("length_percent");
	RegisterProperty(PropertyId::TransformOriginZ, "transform-origin-z", "0", false, false).AddParser("length");
	RegisterShorthand(ShorthandId::TransformOrigin, "transform-origin", "transform-origin-x, transform-origin-y, transform-origin-z", ShorthandType::Custom);

	// Decorators and effects
	RegisterProperty(PropertyId::Decorator, "decorator", "", false, false).AddParser("decorator");
	RegisterProperty(PropertyId::FontEffect, "font-effect", "", true, false).AddParser("font_effect");

	RegisterProperty(PropertyId::Filter, "filter", "", false, false).AddParser("filter", "filter");
	RegisterProperty(PropertyId::BackdropFilter, "backdrop-filter", "", false, false).AddParser("filter");

	RegisterProperty(PropertyId::BoxShadow, "box-shadow", "none", false, false).AddParser("box_shadow");

	// Rare properties (not added to computed values)
	RegisterProperty(PropertyId::FillImage, "fill-image", "", false, false).AddParser("string");

	// Flexbox
	// The alignment keywords of CSS Box Alignment 3 (phase 3 groundwork), in the order of the enums in StyleTypes.h.
	RegisterProperty(PropertyId::AlignContent, "align-content", "normal", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("flex-start, flex-end, center, space-between, space-around, space-evenly, stretch, normal, start, end"));
	RegisterProperty(PropertyId::AlignItems, "align-items", "normal", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("flex-start, flex-end, center, baseline, stretch, normal, start, end, self-start, self-end, last baseline"));
	RegisterProperty(PropertyId::AlignSelf, "align-self", "auto", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("auto, flex-start, flex-end, center, baseline, stretch, normal, start, end, self-start, self-end, last baseline"));

	RegisterProperty(PropertyId::FlexBasis, "flex-basis", "auto", false, true).AddParser("keyword", "auto, min-content, max-content, fit-content, stretch, -webkit-fill-available=4, content").AddParser("length_percent");
	RegisterProperty(PropertyId::FlexDirection, "flex-direction", "row", false, true).AddParser("keyword", "row, row-reverse, column, column-reverse");

	RegisterProperty(PropertyId::FlexGrow, "flex-grow", "0", false, true).AddParser("non_negative_number");
	RegisterProperty(PropertyId::FlexShrink, "flex-shrink", "1", false, true).AddParser("non_negative_number");
	RegisterProperty(PropertyId::FlexWrap, "flex-wrap", "nowrap", false, true).AddParser("keyword", "nowrap, wrap, wrap-reverse, balance, wrap balance=3, balance wrap=3, wrap-reverse balance=4, balance wrap-reverse=4");
	RegisterProperty(PropertyId::JustifyContent, "justify-content", "normal", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("flex-start, flex-end, center, space-between, space-around, space-evenly, stretch, normal, start, end, left, right"));

	RegisterShorthand(ShorthandId::Flex, "flex", "flex-grow, flex-shrink, flex-basis", ShorthandType::Flex);
	RegisterShorthand(ShorthandId::FlexFlow, "flex-flow", "flex-direction, flex-wrap", ShorthandType::FallThrough);

	// Internationalization properties (internal)
	RegisterProperty(PropertyId::RmlUi_Language, "-rmlui-language", "", true, true).AddParser("string");
	RegisterProperty(PropertyId::RmlUi_Direction, "-rmlui-direction", "auto", true, true).AddParser("keyword", "auto, ltr, rtl");

	/*
	  Phase 3 sections (docs/html-ui/phase-3-fork-patches.md 3.0, 3.1.3, Appendix B). Every new id is declared in ID.h by
	  groundwork; each package registers its properties and shorthands in its own section below, and only there. Ids
	  not registered yet are reserved at the end of this function: they have no name, and nothing parses into them.
	*/

	// -- Groundwork (p3-base) --
	RegisterProperty(PropertyId::Order, "order", "0", false, true).AddParser("number");
	// 'legacy' behaves as 'normal'; justify-items and justify-self have no effect in flex layout (grid: p3-grid).
	RegisterProperty(PropertyId::JustifyItems, "justify-items", "legacy", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("flex-start, flex-end, center, baseline, stretch, normal, start, end, self-start, self-end, last baseline, left, right, legacy"));
	RegisterProperty(PropertyId::JustifySelf, "justify-self", "auto", false, true)
		.AddParser("alignment", MakeAlignmentKeywords("auto, flex-start, flex-end, center, baseline, stretch, normal, start, end, self-start, self-end, last baseline, left, right"));
	RegisterShorthand(ShorthandId::PlaceContent, "place-content", "align-content, justify-content", ShorthandType::Custom);
	RegisterShorthand(ShorthandId::PlaceItems, "place-items", "align-items, justify-items", ShorthandType::Custom);
	RegisterShorthand(ShorthandId::PlaceSelf, "place-self", "align-self, justify-self", ShorthandType::Custom);
	for (ShorthandId id : {ShorthandId::PlaceContent, ShorthandId::PlaceItems, ShorthandId::PlaceSelf})
		properties.SetShorthandParser(id, &ParsePlaceShorthand);
	// A browser optimization hint with no visual effect: accepted, so it neither fails nor needs a rewrite.
	RegisterProperty(PropertyId::WillChange, "will-change", "auto", false, false).AddParser("keyword", "auto").AddParser("string");

	// -- Values (p3-values) --
	properties.SetShorthandParser(ShorthandId::Font, &ParseFontShorthand);
	properties.SetShorthandParser(ShorthandId::TransformOrigin, &ParseTransformOriginShorthand);
	properties.SetShorthandParser(ShorthandId::PerspectiveOrigin, &ParsePerspectiveOriginShorthand);
	// (The logical shorthands are registered after every package's properties: they name some, such as p3-paint's
	// border-<side> shorthands. See the end of this function.)

	// -- Cascade (p3-cascade) --
	{
		// 'all' takes CSS-wide keywords only (Cascade::ParseAllShorthand), which set every registered property but direction
		// and RmlUi's interaction properties (Cascade::IsReachedByAll), whenever they are registered. Its items are the
		// properties registered up to here: what a var() value of 'all' and GetShorthandUnderlyingProperties reach.
		String all_properties;
		for (const PropertyId id : properties.GetRegisteredProperties())
		{
			const String& name = GetPropertyName(id);
			if (!Cascade::IsReachedByAll(id))
				continue;
			if (!all_properties.empty())
				all_properties += ", ";
			all_properties += name;
		}
		RegisterShorthand(ShorthandId::All, "all", all_properties, ShorthandType::Custom);
		properties.SetShorthandParser(ShorthandId::All, &Cascade::ParseAllShorthand);
	}
	// Generated content and lists (3.6): pseudo-elements, counters, list markers.
	RegisterProperty(PropertyId::Content, "content", "normal", false, true).AddParser("keyword", "normal, none").AddParser("content");
	RegisterProperty(PropertyId::Quotes, "quotes", "auto", true, false).AddParser("keyword", "auto, none").AddParser("quotes");
	RegisterProperty(PropertyId::CounterReset, "counter-reset", "none", false, false).AddParser("keyword", "none").AddParser("counter_list", "reversed");
	RegisterProperty(PropertyId::CounterIncrement, "counter-increment", "none", false, false).AddParser("keyword", "none").AddParser("counter_list");
	RegisterProperty(PropertyId::CounterSet, "counter-set", "none", false, false).AddParser("keyword", "none").AddParser("counter_list");
	RegisterProperty(PropertyId::ListStyleType, "list-style-type", "disc", true, true)
		.AddParser("list_style_type",
			"disc, circle, square, decimal, decimal-leading-zero, lower-alpha, lower-latin, upper-alpha, upper-latin, lower-roman, upper-roman, "
			"lower-greek, disclosure-open, disclosure-closed, none");
	RegisterProperty(PropertyId::ListStylePosition, "list-style-position", "outside", true, true).AddParser("keyword", "outside, inside");
	RegisterProperty(PropertyId::ListStyleImage, "list-style-image", "none", true, true).AddParser("keyword", "none").AddParser("url");
	RegisterShorthand(ShorthandId::ListStyle, "list-style", "list-style-type, list-style-position, list-style-image", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::ListStyle, &ParseListStyleShorthand);

	// -- Paint (p3-paint) --
	// Borders (phase 3, 3.7): per-side styles, widths with the CSS keywords and initial 'medium' (a side whose style is
	// none or hidden computes its width to 0), and the CSS shorthands in any order. Parsers and computed values:
	// ShorthandBorder.cpp.
	for (PropertyId id : {PropertyId::BorderTopWidth, PropertyId::BorderRightWidth, PropertyId::BorderBottomWidth, PropertyId::BorderLeftWidth})
	{
		static const char* const names[] = {"border-top-width", "border-right-width", "border-bottom-width", "border-left-width"};
		RegisterProperty(id, names[int(id) - int(PropertyId::BorderTopWidth)], "medium", false, true).AddParser("line_width");
	}
	RegisterShorthand(ShorthandId::BorderWidth, "border-width", "border-top-width, border-right-width, border-bottom-width, border-left-width", ShorthandType::Box);
	RegisterProperty(PropertyId::BorderTopStyle, "border-top-style", "none", false, true).AddParser("keyword", "none, hidden, dotted, dashed, solid, double, groove, ridge, inset, outset");
	RegisterProperty(PropertyId::BorderRightStyle, "border-right-style", "none", false, true).AddParser("keyword", "none, hidden, dotted, dashed, solid, double, groove, ridge, inset, outset");
	RegisterProperty(PropertyId::BorderBottomStyle, "border-bottom-style", "none", false, true).AddParser("keyword", "none, hidden, dotted, dashed, solid, double, groove, ridge, inset, outset");
	RegisterProperty(PropertyId::BorderLeftStyle, "border-left-style", "none", false, true).AddParser("keyword", "none, hidden, dotted, dashed, solid, double, groove, ridge, inset, outset");
	RegisterShorthand(ShorthandId::BorderStyle, "border-style", "border-top-style, border-right-style, border-bottom-style, border-left-style", ShorthandType::Box);
	RegisterShorthand(ShorthandId::BorderTop, "border-top", "border-top-width, border-top-style, border-top-color", ShorthandType::AnyOrder);
	RegisterShorthand(ShorthandId::BorderRight, "border-right", "border-right-width, border-right-style, border-right-color", ShorthandType::AnyOrder);
	RegisterShorthand(ShorthandId::BorderBottom, "border-bottom", "border-bottom-width, border-bottom-style, border-bottom-color", ShorthandType::AnyOrder);
	RegisterShorthand(ShorthandId::BorderLeft, "border-left", "border-left-width, border-left-style, border-left-color", ShorthandType::AnyOrder);
	// 'border-image' is accepted as 'none' only, so that 'border' can reset it (compat emulates its other values).
	RegisterProperty(PropertyId::BorderImage, "border-image", "none", false, false).AddParser("keyword", "none");
	RegisterShorthand(ShorthandId::Border, "border", "border-width, border-style, border-color, border-image", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::Border, &ParseBorderShorthand);

	// object-fit and object-position (phase 3, 3.9.3), read by replaced elements. The position's axes are lengths or
	// percentages of the free space; the shorthand takes the CSS <position> (keywords, edge offsets).
	RegisterProperty(PropertyId::ObjectFit, "object-fit", "fill", false, false).AddParser("keyword", "fill, contain, cover, none, scale-down");
	RegisterProperty(PropertyId::ObjectPositionX, "object-position-x", "50%", false, false).AddParser("length_percent");
	RegisterProperty(PropertyId::ObjectPositionY, "object-position-y", "50%", false, false).AddParser("length_percent");
	RegisterShorthand(ShorthandId::ObjectPosition, "object-position", "object-position-x, object-position-y", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::ObjectPosition, &ParsePositionShorthand);

	// Background and mask layers (phase 3, 3.9): comma lists, one item per layer (BackgroundLayerValues.h); url(),
	// gradients and every registered decorator function are images. 'background' takes the colour in its last layer.
	RegisterProperty(PropertyId::BackgroundImage, "background-image", "none", false, false).AddParser("image_layers");
	RegisterProperty(PropertyId::BackgroundPositionX, "background-position-x", "0%", false, false).AddParser("layer_position_x");
	RegisterProperty(PropertyId::BackgroundPositionY, "background-position-y", "0%", false, false).AddParser("layer_position_y");
	RegisterShorthand(ShorthandId::BackgroundPosition, "background-position", "background-position-x, background-position-y", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::BackgroundPosition, &ParseLayerPositionShorthand);
	RegisterProperty(PropertyId::BackgroundSize, "background-size", "auto", false, false).AddParser("layer_size");
	RegisterProperty(PropertyId::BackgroundRepeat, "background-repeat", "repeat", false, false).AddParser("layer_repeat");
	RegisterProperty(PropertyId::BackgroundOrigin, "background-origin", "padding-box", false, false).AddParser("layer_box");
	RegisterProperty(PropertyId::BackgroundClip, "background-clip", "border-box", false, false).AddParser("layer_box");
	RegisterProperty(PropertyId::BackgroundAttachment, "background-attachment", "scroll", false, false).AddParser("layer_attachment");
	RegisterShorthand(ShorthandId::Background, "background",
		"background-color, background-image, background-position-x, background-position-y, background-size, background-repeat, "
		"background-origin, background-clip, background-attachment",
		ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::Background, &ParseBackgroundShorthand);
	// (RCSS decorator syntax in mask-image keeps working: decorator functions are images.)
	RegisterProperty(PropertyId::MaskImage, "mask-image", "none", false, false).AddParser("image_layers", "mask");
	RegisterProperty(PropertyId::MaskPositionX, "mask-position-x", "0%", false, false).AddParser("layer_position_x");
	RegisterProperty(PropertyId::MaskPositionY, "mask-position-y", "0%", false, false).AddParser("layer_position_y");
	RegisterShorthand(ShorthandId::MaskPosition, "mask-position", "mask-position-x, mask-position-y", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::MaskPosition, &ParseLayerPositionShorthand);
	RegisterProperty(PropertyId::MaskSize, "mask-size", "auto", false, false).AddParser("layer_size");
	RegisterProperty(PropertyId::MaskRepeat, "mask-repeat", "repeat", false, false).AddParser("layer_repeat");
	RegisterShorthand(ShorthandId::Mask, "mask", "mask-image, mask-position-x, mask-position-y, mask-size, mask-repeat", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::Mask, &ParseMaskShorthand);

	// Stacking contexts (phase 3, 3.8.2).
	RegisterProperty(PropertyId::Isolation, "isolation", "auto", false, false).AddParser("keyword", "auto, isolate");

	// Outline (phase 3, 3.7.1, 3.7.4): drawn outside the border box, without a place in the layout. The style keywords
	// have the values of Style::BorderStyle (no 'hidden'; 'auto' is the focus ring); the colour is 'currentcolor' unless
	// declared ('invert' is drawn as 'currentcolor', as browsers do).
	RegisterProperty(PropertyId::OutlineWidth, "outline-width", "medium", false, false).AddParser("line_width");
	RegisterProperty(PropertyId::OutlineStyle, "outline-style", "none", false, false)
		.AddParser("keyword", "none=0, dotted=2, dashed=3, solid=4, double=5, groove=6, ridge=7, inset=8, outset=9, auto=10");
	RegisterProperty(PropertyId::OutlineColor, "outline-color", "currentcolor", false, false).AddParser("keyword", "currentcolor, invert").AddParser("color");
	RegisterProperty(PropertyId::OutlineOffset, "outline-offset", "0px", false, false).AddParser("length");
	RegisterShorthand(ShorthandId::Outline, "outline", "outline-width, outline-style, outline-color", ShorthandType::AnyOrder);

	// Radii (phase 3, 3.7.1): every corner has a horizontal and a vertical radius, a length or a percentage of the border
	// box's width or height; 'border-<corner>-radius: <x> <y>?' and 'border-radius: <1-4 values> [ / <1-4 values> ]?' are
	// Custom shorthands of them (the 6.3 ids of the four corners are not registered any more).
	{
		static const char* const corners[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
		static const ShorthandId corner_ids[] = {ShorthandId::BorderTopLeftRadius, ShorthandId::BorderTopRightRadius,
			ShorthandId::BorderBottomRightRadius, ShorthandId::BorderBottomLeftRadius};
		for (int corner = 0; corner < 4; corner++)
		{
			const String name = CreateString("border-%s-radius", corners[corner]);
			const PropertyId x_id = PropertyId(int(PropertyId::RmlUi_BorderTopLeftRadiusX) + 2 * corner);
			RegisterProperty(x_id, "-rmlui-" + name + "-x", "0px", false, false).AddParser("length_percent");
			RegisterProperty(PropertyId(int(x_id) + 1), "-rmlui-" + name + "-y", "0px", false, false).AddParser("length_percent");
			RegisterShorthand(corner_ids[corner], name, "-rmlui-" + name + "-x, -rmlui-" + name + "-y", ShorthandType::Custom);
			properties.SetShorthandParser(corner_ids[corner], &ParseCornerRadiusShorthand);
		}
		RegisterShorthand(ShorthandId::BorderRadius, "border-radius",
			"border-top-left-radius, border-top-right-radius, border-bottom-right-radius, border-bottom-left-radius", ShorthandType::Custom);
		properties.SetShorthandParser(ShorthandId::BorderRadius, &ParseBorderRadiusShorthand);
	}

	// -- Text (p3-text) --
	RegisterProperty(PropertyId::WordSpacing, "word-spacing", "normal", true, true).AddParser("keyword", "normal").AddParser("length_percent").SetRelativeTarget(RelativeTarget::FontSize);
	RegisterProperty(PropertyId::OverflowWrap, "overflow-wrap", "normal", true, true).AddParser("keyword", "normal, anywhere, break-word");
	RegisterShorthand(ShorthandId::WordWrap, "word-wrap", "overflow-wrap", ShorthandType::FallThrough);
	RegisterProperty(PropertyId::WhiteSpaceCollapse, "white-space-collapse", "collapse", true, true).AddParser("keyword", "collapse, preserve, preserve-breaks, preserve-spaces, break-spaces");
	RegisterProperty(PropertyId::TextWrapMode, "text-wrap-mode", "wrap", true, true).AddParser("keyword", "wrap, nowrap");
	RegisterShorthand(ShorthandId::WhiteSpace, "white-space", "white-space-collapse, text-wrap-mode", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::WhiteSpace, &ParseWhiteSpaceShorthand);
	RegisterShorthand(ShorthandId::TextWrap, "text-wrap", "text-wrap-mode", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::TextWrap, &ParseTextWrapShorthand);
	// The decoration longhands are inherited, as text-decoration-line is (a fork-only choice: RmlUi draws decorations per text
	// element, so they reach the text inside the decorating box through inheritance).
	RegisterProperty(PropertyId::TextDecorationColor, "text-decoration-color", "currentcolor", true, false).AddParser("keyword", "currentcolor").AddParser("color");
	RegisterProperty(PropertyId::TextDecorationStyle, "text-decoration-style", "solid", true, false).AddParser("keyword", "solid, double, dotted, dashed, wavy");
	RegisterProperty(PropertyId::TextDecorationThickness, "text-decoration-thickness", "auto", true, false).AddParser("keyword", "auto, from-font").AddParser("length_percent").SetRelativeTarget(RelativeTarget::FontSize);
	RegisterProperty(PropertyId::TextUnderlineOffset, "text-underline-offset", "auto", true, false).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::FontSize);
	RegisterShorthand(ShorthandId::TextDecoration, "text-decoration", "text-decoration-line, text-decoration-style, text-decoration-color, text-decoration-thickness", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::TextDecoration, &ParseTextDecorationShorthand);
	RegisterProperty(PropertyId::TextShadow, "text-shadow", "none", true, false).AddParser("keyword", "none").AddParser("text_shadow");
	RegisterProperty(PropertyId::UserSelect, "user-select", "auto", false, false).AddParser("keyword", "auto, text, none, contain, all");
	RegisterProperty(PropertyId::AccentColor, "accent-color", "auto", true, false).AddParser("keyword", "auto").AddParser("color");

	// -- Text (p6-justify) --
	// The keywords Chromium parses: no 'match-parent' for 'text-align-last', no 'justify-all' for 'text-align'.
	RegisterProperty(PropertyId::TextAlignLast, "text-align-last", "auto", true, true).AddParser("keyword", "auto, start, end, left, right, center, justify");
	RegisterProperty(PropertyId::TextJustify, "text-justify", "auto", true, true).AddParser("keyword", "auto, none, inter-word, inter-character, distribute=3");

	// -- Layout (p3-layout) --
	// The sizing keywords of width, height, min-*, max-* and flex-basis, 'overflow: clip' and 'flex-wrap: balance' are in
	// the keyword lists of those properties above.
	RegisterProperty(PropertyId::OverflowClipMargin, "overflow-clip-margin", "0px", false, false).AddParser("length");
	RegisterProperty(PropertyId::AspectRatio, "aspect-ratio", "auto", false, true).AddParser("aspect_ratio");
	RegisterProperty(PropertyId::ScrollbarGutter, "scrollbar-gutter", "auto", false, true)
		.AddParser("keyword", "auto, stable, stable both-edges, both-edges stable=2");
	RegisterProperty(PropertyId::FlexLineCount, "flex-line-count", "1", false, true).AddParser("number");

	// -- Animation (p3-animation) --
	// The transition and animation longhands are comma-separated lists (PropertyParserAnimation); the 'transition' and
	// 'animation' shorthands set them all (ShorthandAnimation.cpp). 6.3's 'transition' and 'animation' properties
	// (PropertyId::Transition, PropertyId::Animation) are not registered: their names belong to the shorthands.
	RegisterProperty(PropertyId::TransitionProperty, "transition-property", "all", false, false).AddParser("transition_property");
	RegisterProperty(PropertyId::TransitionDuration, "transition-duration", "0s", false, false).AddParser("animation_duration");
	RegisterProperty(PropertyId::TransitionTimingFunction, "transition-timing-function", "ease", false, false).AddParser("animation_easing");
	RegisterProperty(PropertyId::TransitionDelay, "transition-delay", "0s", false, false).AddParser("animation_time");
	RegisterProperty(PropertyId::TransitionBehavior, "transition-behavior", "normal", false, false)
		.AddParser("animation_keyword", GetAnimationKeywordList(PropertyId::TransitionBehavior));
	RegisterShorthand(ShorthandId::Transition, "transition",
		"transition-property, transition-duration, transition-timing-function, transition-delay, transition-behavior", ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::Transition, &ParseTransitionShorthand);

	RegisterProperty(PropertyId::AnimationName, "animation-name", "none", false, false).AddParser("animation_name");
	RegisterProperty(PropertyId::AnimationDuration, "animation-duration", "0s", false, false).AddParser("animation_duration");
	RegisterProperty(PropertyId::AnimationTimingFunction, "animation-timing-function", "ease", false, false).AddParser("animation_easing");
	RegisterProperty(PropertyId::AnimationDelay, "animation-delay", "0s", false, false).AddParser("animation_time");
	RegisterProperty(PropertyId::AnimationIterationCount, "animation-iteration-count", "1", false, false).AddParser("animation_iteration_count");
	RegisterProperty(PropertyId::AnimationDirection, "animation-direction", "normal", false, false)
		.AddParser("animation_keyword", GetAnimationKeywordList(PropertyId::AnimationDirection));
	RegisterProperty(PropertyId::AnimationFillMode, "animation-fill-mode", "none", false, false)
		.AddParser("animation_keyword", GetAnimationKeywordList(PropertyId::AnimationFillMode));
	RegisterProperty(PropertyId::AnimationPlayState, "animation-play-state", "running", false, false)
		.AddParser("animation_keyword", GetAnimationKeywordList(PropertyId::AnimationPlayState));
	// 'add' and 'accumulate' are accepted and composite as 'replace' (the compat layer reports them).
	RegisterProperty(PropertyId::AnimationComposition, "animation-composition", "replace", false, false)
		.AddParser("animation_keyword", GetAnimationKeywordList(PropertyId::AnimationComposition));
	RegisterShorthand(ShorthandId::Animation, "animation",
		"animation-name, animation-duration, animation-timing-function, animation-delay, animation-iteration-count, animation-direction, "
		"animation-fill-mode, animation-play-state",
		ShorthandType::Custom);
	properties.SetShorthandParser(ShorthandId::Animation, &ParseAnimationShorthand);

	// The individual transform properties (CSS Transforms 2), composed before 'transform'.
	RegisterProperty(PropertyId::Translate, "translate", "none", false, false).AddParser("translate");
	RegisterProperty(PropertyId::Rotate, "rotate", "none", false, false).AddParser("rotate");
	RegisterProperty(PropertyId::Scale, "scale", "none", false, false).AddParser("scale");

	// -- Grid (p3-grid) --
	RegisterGridProperties(properties);

	// -- Values (p3-values), continued: the logical shorthands, which name the physical properties and shorthands of
	// the other packages (border-<side> of p3-paint) --
	size_t num_logical_shorthands = 0;
	const LogicalShorthand* logical_shorthands = GetLogicalShorthands(num_logical_shorthands);
	for (size_t i = 0; i < num_logical_shorthands; i++)
	{
		const LogicalShorthand& logical = logical_shorthands[i];
		RegisterShorthand(logical.id, logical.name, logical.items, logical.type);
		if (logical.parser)
			properties.SetShorthandParser(logical.id, logical.parser);
	}

	instance->properties.property_map->ReserveUnnamed(PropertyId::NumDefinedIds);
	instance->properties.shorthand_map->ReserveUnnamed(ShorthandId::NumDefinedIds);

	// Fork (phase 5, P5-1): a filter list applied before 'filter' (the engine's gradient text of effect layers); after the
	// reservation, so its id is past the defined ones
	text_fill_id = RegisterProperty(PropertyId::Invalid, "-jfx-text-fill", "", false, false).AddParser("filter", "filter").GetId();

	RMLUI_ASSERTMSG(instance->properties.shorthand_map->AssertAllInserted(ShorthandId::NumDefinedIds), "Missing specification for one or more Shorthand IDs.");
	RMLUI_ASSERTMSG(instance->properties.property_map->AssertAllInserted(PropertyId::NumDefinedIds), "Missing specification for one or more Property IDs.");
	// clang-format on
}

} // namespace Rml
