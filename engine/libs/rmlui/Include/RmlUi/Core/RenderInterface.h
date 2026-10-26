#pragma once

#include "Header.h"
#include "Traits.h"
#include "Types.h"
#include "Vertex.h"

namespace Rml {

enum class ClipMaskOperation {
	Set,        // Set the clip mask to the area of the rendered geometry, clearing any existing clip mask.
	SetInverse, // Set the clip mask to the area *outside* the rendered geometry, clearing any existing clip mask.
	Intersect,  // Intersect the clip mask with the area of the rendered geometry.
};
enum class BlendMode {
	Blend,   // Normal alpha blending.
	Replace, // Replace the destination colors from the source.
	Plus,    // Add the source to the destination (premultiplied, CSS 'plus-lighter'): cross-fade().
};

/// Fork (S16-F2): the shape a clip mask's geometry covers, a rectangle with elliptical corners, so that a render interface
/// can clip without drawing the mask (a scissor, or an analytic clip in its shaders).
struct ClipShape {
	Rectanglef rect;                  // relative to the mask's translation, before the transform
	Array<Vector2f, 4> radii = {};    // top-left, top-right, bottom-right, bottom-left: horizontal (x) and vertical (y); a corner
	                                  // with a radius <= 0 is square
	const Matrix4f* transform = nullptr; // the transform the mask is drawn with (nullptr: none), valid during the call only
};
inline bool operator==(const ClipShape& a, const ClipShape& b)
{
	return a.rect.TopLeft() == b.rect.TopLeft() && a.rect.BottomRight() == b.rect.BottomRight() && a.radii == b.radii &&
		a.transform == b.transform;
}

/// Fork (S16-F1): a box drawn from its signed distance field instead of a mesh (RenderInterface::RenderPrimitives): a
/// background with its solid border, an outline ring, or a box shadow. Rectangles are relative to the translation, before
/// the transform; radii are top-left, top-right, bottom-right, bottom-left (x horizontal, y vertical; <= 0 is square).
struct BoxPrimitive {
	enum class Kind : uint8_t {
		Box,         // border_box filled with 'fill', its border (border_widths) in border_colors
		ShadowOuter, // the shadow shape border_box in 'fill', blurred by sigma, outside clip_box (the element's border box)
		ShadowInset, // everything outside the shadow shape border_box in 'fill', blurred by sigma, inside clip_box (the padding box)
	};
	Kind kind = Kind::Box;
	Rectanglef border_box;               // Box: the border box; shadows: the shadow's shape (offset and spread applied)
	Array<Vector2f, 4> radii = {};
	Vector4f border_widths;              // Box: top, right, bottom, left
	Vector4f fill_insets;                // Box: the fill's edges inside border_box (top, right, bottom, left), its radii reduced
	                                     // by them (the mesh path's background under opaque borders)
	ColourbPremultiplied fill;
	Array<ColourbPremultiplied, 4> border_colors = {}; // Box: top, right, bottom, left
	float sigma = 0.f;                   // shadows: half the blur radius
	Rectanglef clip_box;                 // shadows
	Array<Vector2f, 4> clip_radii = {};
};

/**
    The abstract base class for application-specific rendering implementation. Your application must provide a concrete
    implementation of this class and install it through Rml::SetRenderInterface() in order for anything to be rendered.
 */

class RMLUICORE_API RenderInterface : public NonCopyMoveable {
public:
	RenderInterface();
	virtual ~RenderInterface();

	/**
	    @name Required functions for basic rendering.
	 */

	/// Called by RmlUi when it wants to compile geometry to be rendered later.
	/// @param[in] vertices The geometry's vertex data.
	/// @param[in] indices The geometry's index data.
	/// @return An application-specified handle to the geometry, or zero if it could not be compiled.
	/// @lifetime The pointed-to vertex and index data are guaranteed to be valid and immutable until ReleaseGeometry()
	/// is called with the geometry handle returned here.
	virtual CompiledGeometryHandle CompileGeometry(Span<const Vertex> vertices, Span<const int> indices) = 0;
	/// Called by RmlUi when it wants to render geometry.
	/// @param[in] geometry The geometry to render.
	/// @param[in] translation The translation to apply to the geometry.
	/// @param[in] texture The texture to be applied to the geometry, or zero if the geometry is untextured.
	virtual void RenderGeometry(CompiledGeometryHandle geometry, Vector2f translation, TextureHandle texture) = 0;
	/// Called by RmlUi when it wants to release geometry.
	/// @param[in] geometry The geometry to release.
	virtual void ReleaseGeometry(CompiledGeometryHandle geometry) = 0;

