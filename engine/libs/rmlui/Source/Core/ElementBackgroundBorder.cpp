#include "ElementBackgroundBorder.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Context.h"
#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "BoxShadowCache.h"
#include "GeometryBorderStyles.h"
#include "GeometryBoxShadow.h"
#include "TransformState.h"
#include <cmath>

namespace Rml {

ElementBackgroundBorder::ElementBackgroundBorder() {}

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
			if (background.first != BackgroundType::BackgroundBorder && background.first != BackgroundType::BorderAbove)
				background.second.geometry.Release();
		}

		GenerateGeometry(element);

		background_dirty = false;
		border_dirty = false;
	}
	else if (element->GetComputedValues().has_decorator() != border_above_decorators)
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
}

void ElementBackgroundBorder::RenderBorder(Element* element)
{
	if (Background* border = GetBackground(BackgroundType::BorderAbove))
	{
		if (border->geometry)
			border->geometry.Render(element->GetAbsoluteOffset(BoxArea::Border));
	}
}

void ElementBackgroundBorder::DirtyBackground()
{
	background_dirty = true;
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

	const float opacity = computed.opacity();
	ColourbPremultiplied background_color = computed.background_color().ToPremultiplied(opacity);
	Array<ColourbPremultiplied, 4> border_colors = {
		computed.border_top_color().ToPremultiplied(opacity),
		computed.border_right_color().ToPremultiplied(opacity),
		computed.border_bottom_color().ToPremultiplied(opacity),
		computed.border_left_color().ToPremultiplied(opacity),
	};

	// CSS paints the background color, then the background images, then the border: with decorators (the images) the
	// border is drawn separately, after them (RenderBorder).
	const bool border_above = computed.has_decorator();
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

	// The background colour covers the border box, under the border (CSS: the initial 'background-clip: border-box').
	for (int i = 0; i < element->GetNumBoxes(); i++)
	{
		const RenderBox padding_box = element->GetRenderBox(BoxArea::Padding, i);
		const RenderBox background_box =
			GeometryBorderStyles::BackgroundRenderBox(element->GetRenderBox(BoxArea::Border, i), padding_box, border_colors.data());
		MeshUtilities::GenerateBackgroundBorder(mesh, background_box, background_color, no_border.data(), feather_unit);
		if (!border_above)
			MeshUtilities::GenerateBackgroundBorder(mesh, padding_box, transparent, border_colors.data(), feather_unit);
	}

	geometry = render_manager->MakeGeometry(std::move(mesh));
}

} // namespace Rml
