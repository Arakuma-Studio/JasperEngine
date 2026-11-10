#include "ElementAnimation.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Decorator.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Filter.h"
#include "../../Include/RmlUi/Core/PropertyDefinition.h"
#include "../../Include/RmlUi/Core/PropertySpecification.h"
#include "../../Include/RmlUi/Core/StyleSheet.h"
#include "../../Include/RmlUi/Core/StyleSheetSpecification.h"
#include "../../Include/RmlUi/Core/StyleSheetTypes.h"
#include "../../Include/RmlUi/Core/StyleValue.h"
#include "../../Include/RmlUi/Core/Transform.h"
#include "../../Include/RmlUi/Core/TransformPrimitive.h"
#include "ComputeProperty.h"
#include "ElementStyle.h"
#include "ElementTransitions.h"
#include "PropertyParserColour.h"
#include "TextShadow.h"
#include "TransformUtilities.h"
#include <cmath>
#include <limits>
#ifdef RMLUI_MATH_EXPRESSIONS
	#include "CalculationResolver.h"
#endif

namespace Rml {

static Property InterpolateProperties(const Property& p0, const Property& p1, float alpha, Element& element, const PropertyDefinition* definition);
static NumericValue InterpolateNumericValue(NumericValue v0, NumericValue v1, float alpha, Element& element, const PropertyDefinition* definition);

template <typename T>
static T Mix(const T& v0, const T& v1, float alpha)
{
	return v0 * (1.0f - alpha) + v1 * alpha;
}

// Colours interpolate in premultiplied sRGB (CSS Color 4 for legacy colours): a transparent end does not darken the other.
// Outputs outside [0, 1] (overshooting easings) extrapolate, and the result is clamped.
static ColourbPremultiplied InterpolatePremultipliedColour(ColourbPremultiplied c0, ColourbPremultiplied c1, float alpha)
{
	auto channel = [alpha](byte a, byte b) { return (byte)Math::Clamp(Math::Round(Mix(float(a), float(b), alpha)), 0.0f, 255.f); };
	ColourbPremultiplied result;
	result.red = channel(c0.red, c1.red);
	result.green = channel(c0.green, c1.green);
	result.blue = channel(c0.blue, c1.blue);
	result.alpha = channel(c0.alpha, c1.alpha);
	// Premultiplied channels never exceed the alpha.
	result.red = Math::Min(result.red, result.alpha);
	result.green = Math::Min(result.green, result.alpha);
	result.blue = Math::Min(result.blue, result.alpha);
	return result;
}

static Colourb InterpolateColour(Colourb c0, Colourb c1, float alpha)
{
	if (c0.alpha == 255 && c1.alpha == 255)
	{
		auto channel = [alpha](byte a, byte b) { return (byte)Math::Clamp(Math::Round(Mix(float(a), float(b), alpha)), 0.0f, 255.f); };
		return Colourb(channel(c0.red, c1.red), channel(c0.green, c1.green), channel(c0.blue, c1.blue), 255);
	}
	return InterpolatePremultipliedColour(c0.ToPremultiplied(), c1.ToPremultiplied(), alpha).ToNonPremultiplied();
}

// 'currentcolor' in an interpolation is the element's colour at this frame, in 'color' itself its parent's (CSS Color 4:
// the interpolated value is a mix with currentcolor, which follows the colour while the transition or animation runs).
static Colourb CurrentColorOf(Element& element, const PropertyDefinition* definition)
{
	if (definition && definition->GetId() == PropertyId::Color)
	{
		Element* parent = element.GetParentNode();
		return parent ? parent->GetComputedValues().color() : DefaultComputedValues().color();
	}
	return element.GetComputedValues().color();
}

// Box shadows interpolate pairwise, the shorter list padded with transparent zero shadows (CSS Backgrounds 3 §7.2); a
// pair with one shadow inset and the other not makes the lists discrete.
static bool InterpolateBoxShadows(const BoxShadowList& list0, const BoxShadowList& list1, float alpha, Element& element, BoxShadowList& result)
{
	const size_t count = Math::Max(list0.size(), list1.size());
	const Colourb element_colour = element.GetComputedValues().color();
	auto colour_of = [&](const BoxShadow& s) {
		return s.current_color ? PropertyParserColour::ResolveCurrentColor(s.color_mix, element_colour).ToPremultiplied() : s.color;
	};
	result.clear();
	result.reserve(count);
	for (size_t i = 0; i < count; i++)
	{
		BoxShadow transparent;
		transparent.color = ColourbPremultiplied(0, 0, 0, 0);
		BoxShadow s0 = (i < list0.size() ? list0[i] : transparent);
		BoxShadow s1 = (i < list1.size() ? list1[i] : transparent);
		if (i >= list0.size())
			s0.inset = s1.inset;
		if (i >= list1.size())
			s1.inset = s0.inset;
		if (s0.inset != s1.inset)
			return false;
#ifdef RMLUI_MATH_EXPRESSIONS
		// Shadow lengths take no percentage: a calc() is a length in pixels at this frame, as its computed value.
		auto resolve = [&](NumericValue& value, CalculationPtr& calculation) {
			if (!calculation)
				return true;
			float pixels = 0.f;
			if (!ResolveElementCalculation(*calculation, element, 0.f, pixels))
				return false;
			value = NumericValue(pixels, Unit::PX);
			calculation = nullptr;
			return true;
		};
		for (BoxShadow* s : {&s0, &s1})
		{
			if (!resolve(s->offset_x, s->offset_x_calculation) || !resolve(s->offset_y, s->offset_y_calculation) ||
				!resolve(s->blur_radius, s->blur_radius_calculation) || !resolve(s->spread_distance, s->spread_distance_calculation))
				return false;
		}
#endif
		auto length = [&](NumericValue v0, NumericValue v1) {
			if (v0.unit == Unit::UNKNOWN)
				v0 = NumericValue(0.f, Unit::PX);
			if (v1.unit == Unit::UNKNOWN)
				v1 = NumericValue(0.f, Unit::PX);
			if (v0.number == 0.f && v0.unit != v1.unit)
				v0.unit = v1.unit;
			if (v1.number == 0.f && v1.unit != v0.unit)
				v1.unit = v0.unit;
			return InterpolateNumericValue(v0, v1, alpha, element, nullptr);
		};
		BoxShadow shadow;
		shadow.inset = s0.inset;
		if (s0.current_color && s1.current_color && s0.color_mix == s1.color_mix)
		{
			shadow.current_color = true;
			shadow.color_mix = s0.color_mix;
		}
		else
			shadow.color = InterpolatePremultipliedColour(colour_of(s0), colour_of(s1), alpha);
		shadow.offset_x = length(s0.offset_x, s1.offset_x);
		shadow.offset_y = length(s0.offset_y, s1.offset_y);
		shadow.blur_radius = length(s0.blur_radius, s1.blur_radius);
		shadow.blur_radius.number = Math::Max(shadow.blur_radius.number, 0.f);
		shadow.spread_distance = length(s0.spread_distance, s1.spread_distance);
		result.push_back(shadow);
	}
	return true;
}

// Merges all the primitives to a single DecomposedMatrix4 primitive
static bool CombineAndDecompose(Transform& t, Element& e)
{
	Matrix4f m = Matrix4f::Identity();

	for (TransformPrimitive& primitive : t.GetPrimitives())
	{
		Matrix4f m_primitive = TransformUtilities::ResolveTransform(primitive, e);
		m *= m_primitive;
	}

	Transforms::DecomposedMatrix4 decomposed;

	if (!TransformUtilities::Decompose(decomposed, m))
		return false;

	t.ClearPrimitives();
	t.AddPrimitive(decomposed);

	return true;
}

static bool PrepareSingleTransform(Transform& transform, Element& element)
{
	bool must_decompose = false;
	for (TransformPrimitive& primitive : transform.GetPrimitives())
	{
		if (!TransformUtilities::PrepareForInterpolation(primitive, element))
		{
			must_decompose = true;
			break;
		}
	}
	return !must_decompose || CombineAndDecompose(transform, element);
}

enum class PrepareTransformResult { Unchanged = 0, ChangedT0 = 1, ChangedT1 = 2, ChangedT0andT1 = 3, Invalid = 4 };
static PrepareTransformResult PrepareTransformPair(Transform& t0, Transform& t1, Element& element);

/**
    An abstraction for decorator and filter declarations.
 */
struct EffectDeclarationView {
	EffectDeclarationView() = default;
	EffectDeclarationView(const DecoratorDeclaration& declaration) :
		instancer(declaration.instancer), type(&declaration.type), properties(&declaration.properties), paint_area(declaration.paint_area)
	{}
	EffectDeclarationView(const NamedDecorator* named_decorator) :
		instancer(named_decorator->instancer), type(&named_decorator->type), properties(&named_decorator->properties)
	{}
	EffectDeclarationView(const FilterDeclaration& declaration) :
		instancer(declaration.instancer), type(&declaration.type), properties(&declaration.properties)
	{}

