#include "LineClampEllipsis.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/FontMetrics.h"
#include "../../Include/RmlUi/Core/Mesh.h"
#include "../../Include/RmlUi/Core/RenderManager.h"
#include "../../Include/RmlUi/Core/TextShapingContext.h"
#include "TransformState.h"
#include "ElementMeta.h"
#include "TextShadow.h"

namespace Rml {

void LineClampAccess::SetBlockEllipsis(Element* block, const LineClampEllipsis* ellipsis, Element* after)
{
	block->draws_line_clamp_ellipsis = (ellipsis != nullptr && after != nullptr);
	if (!block->draws_line_clamp_ellipsis)
	{
		block->meta->line_clamp_ellipsis.reset();
		return;
	}
	block->meta->line_clamp_ellipsis = MakeUnique<LineClampBlockEllipsis>();
	block->meta->line_clamp_ellipsis->ellipsis = *ellipsis;
	block->meta->line_clamp_ellipsis->after = after;
	// (an element whose mark stays after the ellipsis moves on finds no block container that draws one after it)
	after->line_clamp_ellipsis_after = true;
}

void LineClampAccess::DirtyBlockEllipsis(Element* block)
{
	if (block->draws_line_clamp_ellipsis)
		block->meta->line_clamp_ellipsis->dirty = true;
}

void LineClampAccess::SetHidden(Element* element, bool hidden)
{
	if (element->line_clamp_hidden == hidden)
		return;
	element->line_clamp_hidden = hidden;
	element->DirtyStackingContext();
}

bool LineClamp::MakeEllipsis(Element* block, LineClampEllipsis& out_ellipsis)
{
	const FontFaceHandle font_face_handle = block->GetFontFaceHandle();
	if (font_face_handle == 0)
		return false;

	FontEngineInterface* font_engine_interface = GetFontEngineInterface();
	const ComputedValues& computed = block->GetComputedValues();

	// Chromium (LineTruncator::ComputeEllipsisText): U+2026 when the line's font has a glyph for it, otherwise three dots.
	constexpr char ellipsis_chars[] = "\xE2\x80\xA6";
	constexpr char dots_chars[] = "...";
	out_ellipsis.text = (font_engine_interface->GetFontMetrics(font_face_handle).has_ellipsis ? ellipsis_chars : dots_chars);
	out_ellipsis.font_face_handle = font_face_handle;
	out_ellipsis.colour = computed.color();
	out_ellipsis.block = block;
	out_ellipsis.language = computed.language();
	out_ellipsis.direction = computed.direction();
	out_ellipsis.font_kerning = computed.font_kerning();
	out_ellipsis.letter_spacing = computed.letter_spacing();
	out_ellipsis.word_spacing = computed.word_spacing();

	const TextShapingContext text_shaping_context{out_ellipsis.language, out_ellipsis.direction, out_ellipsis.font_kerning,
		out_ellipsis.letter_spacing, out_ellipsis.word_spacing};
	out_ellipsis.width = (float)font_engine_interface->GetStringWidth(font_face_handle, out_ellipsis.text, text_shaping_context);
	return true;
}

void LineClamp::GenerateEllipsis(RenderManager& render_manager, const LineClampEllipsis& ellipsis, Vector2f origin, float opacity,
	Vector<ElementText::TexturedGeometry>& out_geometry, Vector<ElementText::Shadow>& out_shadows)
{
	out_geometry.clear();
	out_shadows.clear();
	if (ellipsis.font_face_handle == 0)
		return;

	FontEngineInterface* font_engine_interface = GetFontEngineInterface();
	const TextShapingContext text_shaping_context{ellipsis.language, ellipsis.direction, ellipsis.font_kerning, ellipsis.letter_spacing,
		ellipsis.word_spacing};
	const Colourb colour = (ellipsis.block ? ellipsis.block->GetComputedValues().color() : ellipsis.colour);

	TexturedMeshList mesh_list;
	font_engine_interface->GenerateString(render_manager, ellipsis.font_face_handle, 0, ellipsis.text, origin, colour.ToPremultiplied(opacity), opacity,
		text_shaping_context, mesh_list);
	for (TexturedMesh& mesh : mesh_list)
		out_geometry.push_back(ElementText::TexturedGeometry{render_manager.MakeGeometry(std::move(mesh.mesh)), mesh.texture});

	// The block container's 'text-shadow' (Chromium paints the ellipsis with the line's style).
	if (!ellipsis.block || !ellipsis.block->GetComputedValues().has_text_shadow())
		return;
	const FontMetrics& metrics = font_engine_interface->GetFontMetrics(ellipsis.font_face_handle);
	for (const ResolvedTextShadow& resolved : ResolveTextShadows(*ellipsis.block))
	{
		const ColourbPremultiplied shadow_colour = resolved.colour.ToPremultiplied(opacity);
		if (shadow_colour.alpha == 0)
			continue;
		ElementText::Shadow shadow;
		shadow.blur = resolved.blur;
		const Vector2f position = origin + resolved.offset;
		TexturedMeshList shadow_meshes;
		font_engine_interface->GenerateString(render_manager, ellipsis.font_face_handle, 0, ellipsis.text, position, shadow_colour, opacity,
			text_shaping_context, shadow_meshes);
		for (TexturedMesh& mesh : shadow_meshes)
			shadow.geometry.push_back(ElementText::TexturedGeometry{render_manager.MakeGeometry(std::move(mesh.mesh)), mesh.texture});
		shadow.ink = Rectanglef::FromCorners(position - Vector2f(0.f, metrics.ascent), position + Vector2f(ellipsis.width, metrics.descent));
		out_shadows.push_back(std::move(shadow));
	}
}

void LineClampAccess::RenderEllipsisAfter(Element* after)
{
	// The block container that draws an ellipsis after this element: an ancestor (or the element itself).
	Element* block = after;
	for (; block; block = block->GetParentNode())
	{
		if (block->draws_line_clamp_ellipsis && block->meta->line_clamp_ellipsis->after == after)
			break;
	}
	if (!block)
	{
		after->line_clamp_ellipsis_after = false;
		return;
	}
	if (block->GetComputedValues().visibility() != Style::Visibility::Visible || block->GetComputedValues().opacity() <= 0.f)
		return;

	LineClampBlockEllipsis& block_ellipsis = *block->meta->line_clamp_ellipsis;
	const LineClampEllipsis& ellipsis = block_ellipsis.ellipsis;
	RenderManager* render_manager = block->GetRenderManager();
	if (!render_manager || ellipsis.font_face_handle == 0)
		return;

	FontEngineInterface* font_engine_interface = GetFontEngineInterface();
	const int version = font_engine_interface->GetVersion(ellipsis.font_face_handle);
	const float opacity = block->GetDrawOpacity();
	if (block_ellipsis.dirty || version != block_ellipsis.font_handle_version || opacity != block_ellipsis.opacity)
	{
		block_ellipsis.dirty = false;
		block_ellipsis.font_handle_version = version;
		block_ellipsis.opacity = opacity;
		LineClamp::GenerateEllipsis(*render_manager, ellipsis, ellipsis.position, opacity, block_ellipsis.geometry, block_ellipsis.shadows);
	}

	// It is content of the block: drawn with its transform, it scrolls with it and is clipped by its overflow clip, as the
	// text of its lines is.
	ElementUtilities::ApplyTransform(*block);
	const bool clips = (block->GetComputedValues().overflow_x() != Style::Overflow::Visible ||
		block->GetComputedValues().overflow_y() != Style::Overflow::Visible);
	if (!ElementUtilities::SetClippingRegion(block, clips))
		return;
	const Vector2f translation = block->GetAbsoluteOffset(BoxArea::Border) - Vector2f(block->GetScrollLeft(), block->GetScrollTop());
	const bool transformed = (block->GetTransformState() && block->GetTransformState()->GetTransform());
	if (!block_ellipsis.shadows.empty())
		ElementText::RenderShadowList(*render_manager, block_ellipsis.shadows, translation, transformed);
	for (const ElementText::TexturedGeometry& part : block_ellipsis.geometry)
		part.geometry.Render(translation, part.texture);
}

void LineClamp::RenderEllipsisAfter(Element* after)
{
	LineClampAccess::RenderEllipsisAfter(after);
}

} // namespace Rml
