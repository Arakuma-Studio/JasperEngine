// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainEdit.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Matrix4.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    TextureData* loadedPixels(const Texture& texture) {
        if (texture.empty() || texture.isFramebuffer() || !texture.hasData()) return nullptr;
        TextureData& data = texture.getData();
        return data.getData() ? &data : nullptr;
    }

    // A hole map is R8 (S55 R52). Any other pixels draw no holes, as a map that fails to load does.
    void fillHoles(SurfaceView& view, TerrainComponent& terrain) {
        if (terrain.holeMap.empty()) return;
        TextureData* pixels = loadedPixels(terrain.holeMap);
        if (!pixels || pixels->getChannels() != 1 || TextureData::getBytesPerChannel(pixels->getColorFormat()) != 1) return;
        view.holes.texels = static_cast<const uint8_t*>(pixels->getData());
        view.holes.width = pixels->getWidth();
        view.holes.height = pixels->getHeight();
    }

    // What the terrain's maps may be for a store that writes them: 8-bit or 16-bit unsigned
    bool writableFormat(ColorFormat format) {
        return format == ColorFormat::RED || format == ColorFormat::RED16 || format == ColorFormat::RGBA;
    }

    Texture* mapTexture(TerrainComponent& terrain, MapId map) {
        switch (map.kind) {
            case MapKind::Height:
                return &terrain.heightMap;
            case MapKind::Hole:
                return &terrain.holeMap;
            case MapKind::Blend:
                return map.index >= 0 && map.index < static_cast<int>(terrain.blendMaps.size()) ? &terrain.blendMaps[map.index] : nullptr;
            case MapKind::Density:
                return map.index >= 0 && map.index < static_cast<int>(terrain.foliageLayers.size()) ? &terrain.foliageLayers[map.index].densityMap : nullptr;
        }
        return nullptr;
    }

    // What a map change means to the terrain until node refit replaces it (S53 R23): the flags and
    // the GPU image a brush stroke sets today
    void markMapChanged(TerrainComponent& terrain, MapId map) {
        switch (map.kind) {
            case MapKind::Height:
                terrain.heightMapLoaded = false;
                terrain.needUpdateTerrain = true;
                terrain.needUpdateTexture = true;
                terrain.needUpdateFoliage = true;
                break;
            case MapKind::Blend:
            case MapKind::Hole:
                terrain.needUpdateTexture = true;
                // T3 R26: a hole takes its foliage with it, and a filled one gives it back
                if (map.kind == MapKind::Hole) {
                    terrain.needUpdateFoliage = true;
                    terrain.holeMapRevision++;
                }
                break;
            case MapKind::Density:
                terrain.needUpdateFoliage = true;
                break;
        }
        if (Texture* texture = mapTexture(terrain, map)) {
            texture->invalidateRender();
        }
    }

    // ---------------------------------------------------------------- the events

    using EventSignal = Signal<const Scene*, Entity, MapId, TexelRect>;

    struct PendingEvent {
        const Scene* scene;
        Entity terrain;
        MapId map;
        TexelRect rect;
    };

    struct EventState {
        EventSignal signal;
        std::vector<PendingEvent> pending;
    };

    // Made by the first subscriber, so an unedited game holds nothing
    EventState* eventState = nullptr;
    bool eventsPending = false;

    EventState& ensureEventState() {
        // Lives to the process's end: subscriptions and scenes go away in any order at exit
        if (!eventState) eventState = new EventState();
        return *eventState;
    }

}

SurfaceView jasperengine::terrainSurfaceView(TerrainComponent& terrain) {
    SurfaceView view;
    view.size = terrain.terrainSize;
    view.maxHeight = terrain.maxHeight;
    if (TextureData* pixels = loadedPixels(terrain.heightMap)) {
        view.pixels = static_cast<const uint8_t*>(pixels->getData());
        view.width = pixels->getWidth();
        view.height = pixels->getHeight();
        view.channels = pixels->getChannels();
        view.bytesPerChannel = TextureData::getBytesPerChannel(pixels->getColorFormat());
    }
    fillHoles(view, terrain);
    return view;
}

ResourceLoadState jasperengine::prepareTerrainHoleMap(TerrainComponent& terrain) {
    Texture& map = terrain.holeMap;
    if (map.empty()) return ResourceLoadState::Finished;
    map.setReleaseDataAfterLoad(false);
    if (map.getMinFilter() != TextureFilter::NEAREST) map.setMinFilter(TextureFilter::NEAREST);
    if (map.getMagFilter() != TextureFilter::NEAREST) map.setMagFilter(TextureFilter::NEAREST);
    if (map.getWrapU() != TextureWrap::CLAMP_TO_EDGE) map.setWrapU(TextureWrap::CLAMP_TO_EDGE);
    if (map.getWrapV() != TextureWrap::CLAMP_TO_EDGE) map.setWrapV(TextureWrap::CLAMP_TO_EDGE);
    return map.load().state;
}