	EffectSpecification* instancer = nullptr;
	const String* type = nullptr;
	const PropertyDictionary* properties = nullptr;
	BoxArea paint_area = BoxArea::Auto;

	explicit operator bool() const { return instancer != nullptr; }
};

// Interpolate two effect declarations. One of them can be empty, in which case the empty one is replaced by default values.
static bool InterpolateEffectProperties(PropertyDictionary& properties, const EffectDeclarationView& d0, const EffectDeclarationView& d1, float alpha,
	Element& element)
{
	if (d0 && d1)
	{
		// Both declarations are specified, check if they are compatible for interpolation.
		if (!d0.instancer || d0.instancer != d1.instancer || *d0.type != *d1.type ||
			d0.properties->GetNumProperties() != d1.properties->GetNumProperties() || d0.paint_area != d1.paint_area)
			return false;

		const auto& properties0 = d0.properties->GetProperties();
		const auto& properties1 = d1.properties->GetProperties();

		for (const auto& pair0 : properties0)
		{
			const PropertyId id = pair0.first;
			const Property& prop0 = pair0.second;

			auto it = properties1.find(id);
			if (it == properties1.end())
			{
				RMLUI_ERRORMSG("Incompatible decorator properties.");
				return false;
			}
			const Property& prop1 = it->second;

			Property p;
			if (prop0 == prop1)
				p = prop0;
			else if (prop0.unit == Unit::STYLE_VALUE && prop1.unit == Unit::STYLE_VALUE &&
				!PropertyParserColour::DependsOnCurrentColor(prop0) && !PropertyParserColour::DependsOnCurrentColor(prop1))
			{
				const StyleValuePtr value0 = prop0.Get<StyleValuePtr>(), value1 = prop1.Get<StyleValuePtr>();
				const StyleValuePtr mixed = value0 && value1 ? value0->Interpolate(*value1, alpha, element) : nullptr;
				// An incompatible typed argument makes the whole decorator pair discrete.
				if (!mixed)
					return false;
				p = Property(mixed, Unit::STYLE_VALUE);
			}
			else
				p = InterpolateProperties(prop0, prop1, alpha, element, prop0.definition);
			p.definition = prop0.definition;
			properties.SetProperty(id, p);
		}
		return true;
	}
	else if ((d0 && !d1) || (!d0 && d1))
	{
		// One of the declarations is empty, interpolate against the default values of its type.
		const auto& d_filled = (d0 ? d0 : d1);

		const PropertySpecification& specification = d_filled.instancer->GetPropertySpecification();
		const PropertyMap& properties_filled = d_filled.properties->GetProperties();

		for (const auto& pair_filled : properties_filled)
		{
			const PropertyId id = pair_filled.first;
			const PropertyDefinition* underlying_definition = specification.GetProperty(id);
			if (!underlying_definition)
				return false;

			const Property& p_filled = pair_filled.second;
			const Property& p_default = *underlying_definition->GetDefaultValue();
			const Property& p_interp0 = (d0 ? p_filled : p_default);
			const Property& p_interp1 = (d1 ? p_filled : p_default);

			Property p = InterpolateProperties(p_interp0, p_interp1, alpha, element, p_filled.definition);
			p.definition = p_filled.definition;
			properties.SetProperty(id, p);
		}
		return true;
	}

	return false;
}

static NumericValue InterpolateNumericValue(NumericValue v0, NumericValue v1, float alpha, Element& element, const PropertyDefinition* definition)
{
	// If we have the same units, we can simply interpolate regardless of what the value represents.
	if (v0.unit == v1.unit)
		return NumericValue{Mix(v0.number, v1.number, alpha), v0.unit};

	// When mixing lengths or relative sizes, resolve them to pixel lengths and interpolate. This only works if we have a definition.
	if (Any(v0.unit & Unit::NUMBER_LENGTH_PERCENT) && Any(v1.unit & Unit::NUMBER_LENGTH_PERCENT) && definition)
	{
		float f0 = element.GetStyle()->ResolveRelativeLength(v0, definition->GetRelativeTarget());
		float f1 = element.GetStyle()->ResolveRelativeLength(v1, definition->GetRelativeTarget());
		return NumericValue{Mix(f0, f1, alpha), Unit::PX};
	}

	// As long as we don't mix lengths and percentages, we can still resolve lengths without a definition.
	if (Any(v0.unit & Unit::LENGTH) && Any(v1.unit & Unit::LENGTH))
	{
		float f0 = element.ResolveLength(v0);
		float f1 = element.ResolveLength(v1);
		return NumericValue{Mix(f0, f1, alpha), Unit::PX};
	}

	if (Any(v0.unit & Unit::ANGLE) && Any(v1.unit & Unit::ANGLE))
	{
		float f = Mix(ComputeAngle(v0), ComputeAngle(v1), alpha);
		return NumericValue{f, Unit::RAD};
	}

	// Fall back to discrete interpolation for incompatible units.
	return alpha < 0.5f ? v0 : v1;
}

#ifdef RMLUI_MATH_EXPRESSIONS
static bool ResolveCalculatedAnimationEndpoint(const Property& property, Element& element, const PropertyDefinition* definition, NumericValue& result)
{
	if (Any(property.unit & Unit::NUMERIC))
	{
		result = property.GetNumericValue();
		return true;
	}
	if (property.unit != Unit::CALCULATION)
		return false;

	const CalculationPtr calculation = property.value.Get<CalculationPtr>();
	if (!calculation || !calculation->GetRoot())
		return false;

	RelativeTarget relative_target = (definition ? definition->GetRelativeTarget() : RelativeTarget::None);
	float percentage_basis = 1.f;
	switch (relative_target)
	{
	case RelativeTarget::ContainingBlockWidth: percentage_basis = element.GetContainingBlock().x; break;
	case RelativeTarget::ContainingBlockHeight: percentage_basis = element.GetContainingBlock().y; break;
	case RelativeTarget::FontSize: percentage_basis = element.GetComputedValues().font_size(); break;
	case RelativeTarget::ParentFontSize:
		if (Element* parent = element.GetParentNode())
			percentage_basis = parent->GetComputedValues().font_size();
		else
			percentage_basis = DefaultComputedValues().font_size();
		break;
	case RelativeTarget::LineHeight: percentage_basis = element.GetLineHeight(); break;
	case RelativeTarget::None: break;
	}

	float value = 0.f;
	if (!ResolveElementCalculation(*calculation, element, percentage_basis, value, relative_target))
		return false;

	const CalculationNumericType& type = calculation->GetType();
	auto is_dimension = [&](CalculationDimension dimension) {
		for (size_t i = 0; i < type.exponents.size(); i++)
		{
			const int expected = (i == size_t(dimension) ? 1 : 0);
			if (type.exponents[i] != expected)
				return false;
		}
		return true;
	};
	auto is_number = [&]() {
		for (int exponent : type.exponents)
			if (exponent != 0)
				return false;
		return true;
	};

	Unit unit = Unit::UNKNOWN;
	if (is_number() || is_dimension(CalculationDimension::Percent))
		unit = Unit::NUMBER;
	else if (is_dimension(CalculationDimension::Length))
		unit = Unit::PX;
	else if (is_dimension(CalculationDimension::Angle))
		unit = Unit::RAD;
	else if (is_dimension(CalculationDimension::Resolution))
		unit = Unit::X;
	else
		return false;

	result = NumericValue(value, unit);
	return true;
}

static bool CalculationContainsUnit(const Calculation::Node& node, Unit unit)
{
	if (node.kind == Calculation::Kind::Value && node.unit == unit)
		return true;
	for (const auto& child : node.children)
		if (child && CalculationContainsUnit(*child, unit))
			return true;
	return false;
}

static SharedPtr<const Calculation::Node> MakeCalculationEndpointNode(const Property& property, const CalculationNumericType& target_type)
{
	if (property.unit == Unit::CALCULATION)
	{
		const CalculationPtr calculation = property.value.Get<CalculationPtr>();
		if (!calculation || !calculation->GetRoot() || calculation->GetType() != target_type)
			return nullptr;
		return calculation->GetRoot();
	}

	if (!Any(property.unit & Unit::NUMERIC))
		return nullptr;
	auto node = MakeShared<Calculation::Node>();
	node->value = property.Get<float>();
	node->unit = property.unit;
	node->type = target_type;
	return node;
}

// A length and a percentage of a basis the animation does not know (phase 3, p3-paint: radii of the border box,
// object-position's free space): their mix is the calc() of a length.
static bool IsLengthPercentageMix(const Property& p0, const Property& p1, const PropertyDefinition* definition)
{
	if (definition && definition->GetRelativeTarget() != RelativeTarget::None)
		return false;
	const bool length0 = Any(p0.unit & Unit::LENGTH), length1 = Any(p1.unit & Unit::LENGTH);
	return (length0 && p1.unit == Unit::PERCENT) || (p0.unit == Unit::PERCENT && length1);
}

static CalculationPtr InterpolateCalculations(const Property& p0, const Property& p1, float alpha)
{
	const CalculationPtr c0 = (p0.unit == Unit::CALCULATION ? p0.value.Get<CalculationPtr>() : nullptr);
	const CalculationPtr c1 = (p1.unit == Unit::CALCULATION ? p1.value.Get<CalculationPtr>() : nullptr);
	CalculationNumericType length_type;
	length_type.exponents[size_t(CalculationDimension::Length)] = 1;
	const CalculationNumericType* target_type =
		c0 && c0->GetRoot() ? &c0->GetType() : (c1 && c1->GetRoot() ? &c1->GetType() : (IsLengthPercentageMix(p0, p1, nullptr) ? &length_type : nullptr));
	if (!target_type)
		return nullptr;

	const auto endpoint0 = MakeCalculationEndpointNode(p0, *target_type);
	const auto endpoint1 = MakeCalculationEndpointNode(p1, *target_type);
	if (!endpoint0 || !endpoint1)
		return nullptr;

	auto make_weighted = [&](float weight, SharedPtr<const Calculation::Node> endpoint) -> SharedPtr<const Calculation::Node> {
		auto weight_node = MakeShared<Calculation::Node>();
		weight_node->value = weight;
		weight_node->unit = Unit::NUMBER;

		auto product = MakeShared<Calculation::Node>();
		product->kind = Calculation::Kind::Product;
		product->type = *target_type;
		product->children = {std::move(weight_node), std::move(endpoint)};
		return product;
	};

	auto sum = MakeShared<Calculation::Node>();
	sum->kind = Calculation::Kind::Sum;
	sum->type = *target_type;
	sum->children = {make_weighted(1.f - alpha, endpoint0), make_weighted(alpha, endpoint1)};
	return MakeShared<Calculation>(std::move(sum));
}

static bool ShouldDeferCalculatedPercentageInterpolation(const Property& p0, const Property& p1, const PropertyDefinition* definition)
{
	if (definition && definition->GetRelativeTarget() != RelativeTarget::None)
		return false;

	const CalculationPtr calculations[] = {
		p0.unit == Unit::CALCULATION ? p0.value.Get<CalculationPtr>() : nullptr,
		p1.unit == Unit::CALCULATION ? p1.value.Get<CalculationPtr>() : nullptr,
	};
	if (!calculations[0] && !calculations[1])
		return false;
	bool has_length_calculation = false;
	for (const CalculationPtr& calculation : calculations)
	{
		if (!calculation || !calculation->GetRoot())
			continue;
		const CalculationNumericType& type = calculation->GetType();
		bool is_length = true;
		for (size_t i = 0; i < type.exponents.size(); ++i)
			is_length &= type.exponents[i] == (i == size_t(CalculationDimension::Length) ? 1 : 0);
		if (!is_length)
			continue;
		has_length_calculation = true;
		if (CalculationContainsUnit(*calculation->GetRoot(), Unit::PERCENT))
			return true;
	}
	return has_length_calculation && (p0.unit == Unit::PERCENT || p1.unit == Unit::PERCENT);
}
#endif

static Property InterpolateProperties(const Property& p0, const Property& p1, float alpha, Element& element, const PropertyDefinition* definition)
{
	const Property& p_discrete = (alpha < 0.5f ? p0 : p1);

	// (discrete, fork P6-L1: see IsInterpolable)
	if (definition && definition->GetId() == PropertyId::WebkitLineClamp)
		return p_discrete;

#ifdef RMLUI_MATH_EXPRESSIONS
	if ((Any(p0.unit & Unit::NUMERIC) || p0.unit == Unit::CALCULATION) && (Any(p1.unit & Unit::NUMERIC) || p1.unit == Unit::CALCULATION))
	{
		if (ShouldDeferCalculatedPercentageInterpolation(p0, p1, definition) || IsLengthPercentageMix(p0, p1, definition))
		{
			if (CalculationPtr calculation = InterpolateCalculations(p0, p1, alpha))
				return Property{std::move(calculation), Unit::CALCULATION};
			return p_discrete;
		}
		NumericValue v0, v1;
		if (ResolveCalculatedAnimationEndpoint(p0, element, definition, v0) && ResolveCalculatedAnimationEndpoint(p1, element, definition, v1))
		{
			NumericValue v = InterpolateNumericValue(v0, v1, alpha, element, definition);
			return Property{v.number, v.unit};
		}
		return p_discrete;
	}
#else
	if (Any(p0.unit & Unit::NUMERIC) && Any(p1.unit & Unit::NUMERIC))
	{
		NumericValue v = InterpolateNumericValue(p0.GetNumericValue(), p1.GetNumericValue(), alpha, element, definition);
		return Property{v.number, v.unit};
	}
#endif

	// 'currentcolor' or a color-mix() of it against a colour or another one.
	const bool current0 = PropertyParserColour::DependsOnCurrentColor(p0), current1 = PropertyParserColour::DependsOnCurrentColor(p1);
	if ((current0 || current1) && (current0 || p0.unit == Unit::COLOUR) && (current1 || p1.unit == Unit::COLOUR) && !(p0 == p1))
	{
		const Colourb current = CurrentColorOf(element, definition);
		const Colourb c = InterpolateColour(PropertyParserColour::Resolve(p0, current), PropertyParserColour::Resolve(p1, current), alpha);
		return Property{c, Unit::COLOUR};
	}

	if (p0.unit == Unit::KEYWORD && p1.unit == Unit::KEYWORD)
	{
		// Discrete interpolation, swap at alpha = 0.5.
		// Special case for the 'visibility' and 'display' properties as in the CSS specs:
		//   If present, apply the 'visible' / non-'none' keyword during the entire transition period, i.e. alpha (0,1).
		// See: https://www.w3.org/TR/css-display-4/#display-animation
		if (definition && definition->GetId() == PropertyId::Visibility)
		{
			if (p0.Get<int>() == (int)Style::Visibility::Visible)
				return alpha < 1.f ? p0 : p1;
			else if (p1.Get<int>() == (int)Style::Visibility::Visible)
				return alpha <= 0.f ? p0 : p1;
		}
		if (definition && definition->GetId() == PropertyId::Display)
		{
			if (p0.Get<int>() == (int)Style::Display::None)
				return alpha <= 0.f ? p0 : p1;
			else if (p1.Get<int>() == (int)Style::Display::None)
				return alpha < 1.f ? p0 : p1;
		}

		return p_discrete;
	}

	if (p0.unit == Unit::COLOUR && p1.unit == Unit::COLOUR)
	{
		Colourb c = InterpolateColour(p0.value.Get<Colourb>(), p1.value.Get<Colourb>(), alpha);
		return Property{c, Unit::COLOUR};
	}

	if (p0.unit == Unit::TRANSFORM && p1.unit == Unit::TRANSFORM)
	{
		auto& t0 = p0.value.GetReference<TransformPtr>();
		auto& t1 = p1.value.GetReference<TransformPtr>();

#ifdef RMLUI_MATH_EXPRESSIONS
		if (t0 && t1 && (t0->HasCalculations() || t1->HasCalculations()))
		{
			auto resolve_transform = [&](const Transform& source) {
				Transform result;
				for (int i = 0; i < source.GetNumPrimitives(); i++)
					result.AddPrimitive(source.HasCalculations() ? source.ResolvePrimitive(i, element) : source.GetPrimitive(i));
				return result;
			};

			Transform resolved0 = resolve_transform(*t0);
			Transform resolved1 = resolve_transform(*t1);
			if (!PrepareSingleTransform(resolved0, element) || !PrepareSingleTransform(resolved1, element) ||
				PrepareTransformPair(resolved0, resolved1, element) == PrepareTransformResult::Invalid)
				return p_discrete;

			const auto& resolved_prims0 = resolved0.GetPrimitives();
			const auto& resolved_prims1 = resolved1.GetPrimitives();
			if (resolved_prims0.size() != resolved_prims1.size())
				return p_discrete;

			auto interpolated = MakeShared<Transform>();
			for (size_t i = 0; i < resolved_prims0.size(); i++)
			{
				TransformPrimitive primitive = resolved_prims0[i];
				if (!TransformUtilities::InterpolateWith(primitive, resolved_prims1[i], alpha))
					return p_discrete;
				interpolated->AddPrimitive(primitive);
			}
			return Property{TransformPtr(std::move(interpolated)), Unit::TRANSFORM};
		}
#endif

		const auto& prim0 = t0->GetPrimitives();
		const auto& prim1 = t1->GetPrimitives();

		if (prim0.size() != prim1.size())
		{
			RMLUI_ERRORMSG("Transform primitives not of same size during interpolation. Were the transforms properly prepared for interpolation?");
			return Property{t0, Unit::TRANSFORM};
		}

		// Build the new, interpolating transform
		UniquePtr<Transform> t(new Transform);
		t->GetPrimitives().reserve(t0->GetPrimitives().size());

		for (size_t i = 0; i < prim0.size(); i++)
		{
			TransformPrimitive p = prim0[i];
			if (!TransformUtilities::InterpolateWith(p, prim1[i], alpha))
			{
				RMLUI_ERRORMSG("Transform primitives can not be interpolated. Were the transforms properly prepared for interpolation?");
				return Property{t0, Unit::TRANSFORM};
			}
			t->AddPrimitive(p);
		}

		return Property{TransformPtr(std::move(t)), Unit::TRANSFORM};
	}

	if (p0.unit == Unit::DECORATOR && p1.unit == Unit::DECORATOR)
	{
		auto GetEffectDeclarationView = [](const Vector<DecoratorDeclaration>& declarations, size_t i, Element& element) -> EffectDeclarationView {
			if (i >= declarations.size())
				return EffectDeclarationView();

			const DecoratorDeclaration& declaration = declarations[i];
			if (declaration.instancer)
				return EffectDeclarationView(declaration);

			// If we don't have a decorator instancer, then this should be a named @decorator, look for one now.
			const StyleSheet* style_sheet = element.GetStyleSheet();
			if (!style_sheet)
				return EffectDeclarationView();

			const NamedDecorator* named_decorator = style_sheet->GetNamedDecorator(declaration.type);
			if (!named_decorator)
			{
				Log::Message(Log::LT_WARNING, "Could not find a named @decorator '%s'.", declaration.type.c_str());
				return EffectDeclarationView();
			}

			return EffectDeclarationView(named_decorator);
		};

		auto& ptr0 = p0.value.GetReference<DecoratorsPtr>();
		auto& ptr1 = p1.value.GetReference<DecoratorsPtr>();
		if (!ptr0 || !ptr1)
		{
			RMLUI_ERRORMSG("Invalid decorator pointer, were the decorator keys properly prepared?");
			return p_discrete;
		}

		// Build the new, interpolated decorator list.
		const bool p0_bigger = ptr0->list.size() > ptr1->list.size();
		auto& big_list = (p0_bigger ? ptr0->list : ptr1->list);
		auto decorator = MakeUnique<DecoratorDeclarationList>();
		auto& list = decorator->list;
		list.reserve(big_list.size());

		for (size_t i = 0; i < big_list.size(); i++)
		{
			EffectDeclarationView d0 = GetEffectDeclarationView(ptr0->list, i, element);
			EffectDeclarationView d1 = GetEffectDeclarationView(ptr1->list, i, element);

			const EffectDeclarationView& declaration = (p0_bigger ? d0 : d1);
			list.push_back(DecoratorDeclaration{*declaration.type, static_cast<DecoratorInstancer*>(declaration.instancer), PropertyDictionary(),
				declaration.paint_area});

			if (!InterpolateEffectProperties(list.back().properties, d0, d1, alpha, element))
				return p_discrete;
		}

		return Property{DecoratorsPtr(std::move(decorator)), Unit::DECORATOR};
	}

	if (p0.unit == Unit::FILTER && p1.unit == Unit::FILTER)
	{
		auto GetEffectDeclarationView = [](const Vector<FilterDeclaration>& declarations, size_t i) -> EffectDeclarationView {
			if (i >= declarations.size())
				return EffectDeclarationView();
			return EffectDeclarationView(declarations[i]);
		};

		auto& ptr0 = p0.value.GetReference<FiltersPtr>();
		auto& ptr1 = p1.value.GetReference<FiltersPtr>();
		if (!ptr0 || !ptr1)
		{
			RMLUI_ERRORMSG("Invalid filter pointer, were the filter keys properly prepared?");
			return p_discrete;
		}

		// Build the new, interpolated filter list.
		const bool p0_bigger = ptr0->list.size() > ptr1->list.size();
		auto& big_list = (p0_bigger ? ptr0->list : ptr1->list);
		auto filter = MakeUnique<FilterDeclarationList>();
		auto& list = filter->list;
		list.reserve(big_list.size());

		for (size_t i = 0; i < big_list.size(); i++)
		{
			EffectDeclarationView d0 = GetEffectDeclarationView(ptr0->list, i);
			EffectDeclarationView d1 = GetEffectDeclarationView(ptr1->list, i);

			const EffectDeclarationView& declaration = (p0_bigger ? d0 : d1);
			list.push_back(FilterDeclaration{*declaration.type, static_cast<FilterInstancer*>(declaration.instancer), PropertyDictionary()});

			if (!InterpolateEffectProperties(list.back().properties, d0, d1, alpha, element))
				return p_discrete;
		}

		return Property{FiltersPtr(std::move(filter)), Unit::FILTER};
	}

	if (p0.unit == Unit::COLORSTOPLIST && p1.unit == Unit::COLORSTOPLIST)
	{
		RMLUI_ASSERT(p0.value.GetType() == Variant::COLORSTOPLIST && p1.value.GetType() == Variant::COLORSTOPLIST);
		const auto& c0 = p0.value.GetReference<ColorStopList>();
		const auto& c1 = p1.value.GetReference<ColorStopList>();

		if (c0.size() != c1.size())
			return p_discrete;

		const size_t N = c0.size();
		ColorStopList result(N);
		const Colourb element_colour = element.GetComputedValues().color();
		auto colour_of = [&](const ColorStop& stop) {
			return stop.current_color ? PropertyParserColour::ResolveCurrentColor(stop.color_mix, element_colour) : stop.color.ToNonPremultiplied();
		};

		for (size_t i = 0; i < N; i++)
		{
			if (c0[i].current_color && c1[i].current_color && c0[i].color_mix == c1[i].color_mix)
			{
				result[i].current_color = true;
				result[i].color_mix = c0[i].color_mix;
			}
			else
				result[i].color = InterpolateColour(colour_of(c0[i]), colour_of(c1[i]), alpha).ToPremultiplied();

#ifdef RMLUI_MATH_EXPRESSIONS
			if (c0[i].position_calculation || c1[i].position_calculation)
			{
				const Property position0 = c0[i].position_calculation ? Property(c0[i].position_calculation, Unit::CALCULATION)
																	  : Property(c0[i].position.number, c0[i].position.unit);
				const Property position1 = c1[i].position_calculation ? Property(c1[i].position_calculation, Unit::CALCULATION)
																	  : Property(c1[i].position.number, c1[i].position.unit);
				result[i].position_calculation = InterpolateCalculations(position0, position1, alpha);
				if (!result[i].position_calculation)
					return p_discrete;
				result[i].position = c0[i].position_calculation ? c0[i].position : c1[i].position;
				continue;
			}
#endif

			// We don't provide the property definition in the following, because it doesn't actually represent how
			// percentages are resolved for stop positions. Here, we don't trivially know how they are resolved, so if
			// users try to mix lengths and percentages, we instead fall back to discrete interpolation. See the
			// gradient decorators for how stop positions are resolved.
			result[i].position = InterpolateNumericValue(c0[i].position, c1[i].position, alpha, element, nullptr);
		}

		return Property{std::move(result), Unit::COLORSTOPLIST};
	}

	if (p0.unit == Unit::BOXSHADOWLIST && p1.unit == Unit::BOXSHADOWLIST)
	{
		BoxShadowList result;
		if (!InterpolateBoxShadows(p0.value.GetReference<BoxShadowList>(), p1.value.GetReference<BoxShadowList>(), alpha, element, result))
			return p_discrete;
		return Property{std::move(result), Unit::BOXSHADOWLIST};
	}

	// 'text-shadow: none' is the empty shadow list: it interpolates with a list (padded with transparent shadows).
	if (definition && definition->GetId() == PropertyId::TextShadow && p0.unit != p1.unit &&
		(p0.unit == Unit::STYLE_VALUE || p1.unit == Unit::STYLE_VALUE) && (p0.unit == Unit::KEYWORD || p1.unit == Unit::KEYWORD))
	{
		const Property empty(StyleValuePtr(MakeShared<TextShadowListValue>()), Unit::STYLE_VALUE);
		return InterpolateProperties(p0.unit == Unit::KEYWORD ? empty : p0, p1.unit == Unit::KEYWORD ? empty : p1, alpha, element, definition);
	}

	if (p0.unit == Unit::STYLE_VALUE && p1.unit == Unit::STYLE_VALUE)
	{
		const StyleValuePtr v0 = p0.value.Get<StyleValuePtr>();
		const StyleValuePtr v1 = p1.value.Get<StyleValuePtr>();
		if (v0 && v1)
		{
			if (StyleValuePtr result = v0->Interpolate(*v1, alpha, element))
				return Property{std::move(result), Unit::STYLE_VALUE};
		}
		return p_discrete;
	}

	// Fall back to discrete interpolation for incompatible units.
	return p_discrete;
}

static PrepareTransformResult PrepareTransformPair(Transform& t0, Transform& t1, Element& element)
{
	using namespace Transforms;

	// Insert or modify primitives such that the two transforms match exactly in both number of and types of primitives.
	// Based largely on https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms

	auto& prims0 = t0.GetPrimitives();
	auto& prims1 = t1.GetPrimitives();

	// Check for trivial case where they contain the same primitives
	if (prims0.size() == prims1.size())
	{
		PrepareTransformResult result = PrepareTransformResult::Unchanged;
		bool same_primitives = true;

		for (size_t i = 0; i < prims0.size(); i++)
		{
			auto p0_type = prims0[i].type;
			auto p1_type = prims1[i].type;

			// See if they are the same or can be converted to a matching generic type.
			if (TransformUtilities::TryConvertToMatchingGenericType(prims0[i], prims1[i]))
			{
				if (prims0[i].type != p0_type)
					result = PrepareTransformResult((int)result | (int)PrepareTransformResult::ChangedT0);
				if (prims1[i].type != p1_type)
					result = PrepareTransformResult((int)result | (int)PrepareTransformResult::ChangedT1);
			}
			else
			{
				same_primitives = false;
				break;
			}
		}
		if (same_primitives)
			return result;
	}

	if (prims0.size() != prims1.size())
	{
		// Try to match the smallest set of primitives to the larger set, set missing keys in the small set to identity.
		// Requirement: The small set must match types in the same order they appear in the big set.
		// Example: (letter indicates type, number represents values)
		// big:       a0 b0 c0 b1
		//               ^     ^
		// small:     b2 b3
		//            ^  ^
		// new small: a1 b2 c1 b3
		bool prims0_smallest = (prims0.size() < prims1.size());

		auto& small = (prims0_smallest ? prims0 : prims1);
		auto& big = (prims0_smallest ? prims1 : prims0);

		Vector<size_t> matching_indices; // Indices into 'big' for matching types
		matching_indices.reserve(small.size() + 1);

		size_t i_big = 0;
		bool match_success = true;
		bool changed_big = false;

		// Iterate through the small set to see if its types fit into the big set
		for (size_t i_small = 0; i_small < small.size(); i_small++)
		{
			match_success = false;

			for (; i_big < big.size(); i_big++)
			{
				auto big_type = big[i_big].type;

				if (TransformUtilities::TryConvertToMatchingGenericType(small[i_small], big[i_big]))
				{
					// They matched exactly or in their more generic form. One or both primitives may have been converted.
					if (big[i_big].type != big_type)
						changed_big = true;

					matching_indices.push_back(i_big);
					match_success = true;
					i_big += 1;
					break;
				}
			}

			if (!match_success)
				break;
		}

		if (match_success)
		{
			// Success, insert the missing primitives into the small set
			matching_indices.push_back(big.size()); // Needed to copy elements behind the last matching primitive
			small.reserve(big.size());
			size_t i0 = 0;
			for (size_t match_index : matching_indices)
			{
				for (size_t i = i0; i < match_index; i++)
				{
					TransformPrimitive p = big[i];
					TransformUtilities::SetIdentity(p);
					small.insert(small.begin() + i, p);
				}

				// Next value to copy is one-past the matching primitive
				i0 = match_index + 1;
			}

			// The small set has always been changed if we get here, but the big set is only changed
			// if one or more of its primitives were converted to a general form.
			if (changed_big)
				return PrepareTransformResult::ChangedT0andT1;

			return (prims0_smallest ? PrepareTransformResult::ChangedT0 : PrepareTransformResult::ChangedT1);
		}
	}

	// If we get here, things get tricky. Need to do full matrix interpolation.
	// In short, we decompose the Transforms into translation, rotation, scale, skew and perspective components.
	// Then, during update, interpolate these components and combine into a new transform matrix.
	if (!CombineAndDecompose(t0, element))
		return PrepareTransformResult::Invalid;
	if (!CombineAndDecompose(t1, element))
		return PrepareTransformResult::Invalid;

	return PrepareTransformResult::ChangedT0andT1;
}

static bool PrepareTransforms(Vector<AnimationKey>& keys, Element& element, int start_index)
{
	bool result = true;

	// Prepare each transform individually.
	for (int i = start_index; i < (int)keys.size(); i++)
	{
		Property& property = keys[i].property;
		RMLUI_ASSERT(property.value.GetType() == Variant::TRANSFORMPTR);

		if (!property.value.GetReference<TransformPtr>())
			property.value = MakeShared<Transform>();

		Transform& transform = *property.value.GetReference<TransformPtr>();
#ifdef RMLUI_MATH_EXPRESSIONS
		if (transform.HasCalculations())
			continue;
#endif
		result &= PrepareSingleTransform(transform, element);
	}

	if (!result)
		return false;

	// We don't need to prepare the transforms pairwise if we only have a single key added so far.
	if (keys.size() < 2 || start_index < 1)
		return true;

	// Now, prepare the transforms pair-wise so they can be interpolated.
	const int N = (int)keys.size();

	int count_iterations = -1;
	const int max_iterations = 3 * N;

	Vector<bool> dirty_list(N + 1, false);
	dirty_list[start_index] = true;

	// For each pair of keys, match the transform primitives such that they can be interpolated during animation update
	for (int i = start_index; i < N && count_iterations < max_iterations; count_iterations++)
	{
		if (!dirty_list[i])
		{
			++i;
			continue;
		}

		auto& prop0 = keys[i - 1].property;
		auto& prop1 = keys[i].property;

		if (prop0.unit != Unit::TRANSFORM || prop1.unit != Unit::TRANSFORM)
			return false;

		auto& t0 = prop0.value.GetReference<TransformPtr>();
		auto& t1 = prop1.value.GetReference<TransformPtr>();

#ifdef RMLUI_MATH_EXPRESSIONS
		if ((t0 && t0->HasCalculations()) || (t1 && t1->HasCalculations()))
		{
			dirty_list[i] = false;
			++i;
			continue;
		}
#endif

		auto prepare_result = PrepareTransformPair(*t0, *t1, element);

		if (prepare_result == PrepareTransformResult::Invalid)
			return false;

		bool changed_t0 = ((int)prepare_result & (int)PrepareTransformResult::ChangedT0);
		bool changed_t1 = ((int)prepare_result & (int)PrepareTransformResult::ChangedT1);

		dirty_list[i] = false;
		dirty_list[i - 1] = dirty_list[i - 1] || changed_t0;
		dirty_list[i + 1] = dirty_list[i + 1] || changed_t1;

		if (changed_t0 && i > 1)
			--i;
		else
			++i;
	}

	// Something has probably gone wrong if we exceeded max_iterations, possibly a bug in PrepareTransformPair()
	return (count_iterations < max_iterations);
}

static void PrepareDecorator(AnimationKey& key)
{
	Property& property = key.property;
	RMLUI_ASSERT(property.value.GetType() == Variant::DECORATORSPTR);

	if (!property.value.GetReference<DecoratorsPtr>())
		property.value = MakeShared<DecoratorDeclarationList>();
}
static void PrepareFilter(AnimationKey& key)
{
	Property& property = key.property;
	RMLUI_ASSERT(property.value.GetType() == Variant::FILTERSPTR);

	if (!property.value.GetReference<FiltersPtr>())
		property.value = MakeShared<FilterDeclarationList>();
}

static bool IsNumericProperty(const Property& property)
{
	return Any(property.unit & Unit::NUMERIC)
#ifdef RMLUI_MATH_EXPRESSIONS
		|| property.unit == Unit::CALCULATION
#endif
		;
}

bool IsInterpolable(const Property& from, const Property& to, Element& element, const PropertyDefinition* definition)
{
	// '-webkit-line-clamp' animates discretely, as in Chromium (fork, P6-L1): its integer is not interpolated.
	if (definition && definition->GetId() == PropertyId::WebkitLineClamp)
		return false;

	// 'text-shadow: none' against a shadow list (the empty list, padded).
	if (definition && definition->GetId() == PropertyId::TextShadow && from.unit != to.unit &&
		(from.unit == Unit::STYLE_VALUE || to.unit == Unit::STYLE_VALUE) && (from.unit == Unit::KEYWORD || to.unit == Unit::KEYWORD))
		return true;

	if (IsNumericProperty(from) && IsNumericProperty(to))
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

	// 'currentcolor' or a color-mix() of it against a colour or another one (the element's colour at each frame).
	const bool current_from = PropertyParserColour::DependsOnCurrentColor(from), current_to = PropertyParserColour::DependsOnCurrentColor(to);
	if ((current_from || current_to) && (current_from || from.unit == Unit::COLOUR) && (current_to || to.unit == Unit::COLOUR))
		return !(from == to);

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
	case Unit::TRANSFORM: return true;
	case Unit::DECORATOR:
		// A list from or to 'none' fades in or out; two lists interpolate when their decorators match.
		if (!from.value.GetReference<DecoratorsPtr>() || !to.value.GetReference<DecoratorsPtr>())
			return true;
		break;
	case Unit::FILTER:
		if (!from.value.GetReference<FiltersPtr>() || !to.value.GetReference<FiltersPtr>())
			return true;
		break;
	case Unit::COLORSTOPLIST:
	case Unit::BOXSHADOWLIST: break;
	case Unit::STYLE_VALUE:
	{
		const StyleValuePtr v0 = from.value.Get<StyleValuePtr>();
		const StyleValuePtr v1 = to.value.Get<StyleValuePtr>();
		return v0 && v1 && v0->Interpolate(*v1, 0.5f, element) != nullptr;
	}
	default: return false;
	}

