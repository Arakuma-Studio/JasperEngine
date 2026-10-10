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
	static void DrawBorder(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, EdgeSizes edge_sizes,
		const ColourbPremultiplied colors[4], const BorderStyles& styles, Vector2f top_gap = {}, FeatherEdges edges = FeatherEdges::None,
		bool feather_inner_edge = true);

	/// Fills the inner area of the metrics (its corners rounded by the inner radii), the edges feathered as asked.
	static void DrawBackground(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, ColourbPremultiplied color,
		FeatherEdges edges);

	/// The colours Chromium draws the dark and light parts of groove, ridge, inset and outset borders with.
	static ColourbPremultiplied DarkColor(ColourbPremultiplied color);
	static ColourbPremultiplied LightColor(ColourbPremultiplied color);
};

} // namespace Rml
