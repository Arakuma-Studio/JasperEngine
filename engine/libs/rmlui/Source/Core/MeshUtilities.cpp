#include "../../Include/RmlUi/Core/MeshUtilities.h"
#include "../../Include/RmlUi/Core/Box.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Types.h"
#include "GeometryBackgroundBorder.h"
#include "GeometryBorderStyles.h"

namespace Rml {

static bool geometry_feathering = true;
static float geometry_pixel_size = 1.f;

void SetGeometryFeathering(bool enable)
{
	geometry_feathering = enable;
}

bool GetGeometryFeathering()
{
	return geometry_feathering;
}

void SetGeometryPixelSize(float pixel_size)
{
	geometry_pixel_size = (pixel_size > 0.f ? pixel_size : 1.f);
}

float GetGeometryPixelSize()
{
	return geometry_pixel_size;
}

void MeshUtilities::GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied colour)
{
	GenerateQuad(mesh, origin, dimensions, colour, Vector2f(0, 0), Vector2f(1, 1));
}

void MeshUtilities::GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied colour, Vector2f top_left_texcoord,
	Vector2f bottom_right_texcoord)
{
	const int v0 = (int)mesh.vertices.size();
	const int i0 = (int)mesh.indices.size();

	mesh.vertices.resize(mesh.vertices.size() + 4);
	mesh.indices.resize(mesh.indices.size() + 6);
	Vertex* vertices = mesh.vertices.data();
	int* indices = mesh.indices.data();

	vertices[v0 + 0].position = origin;
	vertices[v0 + 0].colour = colour;
	vertices[v0 + 0].tex_coord = top_left_texcoord;

	vertices[v0 + 1].position = Vector2f(origin.x + dimensions.x, origin.y);
	vertices[v0 + 1].colour = colour;
	vertices[v0 + 1].tex_coord = Vector2f(bottom_right_texcoord.x, top_left_texcoord.y);

	vertices[v0 + 2].position = origin + dimensions;
	vertices[v0 + 2].colour = colour;
	vertices[v0 + 2].tex_coord = bottom_right_texcoord;

	vertices[v0 + 3].position = Vector2f(origin.x, origin.y + dimensions.y);
	vertices[v0 + 3].colour = colour;
	vertices[v0 + 3].tex_coord = Vector2f(top_left_texcoord.x, bottom_right_texcoord.y);

	indices[i0 + 0] = v0 + 0;
	indices[i0 + 1] = v0 + 3;
	indices[i0 + 2] = v0 + 1;

	indices[i0 + 3] = v0 + 1;
	indices[i0 + 4] = v0 + 3;
	indices[i0 + 5] = v0 + 2;
}

void MeshUtilities::GenerateLine(Mesh& mesh, Vector2f position, Vector2f size, ColourbPremultiplied color)
{
	Math::SnapToPixelGrid(position, size);
	MeshUtilities::GenerateQuad(mesh, position, size, color);
}

void MeshUtilities::GenerateBackgroundBorder(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
	const ColourbPremultiplied border_colors[4])
{
	GenerateBackgroundBorder(out_mesh, render_box, background_color, border_colors, geometry_pixel_size);
}