	// Lists: interpolable unless the interpolation falls back to one of the ends (mismatched decorators or filters,
	// colour stop lists of different lengths, an inset against an outset shadow).
	const Property half = InterpolateProperties(from, to, 0.5f, element, definition);
	return !(half == to) && !(half == from);
}

bool AnimationTrack::AddKey(float offset, const Property& in_property, Element& element, Tween tween, bool has_tween)
{
	// Every value animates at least discretely (CSS); only values that are not resolved are refused.
	if (in_property.unit == Unit::UNKNOWN || in_property.unit == Unit::VAR_EXPRESSION || in_property.unit == Unit::SHORTHAND_PLACEHOLDER ||
		in_property.unit == Unit::CSS_WIDE_KEYWORD)
	{
		Log::Message(Log::LT_WARNING, "Property value does not support animations or transitions. Value: %s", in_property.ToString().c_str());
		return false;
	}

	keys.emplace_back(offset, in_property, tween, has_tween);
	Property& property = keys.back().property;
	bool result = true;

	if (property.unit == Unit::TRANSFORM)
	{
		result = PrepareTransforms(keys, element, (int)keys.size() - 1);
	}
	else if (property.unit == Unit::DECORATOR)
	{
		PrepareDecorator(keys.back());
	}
	else if (property.unit == Unit::FILTER)
	{
		PrepareFilter(keys.back());
	}

	if (!result)
	{
		Log::Message(Log::LT_WARNING, "Could not add animation key with property '%s'.", in_property.ToString().c_str());
		keys.pop_back();
	}

	return result;
}

