#include "ElementBackgroundBorder.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementDocument.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "BoxShadowCache.h"
#include "ElementBackgroundLayers.h"
#include "GeometryBackgroundBorder.h"
#include "GeometryBorderStyles.h"
#include "GeometryBoxShadow.h"
#include "TransformState.h"
#include <cmath>

namespace Rml {

ElementBackgroundBorder::ElementBackgroundBorder() {}

ElementBackgroundBorder::~ElementBackgroundBorder() {}

// The canvas background (CSS Backgrounds 3, 2.11.2): the document's background, or when it has none the background of its
// first child named as the document's body.
static bool HasOwnBackground(Element* element)
{
	const ComputedValues& computed = element->GetComputedValues();
	return computed.background_color().alpha > 0 || computed.has_background_image();
}

static Element* GetCanvasSource(ElementDocument* document)
{
	if (!document || !document->HasCanvasBackground())
		return nullptr;
	if (HasOwnBackground(document))
		return document;
	const String& body_tag = document->GetCanvasBodyTag();
	if (body_tag.empty())
		return nullptr;
	for (int i = 0; i < document->GetNumChildren(); i++)
	{
		Element* child = document->GetChild(i);
		if (child->GetTagName() == body_tag)
			return HasOwnBackground(child) ? child : nullptr;
	}
	return nullptr;
}

void ElementBackgroundBorder::RenderCanvas(Element* document_element)
{
	ElementDocument* document = rmlui_dynamic_cast<ElementDocument*>(document_element);
	Element* source = GetCanvasSource(document);
	Context* context = document_element->GetContext();
	RenderManager* render_manager = document_element->GetRenderManager();
	if (!source || !context || !render_manager)
		return;

	const Vector2f canvas_size(context->GetDimensions());
	const ComputedValues& computed = source->GetComputedValues();

	// Generated again when the source, its background or the canvas changes.
	String signature = CreateString("%p %g %g ", (void*)source, canvas_size.x, canvas_size.y);
	{
		const Colourb c = computed.background_color();
		signature += CreateString("%d,%d,%d,%d %g ", c.red, c.green, c.blue, c.alpha, source->GetDrawOpacity());
	}
	for (PropertyId id : {PropertyId::BackgroundImage, PropertyId::BackgroundPositionX, PropertyId::BackgroundPositionY, PropertyId::BackgroundSize,
			 PropertyId::BackgroundRepeat, PropertyId::BackgroundOrigin, PropertyId::BackgroundClip, PropertyId::BackgroundAttachment})
	{
		if (const Property* property = source->GetProperty(id))
			signature += property->ToString() + ";";
	}
	if (signature != canvas_signature)
	{
		canvas_signature = signature;
		Mesh mesh;
		MeshUtilities::GenerateQuad(mesh, Vector2f(0.f), canvas_size, computed.background_color().ToPremultiplied(source->GetDrawOpacity()));
		canvas_colour = render_manager->MakeGeometry(std::move(mesh));
		if (canvas_layers)
			canvas_layers->Dirty();
	}

	// Over the whole context: the colour, then the layers, sized and positioned in the document's boxes.
	canvas_colour.Render(Vector2f(0.f));
	if (computed.has_background_image())
	{
		if (!canvas_layers)
			canvas_layers = MakeUnique<ElementBackgroundLayers>(ElementBackgroundLayers::Kind::Background);
		const Rectanglef canvas = Rectanglef::FromPositionSize(-source->GetAbsoluteOffset(BoxArea::Border), canvas_size);
		canvas_layers->Render(source, &canvas, document_element);
	}
}

// The size of a device pixel in the element's units: the context's pixel size (SetGeometryPixelSize) over the scale the
// element's transform draws it with, in steps of a fourth of an octave so that an animated scale generates the geometry a
// few times only.
static float GetFeatherUnit(Element* element)
{
	const float pixel_size = GetGeometryPixelSize();
	const TransformState* transform_state = element->GetTransformState();
	const Matrix4f* transform = (transform_state ? transform_state->GetTransform() : nullptr);
	if (!transform)
		return pixel_size;
	const Matrix4f& m = *transform;
	const float determinant = Math::Absolute(m[0][0] * m[1][1] - m[0][1] * m[1][0]);
	const float w = Math::Absolute(m[3][3]);
	if (determinant < 1e-8f || w < 1e-8f)
		return pixel_size;
	const float scale = Math::SquareRoot(determinant) / w;
	if (Math::Absolute(scale - 1.f) < 0.05f)
		return pixel_size;
	const float step = Math::Round(4.f * std::log2(scale)) / 4.f;
	return pixel_size * Math::Clamp(std::pow(2.f, -step), 1.f / 16.f, 16.f);
}

void ElementBackgroundBorder::Render(Element* element)
{
	// The canvas background, below everything of the document (and the body's, when it is the canvas's).
	ElementDocument* owner = element->GetOwnerDocument();
	const bool on_canvas = (owner && owner->HasCanvasBackground() && GetCanvasSource(owner) == element);
	if (on_canvas != background_on_canvas)
	{
		background_on_canvas = on_canvas;
		background_dirty = true;
	}
	if (owner == element && owner->HasCanvasBackground())
		RenderCanvas(element);

	const float new_feather_unit = GetFeatherUnit(element);
	if (new_feather_unit != feather_unit)
	{
		feather_unit = new_feather_unit;
		background_dirty = true;
	}

	if (background_dirty || border_dirty)
	{
		for (auto& background : backgrounds)
		{
			if (background.first != BackgroundType::BackgroundBorder && background.first != BackgroundType::BorderAbove &&
				background.first != BackgroundType::Outline)
				background.second.geometry.Release();
		}
		// The outline follows the border box and its radii.
		outline_dirty = true;

		GenerateGeometry(element);

		background_dirty = false;
		border_dirty = false;
	}
	else if ((element->GetComputedValues().has_decorator() || (element->GetComputedValues().has_background_image() && !background_on_canvas)) !=
		border_above_decorators)
	{
		// decorators came or went: the border moves above or below them (the clip geometries stay valid)
		GenerateGeometry(element);
	}

	if (Background* shadow = GetBackground(BackgroundType::BoxShadowAndBackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		shadow->box_shadow_and_background_border->geometry.Render(offset, shadow->box_shadow_and_background_border->texture);
	}
	else if (Background* background = GetBackground(BackgroundType::BackgroundBorder))
	{
		const Vector2f offset = element->GetAbsoluteOffset(BoxArea::Border);
		background->geometry.Render(offset);
	}

	// The background layers, between the colour and the border (CSS paints them so; the border is drawn by RenderBorder).
	if (element->GetComputedValues().has_background_image() && !background_on_canvas)
	{
		if (!layers)
			layers = MakeUnique<ElementBackgroundLayers>(ElementBackgroundLayers::Kind::Background);
		layers->Render(element);
	}
	else if (layers)
	{
		layers.reset();
	}
}

void ElementBackgroundBorder::RenderBorder(Element* element)
{
	if (Background* border = GetBackground(BackgroundType::BorderAbove))
	{
		if (border->geometry)
			border->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border));
	}
}

