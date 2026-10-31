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
	// Fork (S16-F2): the shape the geometry covers (RenderInterface::RenderToClipMaskShape), when has_shape
	bool has_shape = false;
	ClipShape shape = {};
};
inline bool operator==(const ClipMaskGeometry& a, const ClipMaskGeometry& b)
{
	return a.operation == b.operation && a.geometry == b.geometry && a.absolute_offset == b.absolute_offset && a.transform == b.transform &&
		a.has_shape == b.has_shape && (!a.has_shape || a.shape == b.shape);
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

	/// Fork (S16-F1): the render interface draws box primitives (RenderInterface::SupportsPrimitives).
	bool SupportsPrimitives() const;
	/// Fork (S16-F1): draws box primitives at the translation with the current state.
	void RenderPrimitives(Span<const BoxPrimitive> primitives, Vector2f translation);
	/// Fork (S16-F4): whether the rectangle (absolute, before any transform) is drawn wholly outside the scissor region, so that
	/// painting it may be skipped: the render interface culls (RenderInterface::CullsOffscreenPaint), a scissor region is set
	/// and no transform applies. False when unsure.
	bool IsOutsideScissor(Rectanglef rectangle);
	/// Fork (S16-F5): RenderInterface::SupportsGradientPrimitive of the shader, and its handle for the primitive.
	bool SupportsGradientPrimitive(const CompiledShader& shader, CompiledShaderHandle& handle);

	Texture LoadTexture(const String& source, const String& document_path = String());
	/// Fork (phase 3, p3-paint): the texture of 'source' joined with the document path, with 'query' (a query parameter)
	/// appended to the joined path; for RenderInterface::SupportsTextureWrapHint.
	Texture LoadTexture(const String& source, const String& document_path, const String& query);
	bool SupportsTextureWrapHint() const;
	CallbackTexture MakeCallbackTexture(CallbackTextureFunction callback);

	CompiledFilter CompileFilter(const String& name, const Dictionary& parameters);
	CompiledShader CompileShader(const String& name, const Dictionary& parameters);

	/// A layer covering the region of the top layer (Fork, P7-F2: GetLayerRegion).
	LayerHandle PushLayer();
	/// Fork (P7-F2): a layer covering 'region' (window coordinates), which may extend beyond the viewport
	/// (RenderInterface::PushLayerRegion): an ordinary layer when it lies within the viewport or the render interface does
	/// not support it. GetLayerRegion() then returns what it covers.
	LayerHandle PushLayer(Rectanglei region);
	void CompositeLayers(LayerHandle source, LayerHandle destination, BlendMode blend_mode, Span<const CompiledFilterHandle> filters);
	void PopLayer();

	LayerHandle GetTopLayer() const;
	LayerHandle GetNextLayer() const;

	CompiledFilter SaveLayerAsMaskImage();

	/// Fork (S16-F3): something drawn this frame needs another frame although nothing changed (text settling at its
	/// on-screen scale): the application renders the context again. Taken (and cleared) by ConsumeRenderRequest.
	void RequestRender();
	bool ConsumeRenderRequest();

	/// Fork (S16-F6): the clipping regions ElementUtilities::SetClippingRegion found for elements whose own 'clip' is auto:
	/// that of every such element of the same offset parent, kept for the elements after it in the render pass (the last
	/// few offset parents). Forgotten at each pass and when an element's transform is updated during it (ResetClipMemo).
	struct ClipMemo {
		const void* offset_parent = nullptr;
		bool scissor = false;
		Rectanglei region;
		ClipMaskGeometryList masks;
	};
	ClipMemo* FindClipMemo(const void* offset_parent)
	{
		for (ClipMemo& memo : clip_memos)
			if (memo.offset_parent == offset_parent)
				return &memo;
		return nullptr;
	}
	ClipMemo& AddClipMemo(const void* offset_parent)
	{
		ClipMemo& memo = clip_memos[clip_memo_next];
		clip_memo_next = (clip_memo_next + 1) % clip_memo_count;
		memo.offset_parent = offset_parent;
		return memo;
	}
	void ResetClipMemo()
	{
		for (ClipMemo& memo : clip_memos)
			memo.offset_parent = nullptr;
	}
	/// Fork (S16-F6): the paint traversal takes its shortcuts this pass (the clip memo, transforms updated once): while the
	/// render interface culls (RenderInterface::CullsOffscreenPaint), read at PrepareRender.
	bool PaintShortcuts() const { return paint_shortcuts; }
	/// That of the last render pass prepared (elements updating their transform read it without finding their manager).
	static bool LastPaintShortcuts();

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

	/// Fork (P7-F2): the region the top layer covers (window coordinates): the viewport, or the region of a layer pushed
	/// with PushLayer(region) beyond it, and of the layers pushed on it. Scissor regions are clamped to it.
	Rectanglei GetLayerRegion() const;
	/// Fork (P7-F2): a callback texture's content is drawn at the texture's origin into layers the size of the viewport,
	/// whichever layer is the top one when it is generated (used by the texture database around the callback).
	void BeginTextureGeneration();
	void EndTextureGeneration();

private:
	void ApplyClipMask(const ClipMaskGeometryList& clip_elements);
	LayerHandle PushLayerInRegion(Rectanglei region);

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

	bool render_requested = false;
	static constexpr int clip_memo_count = 8;
	ClipMemo clip_memos[clip_memo_count];
	int clip_memo_next = 0;
	// Fork (S16-F6): state.transform is the identity (SetTransform(nullptr) then compares nothing)
	bool transform_identity = true;
	bool paint_shortcuts = false;
	int compiled_filter_count = 0;
	int compiled_shader_count = 0;

	RenderState state;
	Vector2i viewport_dimensions;

	Vector<LayerHandle> render_stack;
	// Fork (P7-F2): the region of each pushed layer, and the viewport for each texture generation in progress (invalid:
	// the viewport).
	Vector<Rectanglei> layer_regions;

	struct ClipRoot {
		const Element* element;
		Rectanglei window;
	};
	Vector<ClipRoot> clip_roots;

	friend class RenderManagerAccess;
};

} // namespace Rml