Property AnimationTrack::GetValue(float progress, const Tween& default_tween, bool before_flag, Element& element) const
{
	if (keys.empty())
		return Property{};
	if (keys.size() == 1)
		return keys[0].property;

	// The segment that holds the progress: from the last key at or before it (the first key before the first offset, the
	// last segment at the end), eased by the segment's start key.
	size_t key0 = 0;
	for (size_t i = 0; i + 1 < keys.size(); i++)
	{
		if (keys[i].time <= progress)
			key0 = i;
	}
	const size_t key1 = key0 + 1;

	const float t0 = keys[key0].time;
	const float t1 = keys[key1].time;
	float local = 1.f;
	if (t1 - t0 > 1e-6f)
		local = (progress - t0) / (t1 - t0);

	const Tween& tween = (keys[key0].has_tween ? keys[key0].tween : default_tween);
	const float alpha = tween.Evaluate(local, before_flag);

	return InterpolateProperties(keys[key0].property, keys[key1].property, alpha, element, keys[0].property.definition);
}

ElementAnimation::ElementAnimation(PropertyId property_id, ElementAnimationOrigin origin, const Property& current_value, Element& element,
	double start_world_time, float duration, int num_iterations, bool alternate_direction) :
	origin(origin), start_time(start_world_time), duration(duration),
	iteration_count(num_iterations < 0 ? std::numeric_limits<float>::infinity() : float(num_iterations)),
	direction(alternate_direction ? AnimationDirection::Alternate : AnimationDirection::Normal), timing_function(Tween::LinearFunction({}))
{
	if (!current_value.definition)
	{
		Log::Message(Log::LT_WARNING, "Property in animation key did not have a definition (while adding key '%s').",
			current_value.ToString().c_str());
	}
	// Transitions show their start value during their delay.
	if (origin == ElementAnimationOrigin::Transition)
		fill_mode = AnimationFillMode::Backwards;
	tracks.emplace_back(property_id);
	if (!tracks[0].AddKey(0.0f, current_value, element, Tween{}, true))
		tracks.clear();
}