void ElementBackgroundBorder::RenderOutline(Element* element)
{
	if (outline_dirty || background_dirty || border_dirty)
	{
		GenerateOutline(element);
		outline_dirty = false;
	}
	if (Background* outline = GetBackground(BackgroundType::Outline))
	{
		if (outline->geometry)
			outline->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border));
	}
}

// The radii of a box's corners at 'distance' outside of it (inside if negative), as the radii of a box shadow's spread:
// a square corner stays square, small radii grow by less than the distance (CSS Backgrounds 3, 6.1.1).
static CornerSizes2 OutsetRadii(CornerSizes2 radii, float distance)
{
	for (Vector2f& corner : radii)
	{
		if (corner.x <= 0.f || corner.y <= 0.f)
		{
			corner = Vector2f(0.f);
			continue;
		}
		for (int axis = 0; axis < 2; axis++)
		{
			float& radius = corner[axis];
			float factor = 1.f;
			if (distance > 0.f && radius < distance)
			{
				const float ratio_minus_one = (radius / distance) - 1.f;
				factor = 1.f + ratio_minus_one * ratio_minus_one * ratio_minus_one;
			}
			radius = Math::Max(radius + factor * distance, 0.f);
		}
	}
	return radii;
}

void ElementBackgroundBorder::GenerateOutline(Element* element)
{
	RenderManager* render_manager = element->GetRenderManager();
	const ComputedValues& computed = element->GetComputedValues();
	const Style::BorderStyle style = computed.outline_style();
	if (!render_manager || style == Style::BorderStyle::None || style == Style::BorderStyle::Hidden)
	{
		EraseBackground(BackgroundType::Outline);
		return;
	}

	const float opacity = element->GetDrawOpacity();
	const ColourbPremultiplied color = computed.outline_color().ToPremultiplied(opacity);
	const ColourbPremultiplied transparent(0, 0);
	const bool is_auto = (style == Style::BorderStyle::Auto);

	// Whole pixels, as browsers snap them.
	const float width = computed.outline_width();
	float offset = float(int(computed.outline_offset()));
	if (!is_auto && width <= 0.f)
	{
		EraseBackground(BackgroundType::Outline);
		return;
	}

	Geometry& geometry = GetOrCreateBackground(BackgroundType::Outline).geometry;
	Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);

	// A ring of 'ring_width' whose inner edge lies 'distance' outside of the border box (inside when negative).
	auto add_ring = [&](const RenderBox& border_box, float distance, float ring_width, ColourbPremultiplied ring_color,
						Style::BorderStyle ring_style, float min_outer_radius) {
		const Vector2f size = border_box.GetFillSize();
		// A negative offset shrinks the ring to a line or a point in the middle of the box at most.
		distance = Math::Max(distance, -0.5f * Math::Min(size.x, size.y));
		CornerSizes2 radii = OutsetRadii(border_box.GetBorderRadius(), distance + ring_width);
		for (Vector2f& radius : radii)
		{
			if (radius.x < min_outer_radius || radius.y < min_outer_radius)
				radius = Vector2f(min_outer_radius);
		}
		RenderBox ring(size + Vector2f(2.f * distance), border_box.GetBorderOffset() - Vector2f(distance + ring_width),
			EdgeSizes{ring_width, ring_width, ring_width, ring_width}, radii);
		GeometryBackgroundBorder::ScaleRadiiToFit(radii, ring.GetFillSize() + Vector2f(2.f * ring_width));
		ring.SetBorderRadius(radii);
		ring.SetBorderStyles({ring_style, ring_style, ring_style, ring_style});
		// Chromium's outlines are dotted with square dots at every width.
		ring.SetSquareDots(true);
		const ColourbPremultiplied colors[4] = {ring_color, ring_color, ring_color, ring_color};
		MeshUtilities::GenerateBackgroundBorder(mesh, ring, transparent, colors, feather_unit);
	};

	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox border_box = element->GetRenderBox(BoxArea::Border, i);
		if (is_auto)
		{
			// The focus ring as Chromium 153 draws it (measured, fixture p3/paint/outline-auto): a ring of 2px in the
			// outline colour whatever the outline's width, centred on the border edge moved out by the offset (one pixel
			// further in when the element has a border), and a white ring of 1px around it; square corners are rounded by
			// the ring's width.
			const EdgeSizes border_widths = element->GetRenderBox(BoxArea::Padding, i).GetBorderWidths();
			const bool has_border = (border_widths[0] > 0.f || border_widths[1] > 0.f || border_widths[2] > 0.f || border_widths[3] > 0.f);
			const float centre = offset - (has_border ? 1.f : 0.f);
			add_ring(border_box, centre + 1.f, 1.f, ColourbPremultiplied(255, 255).ToNonPremultiplied().ToPremultiplied(opacity),
				Style::BorderStyle::Solid, 4.f);
			add_ring(border_box, centre - 1.f, 2.f, color, Style::BorderStyle::Solid, 3.f);
		}
		else
		{
			add_ring(border_box, offset, width, color, style, 0.f);
		}
	}

	geometry = render_manager->MakeGeometry(std::move(mesh));
}

