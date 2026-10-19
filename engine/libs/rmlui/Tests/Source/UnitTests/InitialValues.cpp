#include "../../../Source/Core/ComputeProperty.h"
#include "../Common/TestsShell.h"
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/Transform.h>
#include <doctest.h>
#include <type_traits>

using namespace Rml;

// Phase 3 groundwork (docs/html-ui/phase-3-fork-patches.md 3.1.2, FORK.md P3-4): the fork's defaults are the CSS
// initial values, and for every registered property the computed value of its registered default equals the value an
// element computes without any declaration, so the two cannot drift apart again.

namespace {

String Text(float value)
{
	return CreateString("%g", value);
}
String Text(bool value)
{
	return value ? "true" : "false";
}
template <typename T, typename std::enable_if_t<std::is_enum_v<T> || std::is_integral_v<T>, int> = 0>
String Text(T value)
{
	return CreateString("%lld", (long long)value);
}
String Text(const String& value)
{
	return '"' + value + '"';
}
String Text(Colourb value)
{
	return CreateString("rgba(%d,%d,%d,%d)", value.red, value.green, value.blue, value.alpha);
}
String Text(const Style::LengthPercentageAuto& value)
{
	String result = Text(value.type) + ":" + Text(value.value);
#ifdef RMLUI_MATH_EXPRESSIONS
	if (value.calculation)
		result += ":calc";
#endif
	return result;
}
String Text(const Style::LengthPercentage& value)
{
	String result = Text(value.type) + ":" + Text(value.value);
#ifdef RMLUI_MATH_EXPRESSIONS
	if (value.calculation)
		result += ":calc";
#endif
	return result;
}
String Text(const Style::NumberAuto& value)
{
	return Text(value.type) + ":" + Text(value.value);
}
String Text(const Style::LineHeight& value)
{
	return Text(value.value) + ":" + Text(value.inherit_type) + ":" + Text(value.inherit_value);
}
String Text(const Style::VerticalAlign& value)
{
	return Text(value.type) + ":" + Text(value.value);
}
String Text(const Style::Clip& value)
{
	return Text(value.GetType()) + ":" + Text(value.GetNumber());
}
String Text(const TransitionList* value)
{
	return (!value || value->none) ? "none" : CreateString("%d transitions", (int)value->transitions.size());
}
String Text(const AnimationList* value)
{
	return (!value || value->empty()) ? "none" : CreateString("%d animations", (int)value->size());
}
String Text(const TransformPtr& value)
{
	return (!value || value->GetNumPrimitives() == 0) ? "none" : "transform";
}

// Every computed value, one per line. (Add a value here when you add one to ComputedValues.)
String Describe(const ComputedValues& values)
{
	String out;
#define RMLUI_FIELD(name) out += #name "=" + Text(values.name()) + "\n"
	// Common
	RMLUI_FIELD(width);
	RMLUI_FIELD(height);
	RMLUI_FIELD(margin_top);
	RMLUI_FIELD(margin_right);
	RMLUI_FIELD(margin_bottom);
	RMLUI_FIELD(margin_left);
	RMLUI_FIELD(padding_top);
	RMLUI_FIELD(padding_right);
	RMLUI_FIELD(padding_bottom);
	RMLUI_FIELD(padding_left);
	RMLUI_FIELD(top);
	RMLUI_FIELD(right);
	RMLUI_FIELD(bottom);
	RMLUI_FIELD(left);
	RMLUI_FIELD(z_index);
	RMLUI_FIELD(border_top_width);
	RMLUI_FIELD(border_right_width);
	RMLUI_FIELD(border_bottom_width);
	RMLUI_FIELD(border_left_width);
	RMLUI_FIELD(box_sizing);
	RMLUI_FIELD(display);
	RMLUI_FIELD(position);
	RMLUI_FIELD(float_);
	RMLUI_FIELD(clear);
	RMLUI_FIELD(overflow_x);
	RMLUI_FIELD(overflow_y);
	RMLUI_FIELD(visibility);
	RMLUI_FIELD(background_color);
	RMLUI_FIELD(border_top_color);
	RMLUI_FIELD(border_right_color);
	RMLUI_FIELD(border_bottom_color);
	RMLUI_FIELD(border_left_color);
	RMLUI_FIELD(has_decorator);
	// Inherited
	RMLUI_FIELD(font_family);
	RMLUI_FIELD(cursor);
	RMLUI_FIELD(font_face_handle);
	RMLUI_FIELD(font_size);
	RMLUI_FIELD(letter_spacing);
	RMLUI_FIELD(has_font_effect);
	RMLUI_FIELD(font_style);
	RMLUI_FIELD(font_weight);
	RMLUI_FIELD(font_kerning);
	RMLUI_FIELD(pointer_events);
	RMLUI_FIELD(focus);
	RMLUI_FIELD(text_align);
	RMLUI_FIELD(text_align_last);
	RMLUI_FIELD(specified_text_align_last);
	RMLUI_FIELD(text_justify);
	RMLUI_FIELD(text_decoration);
	RMLUI_FIELD(text_transform);
	RMLUI_FIELD(white_space);
	RMLUI_FIELD(word_break);
	RMLUI_FIELD(color);
	RMLUI_FIELD(opacity);
	RMLUI_FIELD(line_height);
	RMLUI_FIELD(language);
	RMLUI_FIELD(direction);
	// Rare
	RMLUI_FIELD(min_width);
	RMLUI_FIELD(max_width);
	RMLUI_FIELD(min_height);
	RMLUI_FIELD(max_height);
	RMLUI_FIELD(vertical_align);
	RMLUI_FIELD(animation);
	RMLUI_FIELD(transition);
	RMLUI_FIELD(perspective);
	RMLUI_FIELD(perspective_origin_x);
	RMLUI_FIELD(perspective_origin_y);
	RMLUI_FIELD(transform);
	RMLUI_FIELD(transform_origin_x);
	RMLUI_FIELD(transform_origin_y);
	RMLUI_FIELD(transform_origin_z);
	RMLUI_FIELD(has_local_transform);
	RMLUI_FIELD(has_local_perspective);
	RMLUI_FIELD(align_content);
	RMLUI_FIELD(align_items);
	RMLUI_FIELD(align_self);
	RMLUI_FIELD(flex_direction);
	RMLUI_FIELD(flex_wrap);
	RMLUI_FIELD(justify_content);
	RMLUI_FIELD(flex_grow);
	RMLUI_FIELD(flex_shrink);
	RMLUI_FIELD(flex_basis);
	for (int corner = 0; corner < 4; corner++)
	{
		for (int axis = 0; axis < 2; axis++)
			out += CreateString("border_radius[%d][%d]=", corner, axis) + Text(values.border_radius(corner, axis)) + "\n";
	}
	RMLUI_FIELD(text_overflow);
	if (values.text_overflow() == Style::TextOverflow::String)
		RMLUI_FIELD(text_overflow_string);
	RMLUI_FIELD(clip);
	RMLUI_FIELD(drag);
	RMLUI_FIELD(tab_index);
	RMLUI_FIELD(image_color);
	RMLUI_FIELD(row_gap);
	RMLUI_FIELD(column_gap);
	RMLUI_FIELD(overscroll_behavior);
	RMLUI_FIELD(scrollbar_margin);
	RMLUI_FIELD(has_mask_image);
	RMLUI_FIELD(has_filter);
	RMLUI_FIELD(has_backdrop_filter);
	RMLUI_FIELD(has_box_shadow);
#undef RMLUI_FIELD
	return out;
}

const String initial_values_rml = R"(
<rml>
<head/>
<body><div id="plain"/><div id="explicit"/></body>
</rml>
)";

} // namespace