ElementAnimation::ElementAnimation(const Animation& animation, int occurrence, double start_world_time) :
	origin(ElementAnimationOrigin::Animation), start_time(start_world_time), delay(animation.delay), duration(animation.duration),
	iteration_count(animation.iteration_count), direction(animation.direction), fill_mode(animation.fill_mode), paused(animation.paused),
	hold_time(0), timing_function(animation.tween), name(animation.name), occurrence(occurrence), animation(animation)
{}

bool ElementAnimation::AddKey(float target_time, const Property& in_property, Element& element, Tween tween, bool extend_duration)
{
	if (!IsInitalized())
	{
		Log::Message(Log::LT_WARNING, "Element animation was not initialized properly, can't add key.");
		return false;
	}

	Vector<AnimationKey>& keys = tracks[0].GetKeys();
	const float old_duration = duration;
	const float new_duration = (extend_duration ? Math::Max(target_time, 0.f) : duration);

	// The tween given with a key eases the segment that ends at it: it belongs to the segment's start key.
	const AnimationKey previous_last = keys.back();
	keys.back().tween = tween;
	keys.back().has_tween = true;

	// Keys are stored at offsets of the iteration, rescaled when the iteration grows.
	if (new_duration != old_duration && new_duration > 0.f)
	{
		for (AnimationKey& key : keys)
			key.time = key.time * old_duration / new_duration;
	}
	const float offset = (new_duration > 0.f ? Math::Clamp(target_time / new_duration, 0.f, 1.f) : 1.f);

	if (!tracks[0].AddKey(offset, in_property, element, Tween{}, true))
	{
		// Restore the keys as they were.
		if (new_duration != old_duration && new_duration > 0.f && old_duration > 0.f)
			for (AnimationKey& key : keys)
				key.time = key.time * new_duration / old_duration;
		keys.back() = previous_last;
		return false;
	}

	duration = new_duration;
	return true;
}