void ElementBackgroundBorder::DirtyOutline()
{
	outline_dirty = true;
}

void ElementBackgroundBorder::DirtyBackground()
{
	background_dirty = true;
	if (layers)
		layers->Dirty();
	canvas_signature.clear();
}

void ElementBackgroundBorder::DirtyBorder()
{
	border_dirty = true;
}

Geometry* ElementBackgroundBorder::GetClipGeometry(Element* element, BoxArea clip_area)
{
	BackgroundType type = {};
	switch (clip_area)
	{
	case Rml::BoxArea::Border: type = BackgroundType::ClipBorder; break;
	case Rml::BoxArea::Padding: type = BackgroundType::ClipPadding; break;
	case Rml::BoxArea::Content: type = BackgroundType::ClipContent; break;
	default: RMLUI_ERROR; return nullptr;
	}

	RenderManager* render_manager = element->GetRenderManager();
	Geometry& geometry = GetOrCreateBackground(type).geometry;
	if (render_manager && !geometry)
	{
		Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);
		MeshUtilities::GenerateBackground(mesh, element->GetRenderBox(clip_area, 0, false), ColourbPremultiplied(255));
		geometry = render_manager->MakeGeometry(std::move(mesh));
	}

	return &geometry;
}

ElementBackgroundBorder::Background* ElementBackgroundBorder::GetBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return &it->second;
	return nullptr;
}

