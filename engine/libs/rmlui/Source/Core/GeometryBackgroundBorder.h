#pragma once

#include "../../Include/RmlUi/Core/RenderBox.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "../../Include/RmlUi/Core/Vertex.h"

namespace Rml {

class Box;

using CornerPositions = Array<Vector2f, 4>;

// The background-border metrics specify an inner and an outer rectangular area, whose corners can be rounded.
struct BorderMetrics {
	// Outer corner positions (e.g. at border edge)
	CornerPositions positions_outer;
	// Inner corner positions (e.g. at padding edge)
	CornerPositions positions_inner;
	// Curved borders are drawn as ellipses (outer and inner border) around the centers.
	CornerPositions positions_circle_center;

	// Radii of the outer edges, horizontal and vertical (CSS: elliptical corners). Zero for a square corner.
	CornerSizes2 outer_radii;
	// Radii of the inner edges, 2-dimensional as these can be elliptic.
	// The inner radii is effectively the (signed) distance from the circle center to the inner edge.
	// They can also be zero or negative, in which case a sharp corner should be drawn instead of an arc.
	CornerSizes2 inner_radii;
};

class GeometryBackgroundBorder {
public:
	// Construct the background-border geometry drawer, passing in the target vertex and index lists to be filled by later draw operations.
	GeometryBackgroundBorder(Vector<Vertex>& vertices, Vector<int>& indices);

	/// Compute background-border metrics used by later draw operations.
	/// @param outer_position Top-left position of the outer edge.
	/// @param edge_sizes Widths of the border.
	/// @param inner_size Size of the inner area.
	/// @param outer_radii The radii of the outer edge at each corner, resolved and scaled so that they do not overlap
	/// (Element::GetRenderBox; scaled here again if they do).
	/// @return The computed metrics.
	static BorderMetrics ComputeBorderMetrics(Vector2f outer_position, EdgeSizes edge_sizes, Vector2f inner_size, CornerSizes2 outer_radii);

	/// Scales the radii of a box so that adjacent curves do not overlap (CSS Backgrounds 3, 5.5): every radius by the
	/// smallest ratio of a side's length to the sum of the two radii along it.
	static void ScaleRadiiToFit(CornerSizes2& radii, Vector2f box_size);

	/// The number of points of a quarter arc of the given radius: enough for the curve to stay within a tenth of a pixel
	/// of its chords.
	static int GetNumPoints(Vector2f radius);

	// Generate geometry for the background, defined by the inner area of the border metrics.
	void DrawBackground(const BorderMetrics& metrics, ColourbPremultiplied color);

	/// Generate geometry for the border, defined by the intersection of the outer and inner areas of the border metrics.
	/// @param top_gap The part of the top border that is not drawn: begin (x) and end (y) in px from the left outer edge.
	void DrawBorder(const BorderMetrics& metrics, EdgeSizes edge_sizes, const ColourbPremultiplied border_colors[4], Vector2f top_gap = {});

private:
	enum Edge { TOP, RIGHT, BOTTOM, LEFT };
	enum Corner { TOP_LEFT, TOP_RIGHT, BOTTOM_RIGHT, BOTTOM_LEFT };

	// -- Background --
	// All draw operations place vertices in clockwise order.

	// Draw the corner, delegate to the specific corner shape drawing function.
	void DrawBackgroundCorner(Corner corner, Vector2f pos_inner, Vector2f pos_circle_center, Vector2f R, Vector2f r, ColourbPremultiplied color);

	// Add a single point.
	void DrawPoint(Vector2f pos, ColourbPremultiplied color);

	// Draw an arc by placing vertices along the ellipse formed by the two-axis radius r, spaced evenly between angles a0,a1 (inclusive). Colors are
	// interpolated.
	void DrawArc(Vector2f pos_center, Vector2f r, float a0, float a1, ColourbPremultiplied color0, ColourbPremultiplied color1, int num_points);

	// Generates triangles by connecting the added vertices.
	void FillBackground(int index_start);

	// -- Border --
	// All draw operations place the first and last vertices in the following manner.
	// Let N be the number of vertices placed, and the numbers be indices into the newly placed vertices:
	//   0: Inner edge, aligned with the previous corner
	//   1: Outer edge, aligned with the previous corner
	//   N-2: Inner edge, aligned with the next corner
	//   N-1: Outer edge, aligned with the next corner
	// Where 'next' corner means along the clockwise direction. This way we can easily fill the triangles of the edges in FillEdge().

	// Draw the corner, delegate to the specific corner shape drawing function.
	void DrawBorderCorner(Corner corner, Vector2f pos_outer, Vector2f pos_inner, Vector2f pos_circle_center, Vector2f R, Vector2f r,
		ColourbPremultiplied color0, ColourbPremultiplied color1);

	// Draw a sharp border corner, ie. no border-radius. Does not produce any triangles.
	void DrawPointPoint(Vector2f pos_outer, Vector2f pos_inner, ColourbPremultiplied color0, ColourbPremultiplied color1);

	// Draw an arc along the outer edge (radius R), and an arc along the inner edge (two-axis radius r),
	// spaced evenly between angles a0,a1 (inclusive). Connect them by triangles. Colors are interpolated.
	void DrawArcArc(Vector2f pos_center, Vector2f R, Vector2f r, float a0, float a1, ColourbPremultiplied color0, ColourbPremultiplied color1,
		int num_points);

	// Draw an arc along the outer edge, and connect them by triangles to a point on the inner edge.
	void DrawArcPoint(Vector2f pos_center, Vector2f pos_inner, Vector2f R, float a0, float a1, ColourbPremultiplied color0,
		ColourbPremultiplied color1, int num_points);

	// Add triangles between the previous corner to another one specified by the index (possibly yet-to-be-drawn).
	void FillEdge(int index_next_corner);

	Vector<Vertex>& vertices;
	Vector<int>& indices;
};

} // namespace Rml
