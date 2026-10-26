#include "../../Include/RmlUi/Core/RenderInterface.h"

namespace Rml {

namespace CoreInternal {
	bool HasRenderManager(RenderInterface* render_interface);
}

RenderInterface::RenderInterface() {}

RenderInterface::~RenderInterface()
{
	// Note: We cannot automatically release render resources here, because that involves a virtual call to this interface during its destruction
	// which is illegal.
	RMLUI_ASSERTMSG(!CoreInternal::HasRenderManager(this),
		"RenderInterface is being destroyed, but it is still actively referenced and used within the RmlUi library. This may lead to use-after-free "
		"or nullptr dereference when releasing render resources. Ensure that the render interface is destroyed *after* the call to Rml::Shutdown.");
}

void RenderInterface::EnableClipMask(bool /*enable*/) {}

void RenderInterface::RenderToClipMask(ClipMaskOperation /*operation*/, CompiledGeometryHandle /*geometry*/, Vector2f /*translation*/) {}

void RenderInterface::RenderToClipMaskShape(ClipMaskOperation operation, CompiledGeometryHandle geometry, Vector2f translation,
	const ClipShape& /*shape*/)
{
	RenderToClipMask(operation, geometry, translation);
}

bool RenderInterface::SupportsPrimitives()
{
	return false;
}

bool RenderInterface::CullsOffscreenPaint()
{
	return false;
}

void RenderInterface::RenderPrimitives(Span<const BoxPrimitive> /*primitives*/, Vector2f /*translation*/) {}

void RenderInterface::SetTransform(const Matrix4f* /*transform*/) {}

LayerHandle RenderInterface::PushLayer()
{
	return {};
}

void RenderInterface::CompositeLayers(LayerHandle /*source*/, LayerHandle /*destination*/, BlendMode /*blend_mode*/,
	Span<const CompiledFilterHandle> /*filters*/)
{}

void RenderInterface::PopLayer() {}

bool RenderInterface::SupportsTextureWrapHint()
{
	return false;
}

TextureHandle RenderInterface::GenerateTextureWithWrap(Span<const byte> source, Vector2i source_dimensions, bool /*repeat_x*/, bool /*repeat_y*/)
{
	return GenerateTexture(source, source_dimensions);
}

TextureHandle RenderInterface::SaveLayerAsTexture()
{
	return TextureHandle{};
}

TextureHandle RenderInterface::SaveLayerIntoTexture(TextureHandle /*texture*/, Vector2i /*texture_dimensions*/, Vector2i /*destination_offset*/)
{
	return TextureHandle{};
}

CompiledFilterHandle RenderInterface::SaveLayerAsMaskImage()
{
	return CompiledFilterHandle{};
}

CompiledFilterHandle RenderInterface::CompileFilter(const String& /*name*/, const Dictionary& /*parameters*/)
{
	return CompiledFilterHandle{};
}

void RenderInterface::ReleaseFilter(CompiledFilterHandle /*filter*/) {}

CompiledShaderHandle RenderInterface::CompileShader(const String& /*name*/, const Dictionary& /*parameters*/)
{
	return CompiledShaderHandle{};
}

void RenderInterface::RenderShader(CompiledShaderHandle /*shader*/, CompiledGeometryHandle /*geometry*/, Vector2f /*translation*/,
	TextureHandle /*texture*/)
{}

void RenderInterface::ReleaseShader(CompiledShaderHandle /*shader*/) {}

} // namespace Rml
