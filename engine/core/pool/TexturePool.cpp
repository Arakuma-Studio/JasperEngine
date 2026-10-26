// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TexturePool.h"
#include "profiler/Profile.h"

#include "Engine.h"
#include "Log.h"
#include "render/SystemRender.h"
#include "texture/TexturePrepare.h"

#include <cstring>

#include <vector>

using namespace jasperengine;

textures_t& TexturePool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static textures_t* map = new textures_t();
    return *map;
};

// Ids marked by invalidate() whose backend texture must be rebuilt. The stale texture is
// kept alive (holders may still bind it) until get() with fresh data swaps it in-place.
std::set<std::string>& TexturePool::getInvalidated(){
    static std::set<std::string>* invalidated = new std::set<std::string>();
    return *invalidated;
};

std::shared_ptr<TextureRender> TexturePool::get(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);

    if (it != map.end() && it->second && it->second->isCreated() && getInvalidated().count(id) == 0){
        return it->second;
    }

	return NULL;
}

std::shared_ptr<TextureRender> TexturePool::get(const std::string& id, TextureType type, const std::shared_ptr<std::array<TextureData,6>> &data, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
	auto& map = getMap();
	auto it = map.find(id);
	std::shared_ptr<TextureRender> shared = (it != map.end()) ? it->second : nullptr;

	bool stale = getInvalidated().count(id) > 0;

	if (shared && shared->isCreated() && !stale){
		return shared;
	}

	if (!data){
		return NULL;
	}

	int numFaces = (type == TextureType::TEXTURE_CUBE) ? 6 : 1;

	// the levels to upload: one TextureData with every face (a cooked file), one per face
	// (raw loads), or plain pixels (setData, model textures) that get their mips here
	std::vector<TextureLevel> levels;
	GpuTextureFormat format = GpuTextureFormat::RGBA8;
	int width = 0;
	int height = 0;
	auto* owned = new std::vector<std::vector<uint8_t>>();
	TextureData& first = data->at(0);
	std::string error;
	if (first.hasLevels() && first.getFaceCount() == numFaces){
		levels = first.getLevels();
		format = first.getGpuFormat();
	}else{
		std::vector<std::vector<TextureLevel>> faceLevels(numFaces);
		bool leveled = true;
		for (int f = 0; f < numFaces; f++){
			TextureData& face = data->at(f);
			leveled = leveled && face.hasLevels() && face.getFaceCount() == 1 && face.getGpuFormat() == first.getGpuFormat() &&
			          face.getLevels().size() == first.getLevels().size();
			if (!face.hasPixels()){
				// Pixel data was released or never populated. Skip the upload so we
				// don't trigger Sokol's VALIDATE_IMAGEDATA_NODATA. The caller will
				// fall back and Texture::load() will refresh the data on a later try.
				Log::error("Texture data is empty: %s", id.c_str());
				delete owned;
				return NULL;
			}
		}
		if (leveled){
			format = first.getGpuFormat();
			for (int f = 0; f < numFaces; f++)
				faceLevels[f] = data->at(f).getLevels();
		}else{
			// mips only when the sampler reads them, as the box filter they replace did
			const bool mipmapped = minFilter == TextureFilter::LINEAR_MIPMAP_LINEAR || minFilter == TextureFilter::LINEAR_MIPMAP_NEAREST ||
			                       minFilter == TextureFilter::NEAREST_MIPMAP_LINEAR || minFilter == TextureFilter::NEAREST_MIPMAP_NEAREST;
			std::vector<const TextureData*> faces;
			for (int f = 0; f < numFaces; f++)
				faces.push_back(&data->at(f));
			TextureProcessing processing = TextureProcessing::defaultFor(first, false, numFaces == 6);
			processing.mipmaps = mipmapped;
			PreparedTexture prepared;
			std::vector<std::vector<uint8_t>> combined;
			if (!TexturePrepare::prepare(faces, processing, prepared, error) ||
			    !TexturePrepare::toUncompressed(prepared, TexturePrepare::uncompressedFormat(prepared.layout), combined, error)){
				Log::error("Texture %s: %s", id.c_str(), error.c_str());
				delete owned;
				return NULL;
			}
			format = TexturePrepare::uncompressedFormat(prepared.layout);
			*owned = std::move(combined);
			for (size_t level = 0; level < owned->size(); level++){
				TextureLevel l;
				l.width = mipDimension(prepared.width, (int)level);
				l.height = mipDimension(prepared.height, (int)level);
				l.data = (*owned)[level].data();
				l.size = (*owned)[level].size();
				levels.push_back(l);
			}
		}
		if (leveled){
			// every face of a level in one block
			const size_t count = faceLevels[0].size();
			owned->resize(count);
			for (size_t level = 0; level < count; level++){
				for (int f = 0; f < numFaces; f++)
					(*owned)[level].insert((*owned)[level].end(), faceLevels[f][level].data, faceLevels[f][level].data + faceLevels[f][level].size);
				TextureLevel l = faceLevels[0][level];
				l.data = (*owned)[level].data();
				l.size = (*owned)[level].size();
				levels.push_back(l);
			}
		}
	}
	// R16 is not a GLES3 / WebGL2 format: those sample its high byte as R8 (the CPU keeps R16)
	if (format == GpuTextureFormat::R16 && !SystemRender::isTextureFormatSupported(GpuTextureFormat::R16)){
		auto* converted = new std::vector<std::vector<uint8_t>>(levels.size());
		for (size_t level = 0; level < levels.size(); level++){
			const size_t n = levels[level].size / 2;
			(*converted)[level].resize(n);
			for (size_t i = 0; i < n; i++){
				uint16_t v;
				std::memcpy(&v, levels[level].data + i * 2, 2);
				(*converted)[level][i] = (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
			}
			levels[level].data = (*converted)[level].data();
			levels[level].size = n;
		}
		delete owned;
		owned = converted;
		format = GpuTextureFormat::R8;
	}
	width = levels.empty() ? 0 : levels[0].width;
	height = levels.empty() ? 0 : levels[0].height;

	if (!shared){
		shared = std::make_shared<TextureRender>();
	}

	// Stale entry with fresh data: swap the backend texture now, inside a single call with
	// no draw in between, so holders never bind a destroyed texture.
	if (stale && shared->isCreated()){
		shared->destroyTexture();
	}

	JE_GPU_ALLOC_SCOPE(Textures, id);
	const bool created = shared->createTextureLevels(id, type, format, width, height, numFaces, levels, minFilter, magFilter, wrapU, wrapV);
	// after the creation: the queued path reads the bytes when the queue runs
	SystemRender::scheduleCleanup([](void* buffers){ delete static_cast<std::vector<std::vector<uint8_t>>*>(buffers); }, owned);
	if (!created){
		shared->destroyTexture();
		if (it != map.end()){
			map.erase(it);
		}
		return NULL;
	}

	// what the image's alpha says, for the users that take the texture from the pool
	bool transparent = false;
	for (int f = 0; f < numFaces; f++){
		transparent = transparent || data->at(f).isTransparent();
	}
	shared->setTransparent(transparent);

	map[id] = shared;
	getInvalidated().erase(id);

	return shared;
}

void TexturePool::add(const std::string& id, std::shared_ptr<TextureRender> render){
	if (!id.empty() && render){
		getMap()[id] = render;
	}
}

void TexturePool::remove(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);
	if (it != map.end()){
		if (!it->second || it->second.use_count() <= 1){
			if (it->second){
				it->second->destroyTexture();
			}
			map.erase(it);
			getInvalidated().erase(id);
		}
	}else{
		if (Engine::isViewLoaded()){
			Log::debug("Trying to destroy a non existent texture: %s", id.c_str());
		}
	}
}

// Unlike remove(), forces a rebuild even while other Texture instances still reference the
// pooled TextureRender. The backend texture is NOT destroyed here: reloading can take
// frames (async file load / SVG rasterization) and holders keep binding the old texture
// meanwhile, so it must stay alive. The id is only marked stale — get() then reports a miss
// so callers reload, and get() with data destroys and recreates the texture in-place inside
// the same shared object, letting every holder pick up the rebuilt texture. Unreferenced
// entries are destroyed and erased immediately, like remove().
void TexturePool::invalidate(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);
	if (it == map.end()){
		return;
	}

	if (!it->second || it->second.use_count() <= 1){
		if (it->second){
			it->second->destroyTexture();
		}
		map.erase(it);
	}else{
		getInvalidated().insert(id);
	}
}

void TexturePool::clear(){
	for (auto& it: getMap()) {
		if (it.second)
			it.second->destroyTexture();
	}
	getMap().clear();
	getInvalidated().clear();
}

void TexturePool::clearUnused(){
	std::vector<std::string> ids;
	ids.reserve(getMap().size());
	for (const auto& [id, _] : getMap()) {
		ids.push_back(id);
	}
	for (const auto& id : ids) {
		remove(id);
	}
}
