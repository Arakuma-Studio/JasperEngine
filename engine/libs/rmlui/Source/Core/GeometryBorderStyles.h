#pragma once

#include "../../Include/RmlUi/Core/RenderBox.h"
#include "GeometryBackgroundBorder.h"

namespace Rml {

// The edges a generator feathers (anti-aliasing): none, the curves only (a multisampled target resolves straight edges),
// or every edge (SetGeometryFeathering).
enum class FeatherEdges { None, Curves, All };

/**
    Border geometry for the CSS border styles (phase 3, docs/html-ui/phase-3-fork-patches.md 3.7.3): dotted, dashed,
    double, groove, ridge, inset, outset and solid, with rounded corners, and adjacent sides of different colours split
    at the CSS corner angle. GeometryBackgroundBorder::DrawBorder stays the fast path for solid borders that need neither.
 */
class GeometryBorderStyles {
public:
	/// True if the border needs this generator: a painted side has a style other than solid, or two painted sides of
	/// different colours meet at a rounded corner.
	static bool IsNeeded(const BorderMetrics& metrics, EdgeSizes edge_sizes, const ColourbPremultiplied colors[4], const BorderStyles& styles);

	/// Generates the border of the metrics' box with the side styles.
	/// @param top_gap The part of the top border that is not drawn: begin (x) and end (y) in px from the left outer edge.
	/// @param edges The edges to feather (anti-aliasing).
	/// @param feather_inner_edge False keeps the border's inner edge sharp (over a background drawn up to it: no seam).
	/// @param feather_unit The size of a device pixel in the geometry's units (1 unless the geometry is drawn scaled).
	static void DrawBorder(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, EdgeSizes edge_sizes,
		const ColourbPremultiplied colors[4], const BorderStyles& styles, Vector2f top_gap = {}, FeatherEdges edges = FeatherEdges::None,
		bool feather_inner_edge = true, float feather_unit = 1.f, bool square_dots = false);

	/// Fills the inner area of the metrics (its corners rounded by the inner radii), the edges feathered as asked.
	/// @param under_border The widths of a border drawn over it: the fill then reaches half a pixel under the border.
	static void DrawBackground(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, ColourbPremultiplied color,
		FeatherEdges edges, const EdgeSizes* under_border = nullptr, float feather_unit = 1.f);

	/// The box a background covering the border box is filled in: the border box, moved in by a pixel under each side
	/// whose border is opaque and without gaps, so that the background's anti-aliased edge does not show around the border
	/// (as Chromium shrinks backgrounds under opaque borders).
	/// @param border_box The element's render box with the border box as its fill.
	/// @param padding_box The render box with the padding box as its fill: its border widths and styles are the element's.
	static RenderBox BackgroundRenderBox(const RenderBox& border_box, const RenderBox& padding_box, const ColourbPremultiplied border_colors[4]);

	/// The colours Chromium draws the dark and light parts of groove, ridge, inset and outset borders with.
	static ColourbPremultiplied DarkColor(ColourbPremultiplied color);
	static ColourbPremultiplied LightColor(ColourbPremultiplied color);
};

} // namespace Rml
