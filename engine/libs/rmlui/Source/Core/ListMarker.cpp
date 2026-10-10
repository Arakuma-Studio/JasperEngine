#include "ListMarker.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Factory.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/PropertyIdSet.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/SystemInterface.h"
#include "ContentValue.h"
#include "ElementCascade.h"
#include "ElementDefinition.h"
#include <math.h>

namespace Rml {

RMLUI_RTTI_Define(ElementListMarker)

// Chromium's list markers (core/layout/list/list_marker.cc): the padding of a symbol marker and its size from the
// font's ascent.
static constexpr int marker_padding = 7;

ElementListMarker::ElementListMarker(const String& tag) : ElementPseudo(tag) {}

ElementListMarker::~ElementListMarker() {}

// The list-style-type of an element: a keyword name, a string ('"' first), or "none".
static String GetListStyleType(Element& element)
{
	const Property* property = element.GetProperty(PropertyId::ListStyleType);
	if (!property)
		return "disc";
	if (property->unit == Unit::STRING)
		return "\"" + property->Get<String>();
	return property->ToString();
}

static String GetListStyleImage(Element& element)
{
	const Property* property = element.GetProperty(PropertyId::ListStyleImage);
	if (!property || property->unit != Unit::STRING)
		return String();
	return property->Get<String>();
}

static bool HasContent(const ElementDefinition* definition)
{
	const Property* content = (definition ? definition->GetProperty(PropertyId::Content) : nullptr);
	return content && (content->unit == Unit::STYLE_VALUE || content->unit == Unit::VAR_EXPRESSION);
}

bool ElementListMarker::IsGenerated(Element& element, const ElementDefinition* marker_definition)
{
	if (element.GetComputedValues().display() != Style::Display::ListItem)
		return false;
	if (HasContent(marker_definition))
		return true;
	return GetListStyleType(element) != "none" || !GetListStyleImage(element).empty();
}

bool ElementListMarker::UsesCounters() const
{
	if (custom_content)
		return ElementPseudo::UsesCounters();
	return image_source.empty() && !CounterResolver::IsSymbolStyle(style) && !(style.size() > 0 && style[0] == '"');
}

void ElementListMarker::OnCountersResolved(const CssCounterSet& counters, int& quote_depth)
{
	if (custom_content)
	{
		ElementPseudo::OnCountersResolved(counters, quote_depth);
		RefreshBoxBeforeLayout();
		return;
	}
	last_counters = counters;
	const int new_ordinal = CounterResolver::GetCounterValue(counters, "list-item");
	if (new_ordinal != ordinal)
	{
		ordinal = new_ordinal;
		UpdateGeneratedText();
	}
	RefreshBoxBeforeLayout();
}

void ElementListMarker::RefreshBoxBeforeLayout()
{
	// The counters are resolved right before the layout: the box of the new text is computed now, not in the next
	// update.
	if (!box_dirty)
		return;
	UpdateBox();
	ElementCascade::UpdateProperties(*this);
}

void ElementListMarker::OnGeneratedTextChange()
{
	box_dirty = true;
}

void ElementListMarker::OnUpdate()
{
	ElementPseudo::OnUpdate();
	// A text changed since the last update (an attribute of the item in a ::marker content).
	if (box_dirty && box_applied)
		UpdateBox();
}

void ElementListMarker::OnPropertyChange(const PropertyIdSet& changed_properties)
{
	if (changed_properties.Contains(PropertyId::ListStyleType) || changed_properties.Contains(PropertyId::ListStyleImage))
		content_dirty = true;
	if (changed_properties.Contains(PropertyId::ListStylePosition) || changed_properties.Contains(PropertyId::FontSize) ||
		changed_properties.Contains(PropertyId::FontFamily) || changed_properties.Contains(PropertyId::FontWeight) ||
		changed_properties.Contains(PropertyId::FontStyle) || changed_properties.Contains(PropertyId::LetterSpacing) ||
		changed_properties.Contains(PropertyId::Display))
		box_dirty = true;
	if (changed_properties.Contains(PropertyId::Color) || changed_properties.Contains(PropertyId::Opacity))
		geometry_dirty = true;

	ElementPseudo::OnPropertyChange(changed_properties);

	if (box_dirty)
		UpdateBox();
}

void ElementListMarker::BuildContent()
{
	const Property* property = GetProperty(PropertyId::Content);
	custom_content = (property && property->unit == Unit::STYLE_VALUE);
	if (custom_content)
	{
		text_element = nullptr;
		image_element = nullptr;
		style.clear();
		image_source.clear();
		marker_text.clear();
		ElementPseudo::BuildContent();
		box_dirty = true;
		return;
	}

	content_dirty = false;
	if (content)
	{
		content.reset();
		text_runs.clear();
	}
	const String new_style = GetListStyleType(*this);
	String new_image = GetListStyleImage(*this);
	if (!new_image.empty() && new_image[0] != '/' && new_image.find(':') == String::npos)
	{
		// Relative to the style sheet that declares it.
		const Property* image_property = GetProperty(PropertyId::ListStyleImage);
		if (image_property && image_property->source && !image_property->source->path.empty())
		{
			if (SystemInterface* system_interface = GetSystemInterface())
			{
				String joined;
				system_interface->JoinPath(joined, StringUtilities::Replace(image_property->source->path, '|', ':'), new_image);
				new_image = StringUtilities::Replace(joined, ':', '|');
			}
		}
	}

	const bool counted_before = UsesCounters();
	const bool rebuild = (new_style != style || new_image != image_requested || (!text_element && !image_element));
	style = new_style;
	image_requested = new_image;
	if (rebuild)
	{
		image_source = new_image;
		BuildGenerated();
	}
	UpdateGeneratedText();
	if (counted_before != UsesCounters())
		PseudoElements::DirtyCounters(*this);
	box_dirty = true;
}

void ElementListMarker::BuildGenerated()
{
	while (GetNumChildren() > 0)
		RemoveChild(GetChild(GetNumChildren() - 1));
	text_element = nullptr;
	image_element = nullptr;

	// Markers keep their spaces and their case (Chromium's ::marker defaults).
	SetProperty(PropertyId::WhiteSpace, Property(Style::WhiteSpace::Pre));
	SetProperty(PropertyId::TextTransform, Property(Style::TextTransform::None));

	if (!image_source.empty())
	{
		XMLAttributes attributes;
		attributes["src"] = image_source;
		ElementPtr image = Factory::InstanceElement(this, "img", "img", attributes);
		if (image)
			image_element = AppendChild(std::move(image));
		// An image that cannot be shown: the marker of the list-style-type (CSS Lists 3 §3.2). (Its size is known once
		// it is in the document, where its texture loads.)
		Vector2f dimensions;
		float ratio = 0.f;
		if (!image_element || !image_element->GetIntrinsicDimensions(dimensions, ratio) || dimensions.x <= 0.f || dimensions.y <= 0.f)
		{
			if (image_element)
				RemoveChild(image_element);
			image_element = nullptr;
			image_source.clear();
		}
	}

	// An image marker is the image alone; the others are a text (for a symbol, the space that gives its box a line).
	if (!image_element)
	{
		ElementPtr text = Factory::InstanceElement(this, "#text", "#text", XMLAttributes());
		if (text)
			text_element = rmlui_dynamic_cast<ElementText*>(AppendChild(std::move(text)));
	}
	geometry_dirty = true;
}

void ElementListMarker::UpdateGeneratedText()
{
	if (custom_content)
		return;
	String text;
	if (!image_source.empty())
		text.clear();
	else if (CounterResolver::IsSymbolStyle(style))
		text = " "; // the line of the symbol's box (the symbol is drawn)
	else if (!style.empty() && style[0] == '"')
		text = style.substr(1);
	else if (style != "none")
		text = CounterResolver::FormatCounter(ordinal, style) + ". ";

	marker_text = (CounterResolver::IsSymbolStyle(style) || !image_source.empty() ? String() : text);
	if (text_element && text_element->GetText() != text)
	{
		text_element->SetText(text);
		box_dirty = true;
	}
}

void ElementListMarker::UpdateBox()
{
	box_dirty = false;
	Element* item = GetOriginatingElement();
	if (!item)
		return;

	// 'list-style-position' (inherited from the item): outside (0) or inside (1).
	const Property* position = GetProperty(PropertyId::ListStylePosition);
	const bool outside = (!position || position->unit != Unit::KEYWORD || position->Get<int>() == 0);
	const bool symbol = (!custom_content && image_source.empty() && CounterResolver::IsSymbolStyle(style));

	float width = 0.f;
	float margin_left = 0.f;
	float margin_right = 0.f;
	bool fixed_width = false;
	bool align_right = false;
	// An inline-block keeps the marker's own width (a symbol's) or lets its margins cancel its advance (outside).
	bool inline_block = outside;

	if (symbol)
	{
		const FontFaceHandle font = GetFontFaceHandle();
		const float ascent = (font ? GetFontEngineInterface()->GetFontMetrics(font).ascent : 0.f);
		const int ascent_i = (int)roundf(ascent);
		const int offset = ascent_i * 2 / 3;
		const bool disclosure = (style == "disclosure-open" || style == "disclosure-closed");
		width = disclosure ? GetComputedValues().font_size() * 0.66f : float((offset + 1) / 2 + 2);
		fixed_width = true;
		inline_block = true;
		// Chromium's ListMarker::InlineMarginsForOutside and InlineMarginsForInside.
		const float font_size = GetComputedValues().font_size();
		if (disclosure)
		{
			const float gap = 0.4f * font_size;
			margin_left = (outside ? -(width + gap) : 0.f);
			margin_right = gap;
		}
		else if (outside)
		{
			margin_left = -float(offset + marker_padding + 1);
			margin_right = float(offset + marker_padding + 1) - width;
		}
		else
		{
			margin_left = -1.f;
			margin_right = font_size;
		}
	}
	else if (image_element && !custom_content)
	{
		// Chromium: the image, then 7px to the content; outside, it ends that far from the item's start edge.
		margin_right = float(marker_padding);
		if (outside)
		{
			Vector2f dimensions;
			float ratio = 0.f;
			if (image_element->GetIntrinsicDimensions(dimensions, ratio))
				margin_left = -(dimensions.x + float(marker_padding));
		}
	}
	else if (outside)
	{
		// The marker ends at the item's start edge: as wide as its content measured as one string (the layout of a
		// font engine that rounds each word may be narrower: the text is aligned to the end edge).
		width = 0.f;
		for (int i = 0; i < GetNumChildren(); i++)
		{
			Element* child = GetChild(i);
			if (auto* text = rmlui_dynamic_cast<ElementText*>(child))
			{
				width += (float)ElementUtilities::GetStringWidth(this, text->GetText());
			}
			else
			{
				Vector2f dimensions;
				float ratio = 0.f;
				if (child->GetIntrinsicDimensions(dimensions, ratio))
					width += dimensions.x;
			}
		}
		fixed_width = true;
		align_right = !custom_content;
		margin_left = -width;
	}

	geometry_dirty = true;
	const BoxProperties box{inline_block, fixed_width, align_right, width, margin_left, margin_right};
	if (box_applied && box == applied_box)
		return;
	applied_box = box;
	box_applied = true;

	SetProperty(PropertyId::Display, Property(inline_block ? Style::Display::InlineBlock : Style::Display::Inline));
	if (fixed_width)
		SetProperty(PropertyId::Width, Property(width, Unit::PX));
	else
		RemoveProperty(PropertyId::Width);
	SetProperty(PropertyId::MarginLeft, Property(margin_left, Unit::PX));
	SetProperty(PropertyId::MarginRight, Property(margin_right, Unit::PX));
	if (align_right)
		SetProperty(PropertyId::TextAlign, Property(Style::TextAlign::Right));
	else
		RemoveProperty(PropertyId::TextAlign);
}

void ElementListMarker::OnResize()
{
	ElementPseudo::OnResize();
	geometry_dirty = true;
}

void ElementListMarker::OnLayout()
{
	ElementPseudo::OnLayout();
	geometry_dirty = true;
}

// A filled or stroked ellipse in a rectangle, as triangles.
static void AddEllipse(Mesh& mesh, Rectanglef rect, ColourbPremultiplied colour, float stroke)
{
	const Vector2f centre = rect.Center();
	const Vector2f radius = rect.Size() * 0.5f;
	const int segments = Math::Max(12, (int)(Math::Max(radius.x, radius.y) * 4.f));
	const int first = (int)mesh.vertices.size();
	auto Point = [&](int i, float shrink) {
		const float angle = 2.f * Math::RMLUI_PI * float(i) / float(segments);
		return centre + Vector2f(cosf(angle) * Math::Max(radius.x - shrink, 0.f), sinf(angle) * Math::Max(radius.y - shrink, 0.f));
	};
	if (stroke <= 0.f)
	{
		mesh.vertices.push_back({centre, colour, {}});
		for (int i = 0; i < segments; i++)
			mesh.vertices.push_back({Point(i, 0.f), colour, {}});
		for (int i = 0; i < segments; i++)
		{
			mesh.indices.push_back(first);
			mesh.indices.push_back(first + 1 + i);
			mesh.indices.push_back(first + 1 + (i + 1) % segments);
		}
		return;
	}
	// A ring of the stroke width centred on the ellipse.
	for (int i = 0; i < segments; i++)
	{
		mesh.vertices.push_back({Point(i, -stroke * 0.5f), colour, {}});
		mesh.vertices.push_back({Point(i, stroke * 0.5f), colour, {}});
	}
	for (int i = 0; i < segments; i++)
	{
		const int a = first + 2 * i, b = first + 2 * ((i + 1) % segments);
		mesh.indices.insert(mesh.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
	}
}

static void AddPolygon(Mesh& mesh, std::initializer_list<Vector2f> points, ColourbPremultiplied colour)
{
	const int first = (int)mesh.vertices.size();
	for (const Vector2f& point : points)
		mesh.vertices.push_back({point, colour, {}});
	for (int i = 1; i + 1 < (int)points.size(); i++)
		mesh.indices.insert(mesh.indices.end(), {first, first + i, first + i + 1});
}

void ElementListMarker::GenerateSymbolGeometry()
{
	geometry_dirty = false;
	symbol_geometry = Geometry();
	if (custom_content || !image_source.empty() || !CounterResolver::IsSymbolStyle(style) || !text_element)
		return;
	const FontFaceHandle font = GetFontFaceHandle();
	if (!font)
		return;

	// The symbol's box in Chromium (ListMarker::RelativeSymbolMarkerRect): from the marker's text box, whose top is
	// the baseline minus the ascent.
	const FontMetrics& metrics = GetFontEngineInterface()->GetFontMetrics(font);
	const int ascent = (int)roundf(metrics.ascent);
	float baseline = (float)ascent;
	const ElementText::LineList& lines = text_element->GetLines();
	const Vector2f text_offset = text_element->GetAbsoluteOffset(BoxArea::Border) - GetAbsoluteOffset(BoxArea::Border);
	if (!lines.empty())
		baseline = text_offset.y + lines.front().position.y;
	const float text_top = baseline - float(ascent);
	const float left = GetBox().GetPosition(BoxArea::Content).x;

	Rectanglef rect;
	if (style == "disclosure-open" || style == "disclosure-closed")
	{
		const float size = floorf(GetComputedValues().font_size() * 0.66f);
		rect = Rectanglef::FromPositionSize({left, text_top + float(ascent) - size}, {size, size});
	}
	else
	{
		const float bullet = float((ascent * 2 / 3 + 1) / 2);
		rect = Rectanglef::FromPositionSize({left + 1.f, text_top + float(3 * (ascent - ascent * 2 / 3) / 2)}, {bullet, bullet});
	}

	const ComputedValues& computed = GetComputedValues();
	const ColourbPremultiplied colour = computed.color().ToPremultiplied(computed.opacity());
	Mesh mesh;
	if (style == "disc")
		AddEllipse(mesh, rect, colour, 0.f);
	else if (style == "circle")
		AddEllipse(mesh, rect, colour, 1.f);
	else if (style == "square")
		AddPolygon(mesh, {rect.TopLeft(), rect.TopRight(), rect.BottomRight(), rect.BottomLeft()}, colour);
	else if (style == "disclosure-closed")
	{
		// A triangle pointing right, as Chromium's disclosure path (CreatePath in list_marker_painter.cc).
		const Vector2f p = rect.TopLeft();
		const float s = rect.Width();
		AddPolygon(mesh, {p + Vector2f(0.f, 0.f) * s, p + Vector2f(1.f, 0.5f) * s, p + Vector2f(0.f, 1.f) * s}, colour);
	}
	else if (style == "disclosure-open")
	{
		const Vector2f p = rect.TopLeft();
		const float s = rect.Width();
		AddPolygon(mesh, {p + Vector2f(0.f, 0.f) * s, p + Vector2f(1.f, 0.f) * s, p + Vector2f(0.5f, 1.f) * s}, colour);
	}

	if (RenderManager* render_manager = GetRenderManager())
		symbol_geometry = render_manager->MakeGeometry(std::move(mesh));
}

void ElementListMarker::OnRender()
{
	ElementPseudo::OnRender();
	if (geometry_dirty)
		GenerateSymbolGeometry();
	if (symbol_geometry)
		symbol_geometry.Render(GetAbsoluteOffset(BoxArea::Border));
}

} // namespace Rml