void MeshUtilities::GenerateBackgroundBorder(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
	const ColourbPremultiplied border_colors[4], float feather_unit)
{
	RMLUI_ASSERT(border_colors);

	Vector<Vertex>& vertices = out_mesh.vertices;
	Vector<int>& indices = out_mesh.indices;

	const EdgeSizes& border_widths = render_box.GetBorderWidths();
	int num_borders = 0;
	for (int i = 0; i < 4; i++)
		if (border_colors[i].alpha > 0 && border_widths[i] > 0)
			num_borders += 1;

	const Vector2f fill_size = render_box.GetFillSize();
	const bool has_background = (background_color.alpha > 0 && fill_size.x > 0 && fill_size.y > 0);
	const bool has_border = (num_borders > 0);

	if (!has_background && !has_border)
		return;

	// Reserve geometry. A conservative estimate, does not take border-radii into account and assumes same-colored borders.
	const int estimated_num_vertices = 4 * int(has_background) + 2 * num_borders;
	const int estimated_num_triangles = 2 * int(has_background) + 2 * num_borders;
	vertices.reserve((int)vertices.size() + estimated_num_vertices);
	indices.reserve((int)indices.size() + 3 * estimated_num_triangles);

	// Generate the geometry.
	GeometryBackgroundBorder geometry(vertices, indices);
	const BorderMetrics metrics =
		GeometryBackgroundBorder::ComputeBorderMetrics(render_box.GetBorderOffset(), border_widths, fill_size, render_box.GetBorderRadius());
	const BorderStyles border_styles = render_box.GetBorderStyles();

	// Anti-aliasing (phase 3, 3.7.3): with feathering every edge of the background and the border is feathered; without
	// it (a multisampled target) only curved edges are, multisampling resolving the straight ones. Square, solid borders
	// without feathering keep the fast path.
	bool rounded = false;
	for (const Vector2f& radius : metrics.outer_radii)
		rounded |= (radius.x > 0.f && radius.y > 0.f);
	const FeatherEdges edges = (geometry_feathering ? FeatherEdges::All : FeatherEdges::Curves);

	if (has_background)
	{
		// A background inside a border and the border's inner edge stay sharp where they meet, so that no seam shows.
		if (geometry_feathering || rounded)
		{
			// The sides whose border is drawn (the background reaches a little under them).
			EdgeSizes drawn_widths = border_widths;
			for (int i = 0; i < 4; i++)
			{
				if (border_colors[i].alpha == 0 || !Style::IsBorderStylePainted(border_styles[i]))
					drawn_widths[i] = 0.f;
			}
			GeometryBorderStyles::DrawBackground(vertices, indices, metrics, background_color, has_border ? FeatherEdges::None : edges,
				has_border ? &drawn_widths : nullptr, feather_unit);
		}
		else
			geometry.DrawBackground(metrics, background_color);
	}

	if (has_border)
	{
		// Styles other than solid, and sides of different colours meeting at a rounded corner, need the styled generator.
		if (geometry_feathering || rounded || GeometryBorderStyles::IsNeeded(metrics, border_widths, border_colors, border_styles))
			GeometryBorderStyles::DrawBorder(vertices, indices, metrics, border_widths, border_colors, border_styles, render_box.GetBorderTopGap(),
				edges, !has_background, feather_unit);
		else
			geometry.DrawBorder(metrics, border_widths, border_colors, render_box.GetBorderTopGap());
	}

#if 0
	// Debug draw vertices
	if (render_box.border_radius != CornerSizes{})
	{
		const int num_vertices = (int)vertices.size();
		const int num_indices = (int)indices.size();
		vertices.reserve(num_vertices + 4 * num_vertices);
		indices.reserve(num_indices + 6 * num_indices);

		for (int i = 0; i < num_vertices; i++)
			MeshUtilities::GenerateQuad(out_mesh, vertices[i].position, Vector2f(3, 3), ColourbPremultiplied(255, 0, (i % 2) == 0 ? 0 : 255));
	}
#endif

#ifdef RMLUI_DEBUG
	const int num_vertices = (int)vertices.size();
	for (int index : indices)
	{
		RMLUI_ASSERT(index < num_vertices);
	}
#endif
}

void MeshUtilities::GenerateBackground(Mesh& out_mesh, const RenderBox& render_box, ColourbPremultiplied color)
{
	const Vector2f fill_size = render_box.GetFillSize();
	const bool has_background = (color.alpha > 0 && fill_size.x > 0 && fill_size.y > 0);
	if (!has_background)
		return;

	const BorderMetrics metrics = GeometryBackgroundBorder::ComputeBorderMetrics(render_box.GetBorderOffset(), render_box.GetBorderWidths(),
		fill_size, render_box.GetBorderRadius());

	Vector<Vertex>& vertices = out_mesh.vertices;
	Vector<int>& indices = out_mesh.indices;

	// Reserve geometry. A conservative estimate, does not take border-radii into account.
	vertices.reserve((int)vertices.size() + 4);
	indices.reserve((int)indices.size() + 6);

	// Generate the geometry
	GeometryBackgroundBorder geometry(vertices, indices);
	geometry.DrawBackground(metrics, color);
}

} // namespace Rml