ElementBackgroundBorder::Background& ElementBackgroundBorder::GetOrCreateBackground(BackgroundType type)
{
	auto it = backgrounds.find(type);
	if (it != backgrounds.end())
		return it->second;

	Background& background = backgrounds[type];
	return background;
}

void ElementBackgroundBorder::EraseBackground(BackgroundType type)
{
	backgrounds.erase(type);
}

void ElementBackgroundBorder::GenerateGeometry(Element* element)
{
	RMLUI_ZoneScoped;
	RenderManager* render_manager = element->GetRenderManager();
	if (!render_manager)
		return;

	const ComputedValues& computed = element->GetComputedValues();
	const bool has_box_shadow = computed.has_box_shadow();

	const float opacity = element->GetDrawOpacity();
	// (The canvas draws the background of its source.)
	ColourbPremultiplied background_color =
		background_on_canvas ? ColourbPremultiplied(0, 0) : computed.background_color().ToPremultiplied(opacity);
	Array<ColourbPremultiplied, 4> border_colors = {
		computed.border_top_color().ToPremultiplied(opacity),
		computed.border_right_color().ToPremultiplied(opacity),
		computed.border_bottom_color().ToPremultiplied(opacity),
		computed.border_left_color().ToPremultiplied(opacity),
	};

	// CSS paints the background color, then the background images, then the border: with decorators (the images) the
	// border is drawn separately, after them (RenderBorder).
	const bool border_above = computed.has_decorator() || (computed.has_background_image() && !background_on_canvas);
	border_above_decorators = border_above;
	if (border_above)
	{
		Geometry& border_geometry = GetOrCreateBackground(BackgroundType::BorderAbove).geometry;
		Mesh border_mesh = border_geometry.Release(Geometry::ReleaseMode::ClearMesh);
		for (int i = 0; i < element->GetNumBoxes(); i++)
			MeshUtilities::GenerateBackgroundBorder(border_mesh, element->GetRenderBox(BoxArea::Padding, i), ColourbPremultiplied(0, 0),
				border_colors.data(), feather_unit);
		border_geometry = render_manager->MakeGeometry(std::move(border_mesh));
	}
	else
	{
		EraseBackground(BackgroundType::BorderAbove);
	}

	if (has_box_shadow)
	{
		// The box shadow geometry also includes the element's background and border, thus we can skip the normal background generation.
		EraseBackground(BackgroundType::BackgroundBorder);
		Background& shadow_background = GetOrCreateBackground(BackgroundType::BoxShadowAndBackgroundBorder);
		shadow_background.box_shadow_and_background_border = BoxShadowCache::GetHandle(element, computed, !border_above);
		return;
	}

	EraseBackground(BackgroundType::BoxShadowAndBackgroundBorder);

	const ColourbPremultiplied transparent(0, 0);
	const Array<ColourbPremultiplied, 4> no_border = {transparent, transparent, transparent, transparent};
	Geometry& geometry = GetOrCreateBackground(BackgroundType::BackgroundBorder).geometry;
	Mesh mesh = geometry.Release(Geometry::ReleaseMode::ClearMesh);

	// The background colour covers the clip box of the bottom layer (CSS Backgrounds 3, 2.10), the border box by default,
	// under the border.
	const BoxArea colour_clip = ElementBackgroundLayers::GetColourClip(element);
	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox padding_box = element->GetRenderBox(BoxArea::Padding, i);
		RenderBox background_box =
			GeometryBorderStyles::BackgroundRenderBox(element->GetRenderBox(BoxArea::Border, i), padding_box, border_colors.data());
		if (colour_clip != BoxArea::Border)
		{
			// The fill area with the inner curves (no border of its own).
			const RenderBox clip_box = element->GetRenderBox(colour_clip, i);
			const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(clip_box.GetBorderOffset(), clip_box.GetBorderWidths(),
				clip_box.GetFillSize(), clip_box.GetBorderRadius());
			CornerSizes2 radii = metrics.inner_radii;
			for (Vector2f& radius : radii)
				radius = Math::Max(radius, Vector2f(0.f));
			background_box = RenderBox(clip_box.GetFillSize(), clip_box.GetBorderOffset() + clip_box.GetFillOffset(), EdgeSizes{}, radii);
		}
		MeshUtilities::GenerateBackgroundBorder(mesh, background_box, background_color, no_border.data(), feather_unit);
		if (!border_above)
			MeshUtilities::GenerateBackgroundBorder(mesh, padding_box, transparent, border_colors.data(), feather_unit);
	}

	geometry = render_manager->MakeGeometry(std::move(mesh));
}

} // namespace Rml