TEST_CASE("InitialValues.registered_default_equals_default_computed_value")
{
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(initial_values_rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* plain = document->GetElementById("plain");
	Element* explicit_element = document->GetElementById("explicit");
	const String plain_values = Describe(plain->GetComputedValues());
	REQUIRE(Describe(explicit_element->GetComputedValues()) == plain_values);

	int checked = 0;
	for (const PropertyId id : StyleSheetSpecification::GetRegisteredProperties())
	{
		const PropertyDefinition* definition = StyleSheetSpecification::GetProperty(id);
		REQUIRE(definition);
		const String& name = StyleSheetSpecification::GetPropertyName(id);
		INFO(name, ": ", definition->GetDefaultValue()->ToString());

		explicit_element->SetProperty(id, *definition->GetDefaultValue());
		context->Update();
		CHECK(Describe(explicit_element->GetComputedValues()) == plain_values);

		explicit_element->RemoveProperty(id);
		context->Update();
		checked++;
	}
	CHECK(checked == (int)StyleSheetSpecification::GetRegisteredProperties().Size());
	CHECK(checked >= 100);
	CHECK(StyleSheetSpecification::GetRegisteredProperties().Contains(PropertyId::WillChange));

	document->Close();
	TestsShell::ShutdownShell();
}

TEST_CASE("InitialValues.css_initial_values")
{
	using namespace Style;
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	const ComputedValues& defaults = DefaultComputedValues();
	CHECK(defaults.font_size() == 16.f);
	CHECK(defaults.color() == Colourb(0, 0, 0));
	CHECK(defaults.border_top_color() == Colourb(0, 0, 0));
	CHECK(defaults.border_left_color() == Colourb(0, 0, 0));
	CHECK(defaults.line_height().value == doctest::Approx(16.f * 1.2f));
	CHECK(defaults.min_width().type == LengthPercentageAuto::Auto);
	CHECK(defaults.min_height().type == LengthPercentageAuto::Auto);
	CHECK(defaults.visibility() == Visibility::Visible);
	CHECK(defaults.opacity() == 1.f);

	auto registered = [](PropertyId id) {
		const PropertyDefinition* definition = StyleSheetSpecification::GetProperty(id);
		REQUIRE(definition);
		return definition;
	};
	CHECK(registered(PropertyId::FontSize)->GetDefaultValue()->ToString() == "16px");
	CHECK(registered(PropertyId::Color)->GetDefaultValue()->Get<Colourb>() == Colourb(0, 0, 0));
	CHECK(registered(PropertyId::MinWidth)->GetDefaultValue()->ToString() == "auto");
	CHECK(registered(PropertyId::MinHeight)->GetDefaultValue()->ToString() == "auto");
	CHECK(registered(PropertyId::Opacity)->IsInherited() == false);
	CHECK(registered(PropertyId::Visibility)->IsInherited() == true);
	TestsShell::ShutdownShell();
}

TEST_CASE("InitialValues.behaviour")
{
	static const String rml = R"(
<rml>
<head>
	<style>
		div { display: block; }
		#parent { color: #f00; opacity: 0.5; visibility: hidden; }
		#bordered { border-width: 1px; border-left-color: #00f; }
		#nested { opacity: 0.5; }
		#shown { visibility: visible; }
		#min { display: block; min-width: auto; min-height: 10px; }
	</style>
</head>
<body>
<div id="parent"><div id="bordered"><div id="nested"/></div><div id="shown"/></div>
<div id="min"/>
</body>
</rml>
)";
	Context* context = TestsShell::GetContext();
	REQUIRE(context);
	ElementDocument* document = context->LoadDocumentFromMemory(rml);
	REQUIRE(document);
	document->Show();
	context->Update();

	Element* parent = document->GetElementById("parent");
	Element* bordered = document->GetElementById("bordered");
	Element* nested = document->GetElementById("nested");
	Element* shown = document->GetElementById("shown");

	// border colors are 'currentcolor' unless declared, and follow a change of the color
	const ComputedValues& border = bordered->GetComputedValues();
	CHECK(border.color() == Colourb(255, 0, 0));
	CHECK(border.border_top_color() == Colourb(255, 0, 0));
	CHECK(border.border_right_color() == Colourb(255, 0, 0));
	CHECK(border.border_bottom_color() == Colourb(255, 0, 0));
	CHECK(border.border_left_color() == Colourb(0, 0, 255));
	parent->SetProperty("color", "#0f0");
	context->Update();
	CHECK(bordered->GetComputedValues().border_top_color() == Colourb(0, 255, 0));
	CHECK(bordered->GetComputedValues().border_left_color() == Colourb(0, 0, 255));
	// an invalid substitution makes the declaration unset: currentcolor again
	bordered->SetProperty("border-left-color", "var(--undefined)");
	TestsShell::SetNumExpectedWarnings(1);
	context->Update();
	CHECK(bordered->GetComputedValues().border_left_color() == Colourb(0, 255, 0));
	bordered->RemoveProperty("border-left-color");
	context->Update();
	TestsShell::SetNumExpectedWarnings(0);

	// opacity is not inherited: the computed value is the element's own (the ancestors' apply as groups, phase 3 p3-paint:
	// GroupOpacity.cpp)
	CHECK(bordered->GetProperty(PropertyId::Opacity)->Get<float>() == 1.f);
	CHECK(parent->GetComputedValues().opacity() == doctest::Approx(0.5f));
	CHECK(bordered->GetComputedValues().opacity() == doctest::Approx(1.f));
	CHECK(nested->GetComputedValues().opacity() == doctest::Approx(0.5f));
	parent->SetProperty("opacity", "0.8");
	context->Update();
	CHECK(parent->GetComputedValues().opacity() == doctest::Approx(0.8f));
	CHECK(bordered->GetComputedValues().opacity() == doctest::Approx(1.f));
	CHECK(nested->GetComputedValues().opacity() == doctest::Approx(0.5f));

	// visibility is inherited, and a child can set it back
	CHECK(bordered->GetComputedValues().visibility() == Style::Visibility::Hidden);
	CHECK(nested->GetComputedValues().visibility() == Style::Visibility::Hidden);
	CHECK(shown->GetComputedValues().visibility() == Style::Visibility::Visible);
	parent->SetProperty("visibility", "visible");
	context->Update();
	CHECK(nested->GetComputedValues().visibility() == Style::Visibility::Visible);

	// min-width: auto is 0 outside flex and grid items
	Element* min = document->GetElementById("min");
	CHECK(min->GetComputedValues().min_width().type == Style::LengthPercentageAuto::Auto);
	CHECK(min->GetComputedValues().min_height().type == Style::LengthPercentageAuto::Length);
	CHECK(min->GetBox().GetSize().y == doctest::Approx(10.f));

	document->Close();
	TestsShell::ShutdownShell();
}