	/// Called by RmlUi when a texture is required by the library.
	/// @param[out] texture_dimensions The dimensions of the loaded texture, which must be set by the application.
	/// @param[in] source The application-defined image source, joined with the path of the referencing document.
	/// @return An application-specified handle identifying the texture, or zero if it could not be loaded.
	virtual TextureHandle LoadTexture(Vector2i& texture_dimensions, const String& source) = 0;
	/// Called by RmlUi when a texture is required to be generated from a sequence of pixels in memory.
	/// @param[in] source The raw texture data. Each pixel is made up of four 8-bit values, red, green, blue, and premultiplied alpha, in that order.
	/// @param[in] source_dimensions The dimensions, in pixels, of the source data.
	/// @return An application-specified handle identifying the texture, or zero if it could not be generated.
	virtual TextureHandle GenerateTexture(Span<const byte> source, Vector2i source_dimensions) = 0;
	/// Called by RmlUi when a loaded or generated texture is no longer required.
	/// @param[in] texture The texture handle to release.
	virtual void ReleaseTexture(TextureHandle texture) = 0;

	/// Fork (phase 3, p3-paint): true when LoadTexture understands a source whose query ends in the parameter
	/// 'rmlui-wrap=repeat', '=repeat-x' or '=repeat-y': it strips the parameter and samples that texture with repeat wrap
	/// along those axes (clamped along the other). Background and mask images repeated along an axis are then drawn as one
	/// quad, filtered across the seams of their tiles as browsers do; without it they are drawn tile by tile.
	virtual bool SupportsTextureWrapHint();
	/// Fork (phase 3, p3-integration): GenerateTexture for a texture sampled with repeat wrap along the axes asked for
	/// (clamped along the others), when SupportsTextureWrapHint() is true: an image the application renders at the size of
	/// a background tile (an SVG) repeats as one quad, as LoadTexture's 'rmlui-wrap' textures. By default the texture of
	/// GenerateTexture (clamped).
	virtual TextureHandle GenerateTextureWithWrap(Span<const byte> source, Vector2i source_dimensions, bool repeat_x, bool repeat_y);

	/// Called by RmlUi when it wants to enable or disable scissoring to clip content.
	/// @param[in] enable True to enable scissoring, false to disable it.
	virtual void EnableScissorRegion(bool enable) = 0;
	/// Called by RmlUi when it wants to change the scissor region.
	/// @param[in] region The region to be rendered. All pixels outside this region should be clipped.
	/// @note The region should be applied in window coordinates regardless of any active transform.
	virtual void SetScissorRegion(Rectanglei region) = 0;

	/**
	    @name Optional functions for advanced rendering features.
	 */

	/// Called by RmlUi when it wants to enable or disable the clip mask.
	/// @param[in] enable True to enable the clip mask, false to disable it.
	virtual void EnableClipMask(bool enable);
	/// Called by RmlUi when it wants to set or modify the contents of the clip mask.
	/// @param[in] operation Describes how the geometry should affect the clip mask.
	/// @param[in] geometry The compiled geometry to render.
	/// @param[in] translation The translation to apply to the geometry.
	/// @note When enabled, the clip mask should hide any rendered contents outside the area of the mask.
	/// @note The clip mask applies exclusively to all other functions that render with a geometry handle, in addition
	/// to the `CompositeLayers` function while rendering to its destination.
	virtual void RenderToClipMask(ClipMaskOperation operation, CompiledGeometryHandle geometry, Vector2f translation);
	/// Fork (S16-F2): the same with the shape the geometry covers (exactly: the geometry is the shape's fill). RmlUi calls it
	/// instead of RenderToClipMask for the clip masks of overflow and replaced content; the default forwards to
	/// RenderToClipMask.
	virtual void RenderToClipMaskShape(ClipMaskOperation operation, CompiledGeometryHandle geometry, Vector2f translation,
		const ClipShape& shape);

