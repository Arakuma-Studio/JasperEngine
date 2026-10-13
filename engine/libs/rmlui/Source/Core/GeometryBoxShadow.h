#pragma once

#include "../../Include/RmlUi/Core/DecorationTypes.h"
#include "../../Include/RmlUi/Core/RenderBox.h"
#include "../../Include/RmlUi/Core/Types.h"

namespace Rml {
using RenderBoxList = Vector<RenderBox>;
struct BoxShadowGeometryInfo {
	ColourbPremultiplied background_color;
	Array<ColourbPremultiplied, 4> border_colors;
	Vector2i texture_dimensions;
	Vector2f element_offset_in_texture;
	RenderBoxList padding_render_boxes;
	RenderBoxList border_render_boxes;
	BoxShadowList shadow_list;
	float opacity;
	// Fork (P4-G7): how the background and border drawn into the texture are anti-aliased (MeshUtilities feathering and the
	// size of a device pixel in the element's units): a texture made at another pixel size has other edges, and must not be
	// shared with an element drawn at this one.
	bool feathering = true;
	float feather_unit = 1.f;
};
inline bool operator==(const BoxShadowGeometryInfo& a, const BoxShadowGeometryInfo& b)
{
	return a.background_color == b.background_color && a.border_colors == b.border_colors &&
		a.texture_dimensions == b.texture_dimensions && a.element_offset_in_texture == b.element_offset_in_texture &&
		a.padding_render_boxes == b.padding_render_boxes && a.border_render_boxes == b.border_render_boxes && a.shadow_list == b.shadow_list &&
		a.opacity == b.opacity && a.feathering == b.feathering && a.feather_unit == b.feather_unit;
}
inline bool operator!=(const BoxShadowGeometryInfo& a, const BoxShadowGeometryInfo& b)
{
	return !(a == b);
}

class Geometry;
class CallbackTexture;
class RenderManager;

class GeometryBoxShadow {
public:
	/// Resolve the element's properties into its box geometry info.
	/// @param[in] element The element to resolve.
	/// The radii are those of the element's render boxes (percentages and overlapping curves resolved per box).
	/// @param[in] background_color The background colour of the element.
	/// @param[in] border_colors The border colours of the element.
	/// @param[in] opacity The computed opacity of the element.
	static BoxShadowGeometryInfo Resolve(Element* element, ColourbPremultiplied background_color,
		const Array<ColourbPremultiplied, 4>& border_colors, float opacity);

	/// Generate the texture and geometry for a box shadow and including the element's background and border.
	/// @param[out] out_shadow_texture The target texture, assumes pointer stability during the lifetime of the shadow geometry.
	/// @param[out] out_background_border_geometry The generated geometry for the element's background and border.
	/// @param[in] render_manager The render manager to generate the shadow for.
	/// @param[in] shadow_geometry_info The resolved box shadow geometry of a given element.
	///	@see GeometryBoxShadow::Resolve()
	static void GenerateTexture(CallbackTexture& out_shadow_texture, Geometry& out_background_border_geometry, RenderManager& render_manager,
		const BoxShadowGeometryInfo& shadow_geometry_info);
};

} // namespace Rml