SurfaceSample jasperengine::querySurface(TerrainComponent& terrain, float localX, float localZ) {
    return sampleSurface(terrainSurfaceView(terrain), localX, localZ);
}

bool jasperengine::raycastTerrain(TerrainComponent& terrain, const Transform& transform, const Ray& ray, bool skipHoles, Vector3& worldPoint) {
    // A ray may be the first to want the pixels, so it asks for them; it is on the main thread
    if (!terrain.heightMap.empty() && !terrain.heightMap.isFramebuffer()) {
        terrain.heightMap.setReleaseDataAfterLoad(false);
        terrain.heightMap.load();
    }
    prepareTerrainHoleMap(terrain);
    const SurfaceView view = terrainSurfaceView(terrain);

    const Matrix4 inverseModel = transform.modelMatrix.inverse();
    const Vector3 localOrigin = inverseModel * ray.getOrigin();
    const Vector3 localEnd = inverseModel * (ray.getOrigin() + ray.getDirection());

    Vector3 localPoint;
    float t = 0.0f;
    if (!raycastSurface(view, localOrigin, localEnd - localOrigin, 1.0f, skipHoles, localPoint, t)) {
        return false;
    }
    worldPoint = transform.modelMatrix * localPoint;
    return true;
}

SurfaceSample TerrainComponentQuery::sample(float localX, float localZ) const {
    return querySurface(*component, localX, localZ);
}

float TerrainComponentQuery::heightAt(float localX, float localZ) const {
    return sampleHeight(terrainSurfaceView(*component), localX, localZ);
}

bool TerrainComponentQuery::raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const {
    return raycastSurface(terrainSurfaceView(*component), origin, direction, maxT, skipHoles, point, t);
}

void jasperengine::snapshotTerrainSurface(TerrainComponent& terrain, TerrainSurfaceSnapshot& out) {
    const SurfaceView live = terrainSurfaceView(terrain);
    out.view = live;
    out.pixels.clear();
    out.holes.clear();
    if (live.pixels) {
        const size_t bytes = static_cast<size_t>(live.width) * live.height * live.channels * live.bytesPerChannel;
        out.pixels.assign(live.pixels, live.pixels + bytes);
        out.view.pixels = out.pixels.data();
    }
    if (live.holes.texels) {
        out.holes.assign(live.holes.texels, live.holes.texels + static_cast<size_t>(live.holes.width) * live.holes.height);
        out.view.holes.texels = out.holes.data();
    }
}

void jasperengine::applyNewTerrainDefaults(TerrainComponent& terrain) {
    // T3 R53: a new terrain keeps the strongest layers of a pixel; the struct default stays off, so an
    // old terrain draws what it drew. T5's F12 adds its own here.
    terrain.limitLayersPerPixel = true;
}

void jasperengine::applyLayerOrder(TerrainComponent& terrain, std::span<const int> order) {
    if (order.empty() || order.size() != terrain.surfaceLayers.size()) return;
    std::vector<TerrainSurfaceLayer> moved(terrain.surfaceLayers.size());
    for (size_t layer = 0; layer < order.size(); layer++) {
        moved[order[layer]] = terrain.surfaceLayers[layer];
    }
    terrain.surfaceLayers = std::move(moved);
    terrain.needUpdateTexture = true;
}

Subscription TerrainEditEvents::subscribe(Handler handler) {
    return ensureEventState().signal.subscribe([handler = std::move(handler)](const Scene*, Entity terrain, MapId map, TexelRect rect) {
        handler(terrain, map, rect);
    });
}

Subscription TerrainEditEvents::subscribe(const Scene* scene, Handler handler) {
    return ensureEventState().signal.subscribe([scene, handler = std::move(handler)](const Scene* from, Entity terrain, MapId map, TexelRect rect) {
        if (from == scene) handler(terrain, map, rect);
    });
}

void TerrainEditEvents::publish(const Scene* scene, Entity terrain, MapId map, TexelRect rect) {
    if (!eventState || rect.empty() || eventState->signal.empty()) return;
    for (PendingEvent& pending : eventState->pending) {
        if (pending.scene == scene && pending.terrain == terrain && pending.map == map) {
            pending.rect = unite(pending.rect, rect);
            return;
        }
    }
    eventState->pending.push_back(PendingEvent{scene, terrain, map, rect});
    eventsPending = true;
}

