#pragma once

#include "../../Include/RmlUi/Core/RenderBox.h"
#include "GeometryBackgroundBorder.h"

namespace Rml {

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
	static void DrawBorder(Vector<Vertex>& vertices, Vector<int>& indices, const BorderMetrics& metrics, EdgeSizes edge_sizes,
		const ColourbPremultiplied colors[4], const BorderStyles& styles, Vector2f top_gap = {});

	/// The colours Chromium draws the dark and light parts of groove, ridge, inset and outset borders with.
	static ColourbPremultiplied DarkColor(ColourbPremultiplied color);
	static ColourbPremultiplied LightColor(ColourbPremultiplied color);
};

} // namespace Rml
