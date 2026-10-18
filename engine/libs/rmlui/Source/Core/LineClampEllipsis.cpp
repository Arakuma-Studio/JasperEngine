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
#include "ElementMeta.h"

namespace Rml {

void LineClampAccess::SetBlockEllipsis(Element* block, const LineClampEllipsis* ellipsis)
{
	if (!ellipsis)
	{
		block->meta->line_clamp_ellipsis.reset();
		return;
	}
	block->meta->line_clamp_ellipsis = MakeUnique<LineClampBlockEllipsis>();
	block->meta->line_clamp_ellipsis->ellipsis = *ellipsis;
}

bool LineClampAccess::HasBlockEllipsis(const Element* block)
{
	return block->meta->line_clamp_ellipsis != nullptr;
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

void LineClamp::RenderBlockEllipsis(Element* block, LineClampBlockEllipsis& block_ellipsis)
{
	const LineClampEllipsis& ellipsis = block_ellipsis.ellipsis;
	RenderManager* render_manager = block->GetRenderManager();
	if (!render_manager || ellipsis.font_face_handle == 0)
		return;

	FontEngineInterface* font_engine_interface = GetFontEngineInterface();
	const int version = font_engine_interface->GetVersion(ellipsis.font_face_handle);
	const float opacity = block->GetDrawOpacity();
	if (version != block_ellipsis.font_handle_version || opacity != block_ellipsis.opacity)
	{
		block_ellipsis.font_handle_version = version;
		block_ellipsis.opacity = opacity;
		const TextShapingContext text_shaping_context{ellipsis.language, ellipsis.direction, ellipsis.font_kerning, ellipsis.letter_spacing,
			ellipsis.word_spacing};
		TexturedMeshList mesh_list;
		font_engine_interface->GenerateString(*render_manager, ellipsis.font_face_handle, 0, ellipsis.text, ellipsis.position,
			ellipsis.colour.ToPremultiplied(opacity), opacity, text_shaping_context, mesh_list);
		block_ellipsis.geometry.clear();
		block_ellipsis.textures.clear();
		for (TexturedMesh& mesh : mesh_list)
		{
			block_ellipsis.geometry.push_back(render_manager->MakeGeometry(std::move(mesh.mesh)));
			block_ellipsis.textures.push_back(mesh.texture);
		}
	}

	// It is content of the block: it scrolls with it and is clipped by its overflow clip, as the text of its lines is.
	const bool clips = (block->GetComputedValues().overflow_x() != Style::Overflow::Visible ||
		block->GetComputedValues().overflow_y() != Style::Overflow::Visible);
	if (clips && !ElementUtilities::SetClippingRegion(block, true))
		return;
	const Vector2f translation = block->GetAbsoluteOffset(BoxArea::Border) - Vector2f(block->GetScrollLeft(), block->GetScrollTop());
	for (size_t i = 0; i < block_ellipsis.geometry.size(); i++)
		block_ellipsis.geometry[i].Render(translation, block_ellipsis.textures[i]);
	if (clips)
		ElementUtilities::SetClippingRegion(block);
}

} // namespace Rml