bool TerrainEditEvents::hasPending() {
    return eventsPending;
}

void TerrainEditEvents::flush(const Scene* scene) {
    if (!eventState || !eventsPending) return;
    // A handler may edit: what it writes waits for the next flush
    std::vector<PendingEvent> delivering;
    std::vector<PendingEvent>& pending = eventState->pending;
    for (size_t i = 0; i < pending.size();) {
        if (pending[i].scene == scene) {
            delivering.push_back(pending[i]);
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    eventsPending = !pending.empty();
    for (const PendingEvent& event : delivering) {
        eventState->signal.invoke(event.scene, event.terrain, event.map, event.rect);
    }
}

void TerrainEditEvents::forgetScene(const Scene* scene) {
    if (!eventState || !eventsPending) return;
    std::vector<PendingEvent>& pending = eventState->pending;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PendingEvent& event) { return event.scene == scene; }), pending.end());
    eventsPending = !pending.empty();
}

TerrainComponentStore::TerrainComponentStore(Scene* scene, Entity entity, TerrainComponent& terrain)
    : scene(scene), entity(entity), terrain(terrain), bytes(terrain.terrainSize, terrain.maxHeight) {}

bool TerrainComponentStore::sync(MapId map) const {
    Texture* texture = mapTexture(terrain, map);
    if (!texture || texture->empty() || texture->isFramebuffer()) return false;
    if (!texture->hasData()) {
        texture->setReleaseDataAfterLoad(false);
        texture->load();
    }
    if (!texture->hasData()) return false;
    TextureData& data = texture->getData();
    if (!data.getData() || !writableFormat(data.getColorFormat())) return false;
    const int bits = TextureData::getBytesPerChannel(data.getColorFormat()) * 8;
    bytes.borrowMap(map, data.getWidth(), data.getHeight(), data.getChannels(), bits, static_cast<uint8_t*>(data.getData()));
    return true;
}

void TerrainComponentStore::afterWrite() {
    std::vector<std::pair<MapId, TexelRect>> changed;
    bytes.takeDirty(changed);
    for (const auto& [id, rect] : changed) {
        markMapChanged(terrain, id);
        bool merged = false;
        for (auto& [keptId, keptRect] : dirty) {
            if (keptId == id) {
                keptRect = unite(keptRect, rect);
                merged = true;
            }
        }
        if (!merged) dirty.emplace_back(id, rect);
        TerrainEditEvents::publish(scene, entity, id, rect);
    }
}

TerrainFrame TerrainComponentStore::frame() const {
    return TerrainFrame{terrain.maxHeight};
}

MapFormat TerrainComponentStore::format(MapId map) const {
    return sync(map) ? bytes.format(map) : MapFormat{};
}

bool TerrainComponentStore::read(MapId map, TexelRect rect, std::span<float> out) const {
    return sync(map) && bytes.read(map, rect, out);
}

WriteResult TerrainComponentStore::write(MapId map, TexelRect rect, std::span<const float> in) {
    if (!sync(map)) return WriteResult{false, "the terrain has no such map loaded", 0};
    const WriteResult result = bytes.write(map, rect, in);
    afterWrite();
    return result;
}

bool TerrainComponentStore::readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const {
    return sync(map) && bytes.readBytes(map, rect, out);
}

WriteResult TerrainComponentStore::writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) {
    if (!sync(map)) return WriteResult{false, "the terrain has no such map loaded", 0};
    const WriteResult result = bytes.writeBytes(map, rect, in);
    afterWrite();
    return result;
}

bool TerrainComponentStore::canWrite(MapId map, TexelRect rect, std::string* why) const {
    if (!sync(map)) {
        if (why) *why = "the terrain has no such map loaded";
        return false;
    }
    return bytes.canWrite(map, rect, why);
}

bool TerrainComponentStore::canCreateMap(MapId map, std::string* why) const {
    if (map.kind == MapKind::Density && (map.index < 0 || map.index >= static_cast<int>(terrain.foliageLayers.size()))) {
        if (why) *why = "foliage layer " + std::to_string(map.index) + " does not exist: the terrain has " +
                        std::to_string(terrain.foliageLayers.size()) + " foliage layer(s).";
        return false;
    }
    if (map.kind == MapKind::Blend && (map.index < 0 || map.index >= MAX_TERRAIN_BLENDMAPS)) {
        if (why) *why = "a terrain holds at most " + std::to_string(MAX_TERRAIN_BLENDMAPS) + " blend maps.";
        return false;
    }
    return true;
}

