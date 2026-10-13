#pragma once

#include "Header.h"
#include "RenderBox.h"
#include "Types.h"

namespace Rml {

struct Mesh;

/// Feathers every edge of the backgrounds and borders MeshUtilities::GenerateBackgroundBorder generates (anti-aliasing by a
/// one-pixel ring fading to transparent, close to the coverage Chromium computes): on by default, for render targets
/// without multisampling. Off, only curved edges (rounded corners, round dots) are feathered and multisampling resolves the
/// straight ones. Set it before documents generate their geometry; GenerateBackground (clip masks) is never feathered.
RMLUICORE_API void SetGeometryFeathering(bool enable);
RMLUICORE_API bool GetGeometryFeathering();
/// The size of a pixel of the render target in the context's units, for a context drawn scaled into its target (1 by
/// default; 0.5 for a context drawn at twice its size): feathered edges are one target pixel wide. Elements generate their
/// geometry again when it changes.
RMLUICORE_API void SetGeometryPixelSize(float pixel_size);
RMLUICORE_API float GetGeometryPixelSize();

/**
    A class containing helper functions for generating meshes.

 */
class RMLUICORE_API MeshUtilities {
public:
	/// Generates a quad from a position, size and color.
	/// @param[out] mesh A mesh to append the generated vertices and indices into.
	/// @param[in] origin The origin of the quad to generate.
	/// @param[in] dimensions The dimensions of the quad to generate.
	/// @param[in] color The color to be assigned to each of the quad's vertices.
	static void GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied color);
	/// Generates a quad from a position, size, color and texture coordinates.
	/// @param[out] mesh A mesh to append the generated vertices and indices into.
	/// @param[in] origin The origin of the quad to generate.
	/// @param[in] dimensions The dimensions of the quad to generate.
	/// @param[in] color The color to be assigned to each of the quad's vertices.
	/// @param[in] top_left_texcoord The texture coordinates at the top-left of the quad.
	/// @param[in] bottom_right_texcoord The texture coordinates at the bottom-right of the quad.
	static void GenerateQuad(Mesh& mesh, Vector2f origin, Vector2f dimensions, ColourbPremultiplied color, Vector2f top_left_texcoord,
		Vector2f bottom_right_texcoord);

	/// Generates the geometry required to render a line.
	/// @param[out] mesh A mesh to append the generated vertices and indices into.
	/// @param[in] position The top-left position the line.
	/// @param[in] size The size of the line.
	/// @param[in] color The color to draw the line in.
	static void GenerateLine(Mesh& mesh, Vector2f position, Vector2f size, ColourbPremultiplied color);

	/// Generates the geometry for an element's background and border, with support for border-radius.
	/// @param[out] mesh A mesh to append the generated vertices and indices into.
	/// @param[in] render_box The render box which determines the background and border geometry.
	/// @param[in] background_color The color applied to the background, set alpha to zero to not generate the background.
	/// @param[in] border_colors A four-element array of border colors in top-right-bottom-left order.
	/// @note Vertex positions are relative to the border-box, vertex texture coordinates are default initialized. The edges
	/// are feathered unless SetGeometryFeathering(false), and the border is drawn with the render box's border styles.
	static void GenerateBackgroundBorder(Mesh& mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
		const ColourbPremultiplied border_colors[4]);
	/// As above, for geometry drawn scaled: 'feather_unit' is the size of a device pixel in the mesh's units (the inverse of
	/// the scale; the version above takes GetGeometryPixelSize()), so that feathered edges stay one device pixel wide.
	static void GenerateBackgroundBorder(Mesh& mesh, const RenderBox& render_box, ColourbPremultiplied background_color,
		const ColourbPremultiplied border_colors[4], float feather_unit);

	/// Generates the background geometry for an element's area, with support for border-radius.
	/// @param[out] mesh A mesh to append the generated vertices and indices into.
	/// @param[in] render_box The render box which determines the background geometry.
	/// @param[in] color The color applied to the background.
	/// @note Vertex positions are relative to the border-box, vertex texture coordinates are default initialized.
	static void GenerateBackground(Mesh& mesh, const RenderBox& render_box, ColourbPremultiplied color);

private:
	MeshUtilities() = delete;
};

} // namespace Rml
