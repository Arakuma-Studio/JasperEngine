#pragma once

#include "CallbackTexture.h"
#include "Mesh.h"
#include "RenderInterface.h"
#include "StableVector.h"
#include "Types.h"

namespace Rml {

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

	LayerHandle PushLayer();
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

	friend class RenderManagerAccess;
};

} // namespace Rml