WriteResult TerrainComponentStore::createMap(MapId map, MapFormat newFormat, float fill) {
    std::string why;
    if (!canCreateMap(map, &why)) return WriteResult{false, why, 0};
    if (format(map).exists) return WriteResult{false, "map already exists", 0};
    if (newFormat.bounds.empty() || newFormat.channels <= 0) return WriteResult{false, "empty map", 0};

    if (map.kind == MapKind::Blend && map.index >= static_cast<int>(terrain.blendMaps.size())) {
        terrain.blendMaps.resize(map.index + 1);
    }
    Texture* texture = mapTexture(terrain, map);
    if (!texture) return WriteResult{false, "no such map", 0};

    // The pixels this store fills; an editor store seeds a file instead (EditorTerrainStore)
    const int bytesPerChannel = newFormat.bitsPerChannel >= 16 ? 2 : 1;
    const size_t size = static_cast<size_t>(newFormat.bounds.area()) * newFormat.channels * bytesPerChannel;
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(size));
    if (!raw) return WriteResult{false, "out of memory", 0};
    const uint32_t quantized = quantizeChannel(fill, newFormat.bitsPerChannel);
    const uint32_t opaque = newFormat.bitsPerChannel >= 16 ? 65535u : 255u;
    const size_t texels = static_cast<size_t>(newFormat.bounds.area());
    for (size_t i = 0; i < texels; i++) {
        uint8_t* stored = raw + i * newFormat.channels * bytesPerChannel;
        for (int c = 0; c < newFormat.channels; c++) {
            encodeChannel(stored + c * bytesPerChannel, c == 3 ? opaque : quantized, newFormat.bitsPerChannel);
        }
    }

    static std::atomic<uint32_t> nextId{0};
    const std::string id = "__terrain_map_" + std::to_string(nextId.fetch_add(1)) + "_" + std::to_string(entity);
    const ColorFormat colorFormat = newFormat.channels == 1 ? (bytesPerChannel == 2 ? ColorFormat::RED16 : ColorFormat::RED) : ColorFormat::RGBA;
    TextureData seed(newFormat.bounds.w, newFormat.bounds.h, static_cast<unsigned int>(size), colorFormat, newFormat.channels, raw);
    seed.setDataOwned(false);
    texture->setData(id, seed);
    texture->setReleaseDataAfterLoad(false);
    if (texture->hasData() && texture->getData().getData() == raw) {
        texture->getData().setDataOwned(true);
    } else {
        std::free(raw);
        return WriteResult{false, "the new map could not be stored", 0};
    }

    markMapChanged(terrain, map);
    TerrainEditEvents::publish(scene, entity, map, format(map).bounds);
    return WriteResult{true, "", 0};
}

bool TerrainComponentStore::canRemoveMap(MapId map) const {
    // T3 R29: while a volume holds it, a last fill keeps the map, all solid
    return map.kind == MapKind::Hole && !terrain.holeMap.empty() && terrain.holeMapHolds == 0;
}

WriteResult TerrainComponentStore::removeMap(MapId map) {
    if (!canRemoveMap(map)) return WriteResult{false, "this map stays with the terrain", 0};
    const MapFormat before = format(map);
    terrain.holeMap = Texture();
    terrain.needUpdateTexture = true;
    terrain.needUpdateFoliage = true;
    terrain.holeMapRevision++;
    TerrainEditEvents::publish(scene, entity, map, before.bounds);
    return WriteResult{true, "", 0};
}

void TerrainComponentStore::takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) {
    out.insert(out.end(), dirty.begin(), dirty.end());
    dirty.clear();
}

Vector2 TerrainComponentStore::worldOfTexel(MapId map, int x, int y) const {
    sync(map);
    bytes.setFrame(terrain.terrainSize, terrain.maxHeight);
    return bytes.worldOfTexel(map, x, y);
}

Vector2 TerrainComponentStore::texelOfWorld(MapId map, float localX, float localZ) const {
    sync(map);
    bytes.setFrame(terrain.terrainSize, terrain.maxHeight);
    return bytes.texelOfWorld(map, localX, localZ);
}

TexelRect TerrainComponentStore::preferredBlock(MapId map) const {
    return sync(map) ? bytes.preferredBlock(map) : TexelRect{};
}

std::unique_ptr<TerrainLargeEdit> TerrainComponentStore::beginLarge(MapId, TexelRect) {
    return nullptr;
}

std::unique_ptr<TerrainStoreUndoHook> TerrainComponentStore::takeUndoHook() {
    return nullptr;
}
