#pragma once

#include "CallbackTexture.h"
#include "Mesh.h"
#include "RenderInterface.h"
#include "StableVector.h"
#include "Types.h"

namespace Rml {

class Element;
class Geometry;
class CompiledFilter;
class CompiledShader;
class TextureDatabase;
class Texture;
class RenderManagerAccess;

struct ClipMaskGeometry {
	ClipMaskOperation operation;
	Geometry* geometry;
	Vector2f absolute_offset;
	const Matrix4f* transform;
};
inline bool operator==(const ClipMaskGeometry& a, const ClipMaskGeometry& b)
{
	return a.operation == b.operation && a.geometry == b.geometry && a.absolute_offset == b.absolute_offset && a.transform == b.transform;
}
inline bool operator!=(const ClipMaskGeometry& a, const ClipMaskGeometry& b)
{
	return !(a == b);
}
using ClipMaskGeometryList = Vector<ClipMaskGeometry>;

struct RenderState {
	Rectanglei scissor_region = Rectanglei::MakeInvalid();
	ClipMaskGeometryList clip_mask_list;
	Matrix4f transform = Matrix4f::Identity();
};

/**
    A wrapper over the render interface, which tracks its state and resources.

    All operations to be submitted to the render interface should go through this class.
 */
class RMLUICORE_API RenderManager : NonCopyMoveable {
public:
	RenderManager(RenderInterface* render_interface);
	~RenderManager();

	void PrepareRender(Vector2i dimensions);
	void SetViewport(Vector2i dimensions);
	Vector2i GetViewport() const;

	void DisableScissorRegion();
	void SetScissorRegion(Rectanglei region);
	Rectanglei GetScissorRegion() const;

	void DisableClipMask();
	void SetClipMask(ClipMaskGeometryList clip_elements);
	void SetClipMask(ClipMaskOperation operation, Geometry* geometry, Vector2f translation);

	void SetTransform(const Matrix4f* new_transform);

	// Retrieves the cached render state. If setting this state again, ensure the lifetimes of referenced objects are
	// still valid. Possibly invalidating actions include destroying an element, or altering its transform property.
	const RenderState& GetState() const { return state; }
	void SetState(const RenderState& next);
	void ResetState();

	Geometry MakeGeometry(Mesh&& mesh);

	Texture LoadTexture(const String& source, const String& document_path = String());
	/// Fork (phase 3, p3-paint): the texture of 'source' joined with the document path, with 'query' (a query parameter)
	/// appended to the joined path; for RenderInterface::SupportsTextureWrapHint.
	Texture LoadTexture(const String& source, const String& document_path, const String& query);
	bool SupportsTextureWrapHint() const;
	CallbackTexture MakeCallbackTexture(CallbackTextureFunction callback);

	CompiledFilter CompileFilter(const String& name, const Dictionary& parameters);
	CompiledShader CompileShader(const String& name, const Dictionary& parameters);

	LayerHandle PushLayer();
	void CompositeLayers(LayerHandle source, LayerHandle destination, BlendMode blend_mode, Span<const CompiledFilterHandle> filters);
	void PopLayer();

	LayerHandle GetTopLayer() const;
	LayerHandle GetNextLayer() const;

	CompiledFilter SaveLayerAsMaskImage();

	/// Fork (P7-F1): the element whose effects layer (filter, mask, group opacity) is being drawn into the top layer, and
	/// the layer's window. The content drawn into the layer is clipped by the clips established by the element and its
	/// descendants only, and to the window; the clips established outside it clip the composite of the layer (CSS: an
	/// effect applies to the unclipped content, the ancestors' clips to its result). Used by ElementUtilities::
	/// SetClippingRegion; pushed and popped by ElementEffects around the element's layer.
	void PushClipRoot(const Element* element, Rectanglei window);
	void PopClipRoot();
	/// The element of the innermost layer pushed with PushClipRoot, null outside every such layer.
	const Element* GetClipRoot() const { return clip_roots.empty() ? nullptr : clip_roots.back().element; }
	/// Its window (window coordinates), invalid when unbounded.
	Rectanglei GetClipRootWindow() const { return clip_roots.empty() ? Rectanglei::MakeInvalid() : clip_roots.back().window; }

private:
	void ApplyClipMask(const ClipMaskGeometryList& clip_elements);

	StableVectorIndex InsertGeometry(Mesh&& mesh);
	CompiledGeometryHandle GetCompiledGeometryHandle(StableVectorIndex index);

	void Render(const Geometry& geometry, Vector2f translation, Texture texture, const CompiledShader& shader);

	void GetTextureSourceList(StringList& source_list) const;
	const Mesh& GetMesh(const Geometry& geometry) const;

	bool ReleaseTexture(const String& texture_source);
	void ReleaseAllTextures();
	void ReleaseAllCompiledGeometry();

	void ReleaseResource(const CallbackTexture& texture);
	Mesh ReleaseResource(const Geometry& geometry);
	void ReleaseResource(const CompiledFilter& filter);
	void ReleaseResource(const CompiledShader& shader);

	struct GeometryData {
		Mesh mesh;
		CompiledGeometryHandle handle = {};
	};

	RenderInterface* render_interface = nullptr;

	StableVector<GeometryData> geometry_list;
	UniquePtr<TextureDatabase> texture_database;

	int compiled_filter_count = 0;
	int compiled_shader_count = 0;

	RenderState state;
	Vector2i viewport_dimensions;

	Vector<LayerHandle> render_stack;

	struct ClipRoot {
		const Element* element;
		Rectanglei window;
	};
	Vector<ClipRoot> clip_roots;

	friend class RenderManagerAccess;
};

} // namespace Rml
