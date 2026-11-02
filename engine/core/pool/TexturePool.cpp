// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TexturePool.h"
#include "profiler/Profile.h"

#include "Engine.h"
#include "Log.h"
#include "render/SystemRender.h"
#include "texture/TexturePrepare.h"

#include <atomic>
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

namespace {
    // (0 is never an epoch: RenderSystem keeps 0 for "nothing stood in")
    std::atomic<uint32_t> readyEpoch{1};
    std::atomic<uint32_t> rebindEpoch{1};

    void advance(std::atomic<uint32_t>& epoch){
        if (epoch.fetch_add(1, std::memory_order_acq_rel) + 1 == 0)
            epoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

uint32_t TexturePool::getReadyEpoch(){
    return readyEpoch.load(std::memory_order_acquire);
}

void TexturePool::noteReady(){
    advance(readyEpoch);
}

uint32_t TexturePool::getRebindEpoch(){
    return rebindEpoch.load(std::memory_order_acquire);
}

std::shared_ptr<TextureRender> TexturePool::getKept(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);
	return (it != map.end() && it->second && it->second->isCreated()) ? it->second : nullptr;
}

std::string TexturePool::getRenderId(const std::string& id, TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
	if (id.empty() || (minFilter == TextureFilter::LINEAR && magFilter == TextureFilter::LINEAR &&
	                   wrapU == TextureWrap::REPEAT && wrapV == TextureWrap::REPEAT)){
		return id;
	}
	return id + "|sampler=" + std::to_string((int)minFilter) + "," + std::to_string((int)magFilter) + "," +
	       std::to_string((int)wrapU) + "," + std::to_string((int)wrapV);
}

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
	// float data made in code (a Vertex Animation's offsets, data textures): level 0 as it is,
	// in its own format, through createTexture (its float validation); never the importer's
	// processing, which takes a decoded float file to HDR RGBA16F
	bool rawFloat = false;
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
		rawFloat = !leveled && first.isCodeFloatData();
		if (leveled){
			format = first.getGpuFormat();
			for (int f = 0; f < numFaces; f++)
				faceLevels[f] = data->at(f).getLevels();
		}else if (!rawFloat){
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
	// (R16 where unorm16 does not filter, GLES3 and WebGL2, goes up as float: SokolTexture)
	width = rawFloat ? first.getWidth() : (levels.empty() ? 0 : levels[0].width);
	height = rawFloat ? first.getHeight() : (levels.empty() ? 0 : levels[0].height);

	if (!shared){
		shared = std::make_shared<TextureRender>();
	}

	// Stale entry with fresh data: swap the backend texture now, inside a single call with
	// no draw in between. The bindings that hold its view bind again (getRebindEpoch).
	const bool rebuilt = stale && shared->isCreated();
	if (rebuilt){
		shared->destroyTexture();
	}

	JE_GPU_ALLOC_SCOPE(Textures, id);
	bool created = false;
	if (rawFloat){
		void* faceData[6] = {};
		size_t faceSizes[6] = {};
		for (int f = 0; f < numFaces; f++){
			faceData[f] = data->at(f).getData();
			faceSizes[f] = (size_t)data->at(f).getSize();
		}
		created = shared->createTexture(id, width, height, first.getColorFormat(), type, numFaces, faceData, faceSizes, minFilter, magFilter, wrapU, wrapV);
	}else{
		created = shared->createTextureLevels(id, type, format, width, height, numFaces, levels, minFilter, magFilter, wrapU, wrapV);
	}
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
	shared->setStale(false);
	if (rebuilt){
		shared->noteRebuilt();
		advance(rebindEpoch);
	}
	advance(readyEpoch);

	return shared;
}

bool TexturePool::holds(const std::string& id, const std::shared_ptr<TextureRender>& render){
	auto& map = getMap();
	auto it = map.find(id);
	return render && it != map.end() && it->second == render;
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
// entries are destroyed and erased immediately, like remove(). The id's sampler variants
// (getRenderId) hold the same image, so they are invalidated with it.
void TexturePool::invalidate(const std::string& id){
	auto& map = getMap();

	std::vector<std::string> keys = {id};
	const std::string prefix = id + "|sampler=";
	for (auto it = map.lower_bound(prefix); it != map.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it){
		keys.push_back(it->first);
	}

	for (const std::string& key : keys){
		auto it = map.find(key);
		if (it == map.end()){
			continue;
		}

		if (!it->second || it->second.use_count() <= 1){
			if (it->second){
				it->second->destroyTexture();
			}
			map.erase(it);
		}else{
			getInvalidated().insert(key);
			it->second->setStale(true);
			advance(rebindEpoch);
		}
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