void ElementAnimation::AddTrack(AnimationTrack track)
{
	tracks.push_back(std::move(track));
}

float ElementAnimation::GetActiveDuration() const
{
	if (duration <= 0.f || iteration_count <= 0.f)
		return 0.f;
	if (std::isinf(iteration_count))
		return std::numeric_limits<float>::infinity();
	return duration * iteration_count;
}

ElementAnimation::Sample ElementAnimation::GetSample(double world_time) const
{
	// Web Animations, "Calculating the phase", "active time", "simple iteration progress", "directed progress".
	Sample sample;
	const double local_time = (paused ? hold_time : world_time - start_time);
	const double active_duration = (double)GetActiveDuration();
	const double end_time = Math::Max((double)delay + active_duration, 0.0);
	const double before_active_boundary = Math::Max(Math::Min((double)delay, end_time), 0.0);
	const double active_after_boundary = Math::Max(Math::Min((double)delay + active_duration, end_time), 0.0);

	if (local_time < before_active_boundary)
		sample.phase = AnimationPhase::Before;
	else if (local_time >= active_after_boundary)
		sample.phase = AnimationPhase::After;
	else
		sample.phase = AnimationPhase::Active;

	const bool fills_backwards = (fill_mode == AnimationFillMode::Backwards || fill_mode == AnimationFillMode::Both);
	const bool fills_forwards = (fill_mode == AnimationFillMode::Forwards || fill_mode == AnimationFillMode::Both);

	double active_time = 0.0;
	switch (sample.phase)
	{
	case AnimationPhase::Before:
		sample.has_value = fills_backwards;
		active_time = Math::Max(local_time - (double)delay, 0.0);
		break;
	case AnimationPhase::Active:
		sample.has_value = true;
		active_time = local_time - (double)delay;
		break;
	case AnimationPhase::After:
	case AnimationPhase::Idle:
		sample.has_value = fills_forwards;
		active_time = Math::Max(Math::Min(local_time - (double)delay, active_duration), 0.0);
		break;
	}
	sample.elapsed_time = (float)active_time;

	double overall_progress = 0.0;
	if (duration <= 0.f)
		overall_progress = (sample.phase == AnimationPhase::Before ? 0.0 : (double)iteration_count);
	else
		overall_progress = active_time / (double)duration;

	double simple_progress = (std::isinf(overall_progress) ? 0.0 : std::fmod(overall_progress, 1.0));
	if (simple_progress == 0.0 && sample.phase != AnimationPhase::Before && active_time == active_duration && iteration_count != 0.f &&
		!std::isinf(overall_progress))
		simple_progress = 1.0;
	if (iteration_count == 0.f)
		simple_progress = 0.0;

	double current_iteration = 0.0;
	if (sample.phase == AnimationPhase::After && std::isinf(iteration_count))
		current_iteration = std::numeric_limits<double>::infinity();
	else if (simple_progress == 1.0)
		current_iteration = Math::Max(std::floor(overall_progress) - 1.0, 0.0);
	else
		current_iteration = std::floor(overall_progress);
	sample.iteration = current_iteration;

	bool forwards = true;
	switch (direction)
	{
	case AnimationDirection::Normal: forwards = true; break;
	case AnimationDirection::Reverse: forwards = false; break;
	case AnimationDirection::Alternate: forwards = std::isinf(current_iteration) || std::fmod(current_iteration, 2.0) == 0.0; break;
	case AnimationDirection::AlternateReverse: forwards = !(std::isinf(current_iteration) || std::fmod(current_iteration, 2.0) == 0.0); break;
	}

	sample.progress = (float)(forwards ? simple_progress : 1.0 - simple_progress);
	sample.before_flag = ((sample.phase == AnimationPhase::Before && forwards) || (sample.phase == AnimationPhase::After && !forwards));
	return sample;
}