	/// Fork (S16-F1): whether RenderPrimitives draws boxes: elements then send their solid backgrounds, borders, outlines
	/// and box shadows as primitives instead of meshes and shadow textures. Asked when the geometry is generated.
	virtual bool SupportsPrimitives();
	/// Fork (S16-F1): draws the primitives in order, like RenderGeometry (the scissor, clip mask and transform apply).
	virtual void RenderPrimitives(Span<const BoxPrimitive> primitives, Vector2f translation);
	/// Fork (S16-F4): whether elements may skip painting their background, border and text when it lies wholly outside the
	/// scissor region (no transform): what the render interface would clip anyway. Asked every frame.
	virtual bool CullsOffscreenPaint();

	/// Called by RmlUi when it wants the renderer to use a new transform matrix.
	/// @param[in] transform The new transform to apply, or nullptr if no transform applies to the current element.
	/// @note When nullptr is submitted, the renderer should use an identity transform matrix or otherwise omit the
	/// multiplication with the transform.
	/// @note The transform applies to all functions that render with a geometry handle, and only those.
	virtual void SetTransform(const Matrix4f* transform);

	/// Called by RmlUi when it wants to push a new layer onto the render stack, setting it as the new render target.
	/// @return An application-specified handle representing the new layer. The value 'zero' is reserved for the initial base layer.
	/// @note The new layer should be initialized to transparent black within the current scissor region.
	virtual LayerHandle PushLayer();
	/// Composite two layers with the given blend mode and apply filters.
	/// @param[in] source The source layer.
	/// @param[in] destination The destination layer.
	/// @param[in] blend_mode The mode used to blend the source layer onto the destination layer.
	/// @param[in] filters A list of compiled filters which should be applied before blending.
	/// @note Source and destination can reference the same layer.
	virtual void CompositeLayers(LayerHandle source, LayerHandle destination, BlendMode blend_mode, Span<const CompiledFilterHandle> filters);
	/// Called by RmlUi when it wants to pop the render layer stack, setting the new top layer as the render target.
	virtual void PopLayer();

	/// Called by RmlUi when it wants to store the current layer as a new texture to be rendered later with geometry.
	/// @return An application-specified handle to the new texture.
	/// @note The texture should be extracted using the bounds defined by the active scissor region, thereby matching its size.
	virtual TextureHandle SaveLayerAsTexture();
	/// Fork: called by RmlUi when it wants to store the current layer as one part (tile) of a texture larger than the
	/// render target, e.g. a box shadow larger than the window: the active scissor region of the layer is copied to
	/// 'destination_offset' in 'texture'. With a zero 'texture', a transparent texture of 'texture_dimensions' is made
	/// first and returned; the later tiles go into it.
	/// @return The texture written to, or zero when not supported (the default): the caller then saves the layer with
	/// SaveLayerAsTexture, cut by the render target.
	virtual TextureHandle SaveLayerIntoTexture(TextureHandle texture, Vector2i texture_dimensions, Vector2i destination_offset);

	/// Called by RmlUi when it wants to store the current layer as a mask image, to be applied later as a filter.
	/// @return An application-specified handle to a new filter representing the stored mask image.
	virtual CompiledFilterHandle SaveLayerAsMaskImage();

	/// Called by RmlUi when it wants to compile a new filter.
	/// @param[in] name The name of the filter.
	/// @param[in] parameters The list of name-value parameters specified for the filter.
	/// @return An application-specified handle representing the compiled filter.
	virtual CompiledFilterHandle CompileFilter(const String& name, const Dictionary& parameters);
	/// Called by RmlUi when it no longer needs a previously compiled filter.
	/// @param[in] filter The handle to a previously compiled filter.
	virtual void ReleaseFilter(CompiledFilterHandle filter);

	/// Called by RmlUi when it wants to compile a new shader.
	/// @param[in] name The name of the shader.
	/// @param[in] parameters The list of name-value parameters specified for the filter.
	/// @return An application-specified handle representing the shader.
	virtual CompiledShaderHandle CompileShader(const String& name, const Dictionary& parameters);
	/// Called by RmlUi when it wants to render geometry using the given shader.
	/// @param[in] shader The handle to a previously compiled shader.
	/// @param[in] geometry The handle to a previously compiled geometry.
	/// @param[in] translation The translation to apply to the geometry.
	/// @param[in] texture The texture to use when rendering the geometry, or zero for no texture.
	virtual void RenderShader(CompiledShaderHandle shader, CompiledGeometryHandle geometry, Vector2f translation, TextureHandle texture);
	/// Called by RmlUi when it no longer needs a previously compiled shader.
	/// @param[in] shader The handle to a previously compiled shader.
	virtual void ReleaseShader(CompiledShaderHandle shader);
};

} // namespace Rml
