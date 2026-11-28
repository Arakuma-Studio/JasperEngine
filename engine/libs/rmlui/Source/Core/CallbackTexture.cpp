#include "../../Include/RmlUi/Core/CallbackTexture.h"
#include "../../Include/RmlUi/Core/Texture.h"
#include "RenderManagerAccess.h"
#include <unordered_set>

namespace Rml {

void CallbackTexture::Release()
{
	if (resource_handle != StableVectorIndex::Invalid)
	{
		RenderManagerAccess::ReleaseResource(render_manager, *this);
		Clear();
	}
}

Rml::CallbackTexture::operator Texture() const
{
	return Texture(render_manager, resource_handle);
}

CallbackTextureInterface::CallbackTextureInterface(RenderManager& render_manager, RenderInterface& render_interface, TextureHandle& texture_handle,
	Vector2i& dimensions) : render_manager(render_manager), render_interface(render_interface), texture_handle(texture_handle), dimensions(dimensions)
{}

bool CallbackTextureInterface::GenerateTexture(Span<const byte> source, Vector2i new_dimensions) const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return false;
	}
	texture_handle = render_interface.GenerateTexture(source, new_dimensions);
	if (texture_handle)
		dimensions = new_dimensions;
	return texture_handle != TextureHandle{};
}

bool CallbackTextureInterface::GenerateTexture(Span<const byte> source, Vector2i new_dimensions, bool repeat_x, bool repeat_y) const
{
	if (!repeat_x && !repeat_y)
		return GenerateTexture(source, new_dimensions);
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return false;
	}
	texture_handle = render_interface.GenerateTextureWithWrap(source, new_dimensions, repeat_x, repeat_y);
	if (texture_handle)
		dimensions = new_dimensions;
	return texture_handle != TextureHandle{};
}

void CallbackTextureInterface::SaveLayerAsTexture() const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return;
	}

	const Rectanglei region = render_manager.GetScissorRegion();
	if (!region.Valid())
	{
		RMLUI_ERRORMSG("Save layer as texture requires a scissor region to be set first");
		return;
	}

	texture_handle = render_interface.SaveLayerAsTexture();
	if (texture_handle)
		dimensions = region.Size();
}

bool CallbackTextureInterface::SaveLayerAsTextureTile(Vector2i new_dimensions, Vector2i offset) const
{
	if (texture_handle && dimensions != new_dimensions)
	{
		RMLUI_ERRORMSG("Texture already set");
		return false;
	}
	if (!render_manager.GetScissorRegion().Valid())
	{
		RMLUI_ERRORMSG("Save layer as texture requires a scissor region to be set first");
		return false;
	}

	const TextureHandle result = render_interface.SaveLayerIntoTexture(texture_handle, new_dimensions, offset);
	if (!result)
		return false;
	texture_handle = result;
	dimensions = new_dimensions;
	return true;
}

void CallbackTextureInterface::SetTextureHandle(TextureHandle handle, Vector2i new_dimensions) const
{
	if (texture_handle)
	{
		RMLUI_ERRORMSG("Texture already set");
		return;
	}

	texture_handle = handle;
	dimensions = new_dimensions;
}

RenderManager& CallbackTextureInterface::GetRenderManager() const
{
	return render_manager;
}

namespace {
	// Fork (S93 R13): the live sources (main thread, as every render resource); never destroyed, as sources held by statics
	// may go after it
	std::unordered_set<const CallbackTextureSource*>& LiveSources()
	{
		static auto* sources = new std::unordered_set<const CallbackTextureSource*>();
		return *sources;
	}
} // namespace

CallbackTextureSource::CallbackTextureSource()
{
	LiveSources().insert(this);
}

CallbackTextureSource::CallbackTextureSource(CallbackTextureFunction&& callback) : callback(std::move(callback))
{
	LiveSources().insert(this);
}

CallbackTextureSource::~CallbackTextureSource()
{
	LiveSources().erase(this);
}

CallbackTextureSource::CallbackTextureSource(CallbackTextureSource&& other) noexcept :
	callback(std::move(other.callback)), textures(std::move(other.textures))
{
	LiveSources().insert(this);
}

CallbackTextureSource& CallbackTextureSource::operator=(CallbackTextureSource&& other) noexcept
{
	callback = std::move(other.callback);
	textures = std::move(other.textures);
	return *this;
}

Texture CallbackTextureSource::GetTexture(RenderManager& render_manager) const
{
	CallbackTexture& texture = textures[&render_manager];
	if (!texture)
		texture = render_manager.MakeCallbackTexture(callback);
	return Texture(texture);
}

void CallbackTextureSource::ReleaseRenderManager(RenderManager* render_manager)
{
	// (a copy: a released texture's render interface may destroy a source)
	const std::unordered_set<const CallbackTextureSource*> sources = LiveSources();
	for (const CallbackTextureSource* source : sources)
	{
		if (LiveSources().count(source))
			source->textures.erase(render_manager);
	}
}

} // namespace Rml