Property ElementAnimation::GetValue(size_t track_index, const Sample& sample, Element& element) const
{
	if (track_index >= tracks.size() || !sample.has_value)
		return Property{};
	return tracks[track_index].GetValue(sample.progress, timing_function, sample.before_flag, element);
}

void ElementAnimation::RecordProgress(const Sample& sample)
{
	if (tracks.empty())
		return;
	// The eased progress of the first segment: a transition's output progress, for the reversing rule.
	const Vector<AnimationKey>& keys = tracks[0].GetKeys();
	const Tween& tween = (!keys.empty() && keys[0].has_tween ? keys[0].tween : timing_function);
	last_eased_progress = (sample.has_value ? tween.Evaluate(sample.progress, sample.before_flag) : 0.f);
}

Property ElementAnimation::UpdateAndGetProperty(double world_time, Element& element)
{
	if (!IsInitalized() || tracks[0].GetKeys().size() < 2 || animation_complete)
		return Property{};

	const Sample sample = GetSample(world_time);
	RecordProgress(sample);
	if (sample.phase == AnimationPhase::After)
		animation_complete = true;
	if (!sample.has_value && sample.phase != AnimationPhase::After)
		return Property{};

	// A user animation ends at its last value.
	Sample value_sample = sample;
	value_sample.has_value = true;
	return GetValue(0, value_sample, element);
}

