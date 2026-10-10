#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "IdNameMap.h"
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
#include "ValuesShorthands.h"

namespace Rml {

static StyleSheetSpecification* instance = nullptr;

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
	PropertyParserAnimation animation = PropertyParserAnimation(PropertyParserAnimation::ANIMATION_PARSER);
	PropertyParserAnimation transition = PropertyParserAnimation(PropertyParserAnimation::TRANSITION_PARSER);
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

StyleSheetSpecification::StyleSheetSpecification() :
	// Reserve space for all defined ids and some more for custom properties
	properties((size_t)PropertyId::MaxNumIds, 2 * (size_t)ShorthandId::NumDefinedIds)
{
	RMLUI_ASSERT(instance == nullptr);
	instance = this;

	default_parsers.reset(new DefaultStyleSheetParsers);
}

StyleSheetSpecification::~StyleSheetSpecification()
{
	RMLUI_ASSERT(instance == this);
	instance = nullptr;
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
	RegisterParser("animation", &default_parsers->animation);
	RegisterParser("transition", &default_parsers->transition);
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

	// -- Paint (p3-paint) --

	// -- Text (p3-text) --

	// -- Layout (p3-layout) --

	// -- Animation (p3-animation) --

	// -- Grid (p3-grid) --
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

	RegisterProperty(PropertyId::BorderTopWidth, "border-top-width", "0px", false, true).AddParser("length");
	RegisterProperty(PropertyId::BorderRightWidth, "border-right-width", "0px", false, true).AddParser("length");
	RegisterProperty(PropertyId::BorderBottomWidth, "border-bottom-width", "0px", false, true).AddParser("length");
	RegisterProperty(PropertyId::BorderLeftWidth, "border-left-width", "0px", false, true).AddParser("length");
	RegisterShorthand(ShorthandId::BorderWidth, "border-width", "border-top-width, border-right-width, border-bottom-width, border-left-width", ShorthandType::Box);

	RegisterProperty(PropertyId::BorderTopColor, "border-top-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderRightColor, "border-right-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderBottomColor, "border-bottom-color", "currentcolor", false, false).AddParser("color");
	RegisterProperty(PropertyId::BorderLeftColor, "border-left-color", "currentcolor", false, false).AddParser("color");
	RegisterShorthand(ShorthandId::BorderColor, "border-color", "border-top-color, border-right-color, border-bottom-color, border-left-color", ShorthandType::Box);

	RegisterShorthand(ShorthandId::BorderTop, "border-top", "border-top-width, border-top-color", ShorthandType::FallThrough);
	RegisterShorthand(ShorthandId::BorderRight, "border-right", "border-right-width, border-right-color", ShorthandType::FallThrough);
	RegisterShorthand(ShorthandId::BorderBottom, "border-bottom", "border-bottom-width, border-bottom-color", ShorthandType::FallThrough);
	RegisterShorthand(ShorthandId::BorderLeft, "border-left", "border-left-width, border-left-color", ShorthandType::FallThrough);
	RegisterShorthand(ShorthandId::Border, "border", "border-top, border-right, border-bottom, border-left", ShorthandType::RecursiveRepeat);

	RegisterProperty(PropertyId::BorderTopLeftRadius, "border-top-left-radius", "0px", false, false).AddParser("length");
	RegisterProperty(PropertyId::BorderTopRightRadius, "border-top-right-radius", "0px", false, false).AddParser("length");
	RegisterProperty(PropertyId::BorderBottomRightRadius, "border-bottom-right-radius", "0px", false, false).AddParser("length");
	RegisterProperty(PropertyId::BorderBottomLeftRadius, "border-bottom-left-radius", "0px", false, false).AddParser("length");
	RegisterShorthand(ShorthandId::BorderRadius, "border-radius", "border-top-left-radius, border-top-right-radius, border-bottom-right-radius, border-bottom-left-radius", ShorthandType::Box);

	// 'grid=14, inline-grid=15' join the display keywords with grid layout (p3-grid): ForkFeature::Grid's probe parses them.
	RegisterProperty(PropertyId::Display, "display", "inline", false, true)
		.AddParser("keyword", "none, block, inline, inline-block, flow-root, flex, inline-flex, table, inline-table, table-row, table-row-group, table-column, table-column-group, table-cell, list-item=16, contents=17");
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

	RegisterProperty(PropertyId::Width, "width", "auto", false, true).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MinWidth, "min-width", "auto", false, true).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);
	RegisterProperty(PropertyId::MaxWidth, "max-width", "none", false, true).AddParser("keyword", "none").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockWidth);

	RegisterProperty(PropertyId::Height, "height", "auto", false, true).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::MinHeight, "min-height", "auto", false, true).AddParser("keyword", "auto").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::MaxHeight, "max-height", "none", false, true).AddParser("keyword", "none").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);