void ElementAnimation::SetPaused(bool new_paused, double world_time)
{
	if (new_paused == paused)
		return;
	if (new_paused)
		hold_time = world_time - start_time;
	else
		start_time = world_time - hold_time;
	paused = new_paused;
}

void ElementAnimation::UpdateTiming(const Animation& new_animation, double world_time)
{
	delay = new_animation.delay;
	duration = new_animation.duration;
	iteration_count = new_animation.iteration_count;
	direction = new_animation.direction;
	fill_mode = new_animation.fill_mode;
	timing_function = new_animation.tween;
	SetPaused(new_animation.paused, world_time);
	animation = new_animation;
}

void ElementAnimation::SetTransitionData(const Transition& transition, const Property& in_reversing_adjusted_start, float in_reversing_factor)
{
	delay = transition.delay;
	allow_discrete = transition.allow_discrete;
	reversing_adjusted_start = in_reversing_adjusted_start;
	reversing_shortening_factor = in_reversing_factor;
}

const Property* ElementAnimation::GetEndValue() const
{
	if (tracks.empty() || tracks[0].GetKeys().empty())
		return nullptr;
	return &tracks[0].GetKeys().back().property;
}

const Property* ElementAnimation::GetStartValue() const
{
	if (tracks.empty() || tracks[0].GetKeys().empty())
		return nullptr;
	return &tracks[0].GetKeys().front().property;
}

bool InterpolateDecoratorDeclaration(PropertyDictionary& properties, const DecoratorDeclaration& d0, const DecoratorDeclaration& d1, float alpha,
	Element& element)
{
	return InterpolateEffectProperties(properties, EffectDeclarationView(d0), EffectDeclarationView(d1), alpha, element);
}

} // namespace Rml