#ifdef RMLUI_MATH_EXPRESSIONS
	RegisterProperty(PropertyId::LineHeight, "line-height", "normal", true, true).AddParser("keyword", "normal").AddParser("line_height").SetRelativeTarget(RelativeTarget::FontSize);
#else
	RegisterProperty(PropertyId::LineHeight, "line-height", "normal", true, true).AddParser("keyword", "normal").AddParser("number_length_percent").SetRelativeTarget(RelativeTarget::FontSize);
#endif
	RegisterProperty(PropertyId::VerticalAlign, "vertical-align", "baseline", false, true)
		.AddParser("keyword", "baseline, middle, sub, super, text-top, text-bottom, top, center, bottom")
		.AddParser("length_percent").SetRelativeTarget(RelativeTarget::LineHeight);

	RegisterProperty(PropertyId::OverflowX, "overflow-x", "visible", false, true).AddParser("keyword", "visible, hidden, auto, scroll");
	RegisterProperty(PropertyId::OverflowY, "overflow-y", "visible", false, true).AddParser("keyword", "visible, hidden, auto, scroll");
	RegisterShorthand(ShorthandId::Overflow, "overflow", "overflow-x, overflow-y", ShorthandType::Replicate);
	RegisterProperty(PropertyId::Clip, "clip", "auto", false, false).AddParser("keyword", "auto, none, always").AddParser("number");
	RegisterProperty(PropertyId::Visibility, "visibility", "visible", true, false).AddParser("keyword", "visible, hidden");
	RegisterProperty(PropertyId::TextOverflow, "text-overflow", "clip", false, false).AddParser("keyword", "clip, ellipsis").AddParser("string");

	// Need some work on this if we are to include images.
	RegisterProperty(PropertyId::BackgroundColor, "background-color", "transparent", false, false).AddParser("color");
	RegisterShorthand(ShorthandId::Background, "background", "background-color", ShorthandType::FallThrough);

	RegisterProperty(PropertyId::Color, "color", "black", true, false).AddParser("color");

	RegisterProperty(PropertyId::CaretColor, "caret-color", "auto", true, false).AddParser("keyword", "auto").AddParser("color");

	RegisterProperty(PropertyId::ImageColor, "image-color", "white", false, false).AddParser("color");
	RegisterProperty(PropertyId::Opacity, "opacity", "1", false, false).AddParser("alpha_value");

	RegisterProperty(PropertyId::FontFamily, "font-family", "", true, true).AddParser("font_family");
	RegisterProperty(PropertyId::FontStyle, "font-style", "normal", true, true).AddParser("keyword", "normal, italic");
	RegisterProperty(PropertyId::FontWeight, "font-weight", "normal", true, true).AddParser("keyword", "normal=400, bold=700, bolder=1001, lighter=1002").AddParser("number");
	RegisterProperty(PropertyId::FontSize, "font-size", "16px", true, true)
		.AddParser("keyword", "xx-small, x-small, small, medium, large, x-large, xx-large, xxx-large, smaller, larger, math")
		.AddParser("length").AddParser("length_percent").SetRelativeTarget(RelativeTarget::ParentFontSize);
	RegisterProperty(PropertyId::FontKerning, "font-kerning", "auto", true, true).AddParser("keyword", "auto, normal, none");
	RegisterProperty(PropertyId::LetterSpacing, "letter-spacing", "normal", true, true).AddParser("keyword", "normal").AddParser("length");
	RegisterShorthand(ShorthandId::Font, "font", "font-style, font-weight, font-size, line-height, font-family", ShorthandType::Custom);

	RegisterProperty(PropertyId::TextAlign, "text-align", "left", true, true).AddParser("keyword", "left, right, center, justify");
	RegisterProperty(PropertyId::TextDecoration, "text-decoration", "none", true, false).AddParser("keyword", "none, underline, overline, line-through");
	RegisterProperty(PropertyId::TextTransform, "text-transform", "none", true, true).AddParser("keyword", "none, capitalize, uppercase, lowercase");
	RegisterProperty(PropertyId::WhiteSpace, "white-space", "normal", true, true).AddParser("keyword", "normal, pre, nowrap, pre-wrap, pre-line");
	RegisterProperty(PropertyId::WordBreak, "word-break", "normal", true, true).AddParser("keyword", "normal, break-all, break-word");

	RegisterProperty(PropertyId::RowGap, "row-gap", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterProperty(PropertyId::ColumnGap, "column-gap", "0px", false, true).AddParser("length_percent").SetRelativeTarget(RelativeTarget::ContainingBlockHeight);
	RegisterShorthand(ShorthandId::Gap, "gap", "row-gap, column-gap", ShorthandType::Replicate);

	RegisterProperty(PropertyId::Cursor, "cursor", "", true, false).AddParser("cursor");

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

	RegisterProperty(PropertyId::Transition, "transition", "none", false, false).AddParser("transition");
	RegisterProperty(PropertyId::Animation, "animation", "none", false, false).AddParser("animation");

	// Decorators and effects
	RegisterProperty(PropertyId::Decorator, "decorator", "", false, false).AddParser("decorator");
	RegisterProperty(PropertyId::MaskImage, "mask-image", "", false, false).AddParser("decorator");
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

	RegisterProperty(PropertyId::FlexBasis, "flex-basis", "auto", false, true).AddParser("keyword", "auto").AddParser("length_percent");
	RegisterProperty(PropertyId::FlexDirection, "flex-direction", "row", false, true).AddParser("keyword", "row, row-reverse, column, column-reverse");

	RegisterProperty(PropertyId::FlexGrow, "flex-grow", "0", false, true).AddParser("non_negative_number");
	RegisterProperty(PropertyId::FlexShrink, "flex-shrink", "1", false, true).AddParser("non_negative_number");
	RegisterProperty(PropertyId::FlexWrap, "flex-wrap", "nowrap", false, true).AddParser("keyword", "nowrap, wrap, wrap-reverse");
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
	// A list of quoted and unquoted names reaches the font_family parser as written.
	properties.property_ids_raw_value.Insert(PropertyId::FontFamily);
	properties.property_ids_raw_value.Insert(PropertyId::Cursor);
	size_t num_logical_shorthands = 0;
	const LogicalShorthand* logical_shorthands = GetLogicalShorthands(num_logical_shorthands);
	for (size_t i = 0; i < num_logical_shorthands; i++)
	{
		const LogicalShorthand& logical = logical_shorthands[i];
		RegisterShorthand(logical.id, logical.name, logical.items, logical.type);
		if (logical.parser)
			properties.SetShorthandParser(logical.id, logical.parser);
	}

	// -- Cascade (p3-cascade) --

	// -- Paint (p3-paint) --

	// -- Text (p3-text) --

	// -- Layout (p3-layout) --

	// -- Animation (p3-animation) --

	// -- Grid (p3-grid) --

	instance->properties.property_map->ReserveUnnamed(PropertyId::NumDefinedIds);
	instance->properties.shorthand_map->ReserveUnnamed(ShorthandId::NumDefinedIds);

	RMLUI_ASSERTMSG(instance->properties.shorthand_map->AssertAllInserted(ShorthandId::NumDefinedIds), "Missing specification for one or more Shorthand IDs.");
	RMLUI_ASSERTMSG(instance->properties.property_map->AssertAllInserted(PropertyId::NumDefinedIds), "Missing specification for one or more Property IDs.");
	// clang-format on
}

} // namespace Rml
