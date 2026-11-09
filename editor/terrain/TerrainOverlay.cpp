// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainOverlay.h"

#include "Project.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "object/Mesh.h"
#include "object/TerrainEdit.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "terrain/TerrainLayers.h"
#include "texture/TextureData.h"
#include "util/Angle.h"
#include "util/ScopedDefaultEntityPool.h"
#if JASPERENGINE_TERRAIN_WORLD
#include "subsystem/TerrainWorldRuntime.h"
#include "terrainworld/TerrainWorldOverlay.h"
#endif

#include <algorithm>
#include <map>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using terraincore::MapId;
using terraincore::MapKind;

namespace {

    struct OverlaySource {
        TerrainOverlay::Bake bake;
        TerrainOverlay::Legend legend;
    };

    std::map<std::string, OverlaySource>& overlaySources() {
        static std::map<std::string, OverlaySource> sources;
        return sources;
    }

    uint8_t overlayByte(float value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    void overlayPut(std::vector<uint8_t>& rgba, size_t texel, const Vector4& color) {
        rgba[texel * 4 + 0] = overlayByte(color.x);
        rgba[texel * 4 + 1] = overlayByte(color.y);
        rgba[texel * 4 + 2] = overlayByte(color.z);
        rgba[texel * 4 + 3] = overlayByte(color.w);
    }

    // R65's Height ramp, 0 to Terrain Height
    const TerrainOverlayLegendStop HEIGHT_STOPS[] = {
        {"0", Vector4(0.10f, 0.20f, 0.60f, 1.0f)},
        {"25%", Vector4(0.10f, 0.60f, 0.30f, 1.0f)},
        {"50%", Vector4(0.90f, 0.85f, 0.20f, 1.0f)},
        {"75%", Vector4(0.80f, 0.35f, 0.10f, 1.0f)},
        {"100%", Vector4(1.00f, 1.00f, 1.00f, 1.0f)},
    };

    Vector4 heightRamp(float normalized) {
        const float t = std::clamp(normalized, 0.0f, 1.0f) * 4.0f;
        const int i = std::min(3, static_cast<int>(t));
        const float f = t - static_cast<float>(i);
        const Vector4& a = HEIGHT_STOPS[i].color;
        const Vector4& b = HEIGHT_STOPS[i + 1].color;
        return a + (b - a) * f;
    }

    // R65's slope bands
    const Vector4 SLOPE_COLORS[] = {Vector4(0.2f, 0.75f, 0.2f, 1.0f), Vector4(0.95f, 0.85f, 0.15f, 1.0f),
                                    Vector4(0.95f, 0.5f, 0.1f, 1.0f), Vector4(0.9f, 0.15f, 0.1f, 1.0f)};
    const Vector4 SLOPE_IN_RANGE = Vector4(0.9f, 0.15f, 0.1f, 1.0f);

    const Vector4 LAYER_PALETTE[] = {Vector4(0.9f, 0.3f, 0.3f, 1.0f), Vector4(0.3f, 0.8f, 0.3f, 1.0f), Vector4(0.3f, 0.45f, 0.95f, 1.0f),
                                     Vector4(0.95f, 0.85f, 0.2f, 1.0f), Vector4(0.8f, 0.35f, 0.9f, 1.0f), Vector4(0.2f, 0.85f, 0.85f, 1.0f),
                                     Vector4(0.95f, 0.55f, 0.15f, 1.0f), Vector4(0.6f, 0.6f, 0.6f, 1.0f)};
    const Vector4 BASE_COLOR = Vector4(0.15f, 0.15f, 0.15f, 1.0f);

    float slopeDegrees(const Vector3& normal) {
        return Angle::radToDeg(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));
    }

    std::string numberLabel(float value, const char* unit) {
        char text[48];
        std::snprintf(text, sizeof(text), "%g%s", std::round(value * 100.0f) / 100.0f, unit);
        return text;
    }

    // 1, 2 or 5 times a power of ten, about a twentieth of the range
    float contourStep(const TerrainOverlayBake& context) {
        if (context.settings->contourInterval > 0.0f) return context.settings->contourInterval;
        const float range = std::max(0.001f, std::fabs(context.terrain->maxHeight * context.scaleY));
        const float raw = range / 20.0f;
        const float power = std::pow(10.0f, std::floor(std::log10(raw)));
        for (float m : {1.0f, 2.0f, 5.0f, 10.0f}) {
            if (raw <= m * power) return m * power;
        }
        return 10.0f * power;
    }

    void bakeHeight(const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) {
        const terraincore::SurfaceView view = terrainSurfaceView(*context.terrain);
        const float maxHeight = context.terrain->maxHeight;
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                const float h = terraincore::sampleHeight(view, local.x, local.y);
                overlayPut(rgba, static_cast<size_t>(y) * context.width + x, heightRamp(maxHeight != 0.0f ? h / maxHeight : 0.0f));
            }
        }
    }

    std::vector<TerrainOverlayLegendStop> heightLegend(const TerrainOverlayBake& context) {
        std::vector<TerrainOverlayLegendStop> stops;
        for (int i = 0; i < 5; i++) {
            const float world = context.originY + context.terrain->maxHeight * context.scaleY * static_cast<float>(i) / 4.0f;
            stops.push_back({numberLabel(world, " m"), HEIGHT_STOPS[i].color});
        }
        return stops;
    }

    void bakeSlope(const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) {
        const terraincore::SurfaceView view = terrainSurfaceView(*context.terrain);
        const bool ranged = context.settings->slopeMin >= 0.0f && context.settings->slopeMax > context.settings->slopeMin;
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                const float degrees = slopeDegrees(terraincore::sampleSurface(view, local.x, local.y).normal);
                Vector4 color;
                if (ranged) {
                    color = degrees >= context.settings->slopeMin && degrees <= context.settings->slopeMax ? SLOPE_IN_RANGE : Vector4(0, 0, 0, 0);
                } else {
                    color = SLOPE_COLORS[std::min(3, static_cast<int>(degrees / 15.0f))];
                }
                overlayPut(rgba, static_cast<size_t>(y) * context.width + x, color);
            }
        }
    }

    std::vector<TerrainOverlayLegendStop> slopeLegend(const TerrainOverlayBake& context) {
        if (context.settings->slopeMin >= 0.0f && context.settings->slopeMax > context.settings->slopeMin) {
            return {{numberLabel(context.settings->slopeMin, "") + "-" + numberLabel(context.settings->slopeMax, " deg"), SLOPE_IN_RANGE}};
        }
        return {{"0-15 deg", SLOPE_COLORS[0]}, {"15-30 deg", SLOPE_COLORS[1]}, {"30-45 deg", SLOPE_COLORS[2]}, {"over 45 deg", SLOPE_COLORS[3]}};
    }

    void bakeContours(const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) {
        const terraincore::SurfaceView view = terrainSurfaceView(*context.terrain);
        const float step = contourStep(context);
        std::vector<int> band(static_cast<size_t>(context.width) * context.height);
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                const float world = terraincore::sampleHeight(view, local.x, local.y) * context.scaleY + context.originY;
                band[static_cast<size_t>(y) * context.width + x] = static_cast<int>(std::floor(world / step));
            }
        }
        const Vector4 line(0.05f, 0.05f, 0.05f, 1.0f);
        const Vector4 major(0.9f, 0.1f, 0.1f, 1.0f);
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const size_t i = static_cast<size_t>(y) * context.width + x;
                const int b = band[i];
                const int right = x + 1 < context.width ? band[i + 1] : b;
                const int down = y + 1 < context.height ? band[i + context.width] : b;
                const int crossed = std::max(std::max(b, right), down);
                if (crossed != b || right != b || down != b) {
                    overlayPut(rgba, i, crossed % 5 == 0 ? major : line);
                } else {
                    overlayPut(rgba, i, Vector4(0, 0, 0, 0));
                }
            }
        }
    }

    std::vector<TerrainOverlayLegendStop> contourLegend(const TerrainOverlayBake& context) {
        const float step = contourStep(context);
        return {{"every " + numberLabel(step, " m"), Vector4(0.05f, 0.05f, 0.05f, 1.0f)},
                {"every " + numberLabel(step * 5.0f, " m"), Vector4(0.9f, 0.1f, 0.1f, 1.0f)}};
    }

    void bakeLayerWeights(const TerrainOverlayBake& context, std::vector<uint8_t>& rgba) {
        const int layers = static_cast<int>(context.terrain->surfaceLayers.size());
        // each blend map read once, whole
        std::map<int, std::pair<terraincore::MapFormat, std::vector<float>>> maps;
        for (int l = 0; l < layers; l++) {
            const int index = terraincore::terrainLayerChannel(l).map;
            if (maps.count(index)) continue;
            const MapId blend{MapKind::Blend, index};
            const terraincore::MapFormat format = context.store->format(blend);
            if (!format.exists) continue;
            std::vector<float> values(static_cast<size_t>(format.bounds.area()) * format.channels);
            if (context.store->read(blend, format.bounds, values)) maps[index] = {format, std::move(values)};
        }
        for (int y = 0; y < context.height; y++) {
            for (int x = 0; x < context.width; x++) {
                const Vector2 local = context.localOf(x, y);
                Vector4 color(0, 0, 0, 0);
                float total = 0.0f;
                for (int l = 0; l < layers; l++) {
                    if (context.settings->layer >= 0 && context.settings->layer != l) continue;
                    const terraincore::LayerChannel channel = terraincore::terrainLayerChannel(l);
                    auto found = maps.find(channel.map);
                    if (found == maps.end()) continue;
                    const terraincore::MapFormat& format = found->second.first;
                    const Vector2 at = context.store->texelOfWorld(MapId{MapKind::Blend, channel.map}, local.x, local.y);
                    const int bx = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
                    const int by = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
                    float value = found->second.second[(static_cast<size_t>(by) * format.bounds.w + bx) * format.channels + channel.channel];
                    if (channel.inverted) value = 1.0f - value;
                    color = color + LAYER_PALETTE[l % 8] * value;
                    total += value;
                }
                if (context.settings->layer < 0) {
                    color = color + BASE_COLOR * std::max(0.0f, 1.0f - total);
                    color.w = 1.0f;
                } else {
                    color.w = std::clamp(total, 0.0f, 1.0f);
                }
                overlayPut(rgba, static_cast<size_t>(y) * context.width + x, color);
            }
        }
    }

    std::vector<TerrainOverlayLegendStop> layerLegend(const TerrainOverlayBake& context) {
        std::vector<TerrainOverlayLegendStop> stops;
        if (context.settings->layer < 0) stops.push_back({"base", BASE_COLOR});
        for (size_t l = 0; l < context.terrain->surfaceLayers.size(); l++) {
            if (context.settings->layer >= 0 && context.settings->layer != static_cast<int>(l)) continue;
            std::string name = "layer " + std::to_string(l);
            const std::string& path = context.terrain->surfaceLayers[l].colorTexture.getPath();
            if (!path.empty()) name += " (" + path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1) + ")";
            stops.push_back({name, LAYER_PALETTE[l % 8]});
        }
        return stops;
    }

    void ensureBuiltInSources() {
        static std::once_flag once;
        std::call_once(once, []() {
            auto& sources = overlaySources();
            sources.emplace("height", OverlaySource{&bakeHeight, &heightLegend});
            sources.emplace("slope", OverlaySource{&bakeSlope, &slopeLegend});
            sources.emplace("contours", OverlaySource{&bakeContours, &contourLegend});
            sources.emplace("layer_weights", OverlaySource{&bakeLayerWeights, &layerLegend});
        });
    }

    TerrainOverlayBake bakeContext(Scene* scene, Entity entity, TerrainComponent& terrain, const terraincore::TerrainMapStore& store,
                                   const TerrainOverlaySettings& settings, int width, int height) {
        TerrainOverlayBake context;
        context.scene = scene;
        context.entity = entity;
        context.terrain = &terrain;
        context.store = &store;
        context.settings = &settings;
        context.width = width;
        context.height = height;
        const TerrainWorldPlacement placement = terrainWorldPlacement(scene, entity);
        if (placement.valid) {
            context.scaleY = placement.scale.y;
            context.originY = placement.position.y;
        }
        return context;
    }

    // The image's size: the height map's, at most MAX_IMAGE a side
    void overlayImageSize(const terraincore::TerrainMapStore& store, int& width, int& height) {
        const terraincore::MapFormat format = store.format(MapId{MapKind::Height, 0});
        width = std::clamp(format.exists ? format.bounds.w : 256, 2, TerrainOverlay::MAX_IMAGE);
        height = std::clamp(format.exists ? format.bounds.h : 256, 2, TerrainOverlay::MAX_IMAGE);
    }

}

Vector2 TerrainOverlayBake::localOf(int x, int y) const {
    const float size = terrain->terrainSize;
    return Vector2(-size * 0.5f + (static_cast<float>(x) + 0.5f) * size / static_cast<float>(width),
                   -size * 0.5f + (static_cast<float>(y) + 0.5f) * size / static_cast<float>(height));
}

YAML::Node editor::encodeTerrainOverlaySettings(const TerrainOverlaySettings& settings) {
    const TerrainOverlaySettings defaults;
    YAML::Node node;
    if (settings.source != defaults.source) node["source"] = settings.source;
    if (settings.contourInterval != defaults.contourInterval) node["contourInterval"] = settings.contourInterval;
    if (settings.slopeMin != defaults.slopeMin) node["slopeMin"] = settings.slopeMin;
    if (settings.slopeMax != defaults.slopeMax) node["slopeMax"] = settings.slopeMax;
    if (settings.layer != defaults.layer) node["layer"] = settings.layer;
    if (settings.opacity != defaults.opacity) node["opacity"] = settings.opacity;
    return node;
}

void editor::decodeTerrainOverlaySettings(const YAML::Node& node, TerrainOverlaySettings& settings) {
    settings = TerrainOverlaySettings();
    if (!node.IsDefined() || !node.IsMap()) return;
    try {
        if (node["source"]) settings.source = node["source"].as<std::string>();
        if (node["contourInterval"]) settings.contourInterval = std::max(0.0f, node["contourInterval"].as<float>());
        if (node["slopeMin"]) settings.slopeMin = node["slopeMin"].as<float>();
        if (node["slopeMax"]) settings.slopeMax = node["slopeMax"].as<float>();
        if (node["layer"]) settings.layer = node["layer"].as<int>();
        if (node["opacity"]) settings.opacity = std::clamp(node["opacity"].as<float>(), 0.0f, 1.0f);
    } catch (const YAML::Exception&) {
        settings = TerrainOverlaySettings();
    }
}

void TerrainOverlay::registerSource(const std::string& name, Bake bake, Legend legend) {
    ensureBuiltInSources();
    overlaySources()[name] = OverlaySource{std::move(bake), std::move(legend)};
}

bool TerrainOverlay::hasSource(const std::string& name) {
    ensureBuiltInSources();
    return overlaySources().count(name) > 0;
}

std::vector<std::string> TerrainOverlay::sourceNames() {
    ensureBuiltInSources();
    std::vector<std::string> names;
    for (const auto& [name, source] : overlaySources()) names.push_back(name);
    return names;
}

namespace {

    // T1 R67: the previews by terrain, and a count that changes with each, so a sheet bakes again
    struct OverlayPreviews {
        std::map<std::pair<Scene*, Entity>, std::shared_ptr<const terraincore::TerrainMapStore>> stores;
        uint64_t serial = 0;
    };

    OverlayPreviews& overlayPreviews() {
        static OverlayPreviews previews;
        return previews;
    }

}

void TerrainOverlay::setPreview(Scene* scene, Entity entity, std::shared_ptr<const terraincore::TerrainMapStore> store) {
    overlayPreviews().stores[{scene, entity}] = std::move(store);
    overlayPreviews().serial++;
}

void TerrainOverlay::clearPreview(Scene* scene, Entity entity) {
    if (overlayPreviews().stores.erase({scene, entity})) overlayPreviews().serial++;
}

bool TerrainOverlay::hasPreview(Scene* scene, Entity entity) {
    return overlayPreviews().stores.count({scene, entity}) != 0;
}

bool TerrainOverlay::bake(Scene* scene, Entity entity, const TerrainOverlaySettings& settings, int width, int height,
                          std::vector<uint8_t>& rgba, std::vector<TerrainOverlayLegendStop>& legendOut, std::string& error) {
    ensureBuiltInSources();
    auto found = overlaySources().find(settings.source);
    if (found == overlaySources().end()) {
        error = "\"" + settings.source + "\" is not an overlay.";
        return false;
    }
    TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
    if (!terrain) {
        error = "not a terrain";
        return false;
    }
    TerrainComponentStore own(scene, entity, *terrain);
    const auto preview = overlayPreviews().stores.find({scene, entity});
    const terraincore::TerrainMapStore& store = preview != overlayPreviews().stores.end() ? *preview->second : static_cast<const terraincore::TerrainMapStore&>(own);
    if (width <= 0 || height <= 0) overlayImageSize(store, width, height);
    const TerrainOverlayBake context = bakeContext(scene, entity, *terrain, store, settings, width, height);
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    found->second.bake(context, rgba);
    legendOut = found->second.legend ? found->second.legend(context) : std::vector<TerrainOverlayLegendStop>();
    return true;
}

TerrainOverlay::TerrainOverlay(Scene* scene): scene(scene) {
    edits = TerrainEditEvents::subscribe(scene, [this](Entity terrain, MapId, terraincore::TexelRect) {
        auto found = sheets.find(terrain);
        if (found != sheets.end()) found->second.dirty = true;
    });
}

TerrainOverlay::~TerrainOverlay() {
    hide();
}

void TerrainOverlay::hide() {
    for (auto& [entity, sheet] : sheets) {
        releaseWorld(entity);
        delete sheet.mesh;
    }
    sheets.clear();
    legend.clear();
    drawn = TerrainOverlaySettings();
}

void TerrainOverlay::update(SceneProject& sceneProject, const TerrainOverlaySettings& settings, double nowSeconds) {
    bool worldSource = false;
#if JASPERENGINE_TERRAIN_WORLD
    TerrainWorldOverlayMode worldMode;
    worldSource = terrainWorldOverlayMode(settings.source, worldMode);
#endif
    if (settings.source.empty() || (!worldSource && !hasSource(settings.source))) {
        if (!sheets.empty()) hide();
        return;
    }
    const bool changed = settings != drawn || previewsBaked != overlayPreviews().serial;
    previewsBaked = overlayPreviews().serial;
    if (!changed && !worldPending && nowSeconds - lastBake < MIN_SECONDS) return;
    worldPending = false;

    std::set<Entity> present;
    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (size_t i = 0; i < terrains->size(); i++) {
        const Entity entity = terrains->getEntity(i);
        if (std::find(sceneProject.entities.begin(), sceneProject.entities.end(), entity) == sceneProject.entities.end()) continue;
        // a Terrain World draws T4's overlays, a Terrain T1's
        const bool world = scene->findComponent<TerrainWorldComponent>(entity) != nullptr;
        if (world != worldSource) continue;
        present.insert(entity);
        Sheet& sheet = sheets[entity];
        if (world) {
            refreshWorld(entity, sheet, settings, changed);
            continue;
        }
        const TerrainComponent& terrain = terrains->getComponentFromIndex(i);
        if (changed || sheet.dirty || !sheet.mesh || sheet.revision != terrain.surfaceRevision) refresh(entity, sheet, settings);
    }
    for (auto it = sheets.begin(); it != sheets.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        releaseWorld(it->first);
        delete it->second.mesh;
        it = sheets.erase(it);
    }
    drawn = settings;
    lastBake = nowSeconds;
}

void TerrainOverlay::refresh(Entity entity, Sheet& sheet, const TerrainOverlaySettings& settings) {
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    const TerrainWorldPlacement placement = terrainWorldPlacement(scene, entity);
    if (!placement.valid || terrain.terrainSize <= 0.0f) return;

    std::vector<uint8_t> rgba;
    std::string error;
    TerrainComponentStore store(scene, entity, terrain);
    int width = 0, height = 0;
    overlayImageSize(store, width, height);
    if (!bake(scene, entity, settings, width, height, rgba, legend, error)) return;

    ensureMesh(sheet);

    // DC8: a grid over the terrain, each vertex at the highest texel of its footprint plus the lift
    const terraincore::SurfaceView view = terrainSurfaceView(terrain);
    const int cells = std::clamp(view.width > 1 ? view.width - 1 : 64, 1, MAX_CELLS);
    const float size = terrain.terrainSize;
    const float step = size / static_cast<float>(cells);
    const float lift = std::max(0.03f, 0.01f * terrain.maxHeight);
    const int reach = view.width > 1 ? std::max(1, static_cast<int>(std::ceil(static_cast<float>(view.width - 1) / static_cast<float>(cells) * 0.5f))) : 0;
    const float texel = view.width > 1 ? size / static_cast<float>(view.width - 1) : step;

    MeshComponent& mesh = sheet.mesh->getComponent<MeshComponent>();
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;
    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);
    Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);
    const Vector4 tint(1.0f, 1.0f, 1.0f, std::clamp(settings.opacity, 0.0f, 1.0f));
    for (int z = 0; z <= cells; z++) {
        for (int x = 0; x <= cells; x++) {
            const float lx = -size * 0.5f + static_cast<float>(x) * step;
            const float lz = -size * 0.5f + static_cast<float>(z) * step;
            float top = terraincore::sampleHeight(view, lx, lz);
            for (int dz = -reach; dz <= reach; dz++) {
                for (int dx = -reach; dx <= reach; dx++) {
                    top = std::max(top, terraincore::sampleHeight(view, lx + static_cast<float>(dx) * texel, lz + static_cast<float>(dz) * texel));
                }
            }
            mesh.buffer.addVector3(position, placement.model * Vector3(lx, top + lift, lz));
            mesh.buffer.addVector2(texcoord, Vector2(static_cast<float>(x) / static_cast<float>(cells), static_cast<float>(z) / static_cast<float>(cells)));
            mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
            mesh.buffer.addVector4(color, tint);
        }
    }
    std::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(cells) * cells * 6);
    const int row = cells + 1;
    for (int z = 0; z < cells; z++) {
        for (int x = 0; x < cells; x++) {
            const uint16_t a = static_cast<uint16_t>(z * row + x);
            const uint16_t b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + row);
            const uint16_t d = static_cast<uint16_t>(c + 1);
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    mesh.indices.clear();
    mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), reinterpret_cast<char*>(indices.data()), sizeof(uint16_t));
    scene->getSystem<MeshSystem>()->calculateMeshAABB(mesh);
    if (mesh.loaded) mesh.needReload = true;

    setImage(sheet, rgba, width, height);
    sheet.revision = terrain.surfaceRevision;
    sheet.dirty = false;
}

void TerrainOverlay::ensureMesh(Sheet& sheet) {
    if (sheet.mesh) return;
    ScopedDefaultEntityPool sys(*scene, EntityPool::System);
    sheet.mesh = new Mesh(scene);
    scene->getSystem<RenderSystem>()->setEditorOverlay(sheet.mesh->getEntity(), true);
    sheet.mesh->setTransparent(true);
    sheet.mesh->setReceiveLights(false);
    sheet.mesh->setCastShadows(false);
    sheet.mesh->setReceiveShadows(false);
    sheet.mesh->setFaceCulling(false);
}

void TerrainOverlay::setImage(Sheet& sheet, const std::vector<uint8_t>& rgba, int width, int height) {
    MeshComponent& mesh = sheet.mesh->getComponent<MeshComponent>();
    // the image: replaced when its size changes, else written in place
    Texture& image = mesh.submeshes[0].material.baseColorTexture;
    if (sheet.textureId.empty() || sheet.imageWidth != width || sheet.imageHeight != height || !image.hasData()) {
        static std::atomic<uint32_t> nextId{0};
        sheet.textureId = "__terrain_overlay_" + std::to_string(nextId.fetch_add(1));
        const size_t bytes = rgba.size();
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
        if (!raw) return;
        std::memcpy(raw, rgba.data(), bytes);
        TextureData data(width, height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, raw);
        data.setDataOwned(false);
        sheet.mesh->setTexture(sheet.textureId, data);
        image.setReleaseDataAfterLoad(false);
        if (image.hasData() && image.getData().getData() == raw) {
            image.getData().setDataOwned(true);
        } else {
            std::free(raw);
        }
        sheet.imageWidth = width;
        sheet.imageHeight = height;
    } else {
        std::memcpy(image.getData().getData(), rgba.data(), rgba.size());
        image.invalidateRender();
        mesh.submeshes[0].needUpdateTexture = true;
    }
}

void TerrainOverlay::releaseWorld(Entity entity) {
#if JASPERENGINE_TERRAIN_WORLD
    // R37: render keeps the drawn nodes only while an overlay shows them
    TerrainComponent* terrain = scene->isEntityCreated(entity) ? scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (terrain && terrain->world) terrain->world->recordDrawnNodes(false);
#else
    (void)entity;
#endif
}

void TerrainOverlay::refreshWorld(Entity entity, Sheet& sheet, const TerrainOverlaySettings& settings, bool changed) {
#if JASPERENGINE_TERRAIN_WORLD
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    TerrainWorldRuntime* runtime = terrain.world;
    const Transform* transform = scene->findComponent<Transform>(entity);
    TerrainWorldOverlayMode mode;
    if (!runtime || !runtime->started() || !transform || !terrainWorldOverlayMode(settings.source, mode)) return;
    runtime->recordDrawnNodes(mode == TerrainWorldOverlayMode::LodLevel);
    const TerrainWorldOverlayState state = captureTerrainWorldOverlay(*runtime, mode);
    if (!state.valid) return;
    // recording starts with this bake: the cut render keeps from the next frame on
    if (mode == TerrainWorldOverlayMode::LodLevel && state.rects.empty()) worldPending = true;
    // the grid follows the box, the overview and which tiles are exact; the image what it shows
    int exact = 0;
    for (uint8_t tileState : state.states) exact += tileState >= static_cast<uint8_t>(jasperengine::terraincore::TileState::CpuReady) && tileState <= static_cast<uint8_t>(jasperengine::terraincore::TileState::Resident);
    const uint64_t geometry = (static_cast<uint64_t>(static_cast<uint16_t>(state.low.x)) << 48) ^ (static_cast<uint64_t>(static_cast<uint16_t>(state.low.z)) << 32) ^
                              (static_cast<uint64_t>(static_cast<uint16_t>(state.high.x)) << 16) ^ static_cast<uint64_t>(static_cast<uint16_t>(state.high.z)) ^
                              (static_cast<uint64_t>(runtime->overviewRevision()) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(exact) << 24);
    const uint64_t image = terrainWorldOverlaySignature(state);
    const bool newGeometry = !sheet.mesh || sheet.dirty || geometry != sheet.worldGeometry;
    legend = terrainWorldOverlayLegend(state);
    if (!newGeometry && !changed && image == sheet.worldImage) return;

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    bakeTerrainWorldOverlay(state, rgba, width, height);
    ensureMesh(sheet);
    MeshComponent& mesh = sheet.mesh->getComponent<MeshComponent>();
    if (newGeometry || changed) {
        // a grid over the box, each vertex at the highest of the surface round it (overview or exact,
        // never waiting) plus a lift; a World only translates (R4)
        const int cellsX = std::clamp(state.columns() * 8, 1, 128), cellsZ = std::clamp(state.rows() * 8, 1, 128);
        const float minX = static_cast<float>(state.low.x) * state.tileSize, minZ = static_cast<float>(state.low.z) * state.tileSize;
        const float stepX = static_cast<float>(state.columns()) * state.tileSize / static_cast<float>(cellsX);
        const float stepZ = static_cast<float>(state.rows()) * state.tileSize / static_cast<float>(cellsZ);
        const float lift = std::max(0.05f, 0.005f * runtime->maxHeight());
        const Vector3 origin = transform->worldPosition;
        mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
        mesh.numSubmeshes = 1;
        mesh.buffer.clear();
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
        mesh.buffer.addAttribute(AttributeType::COLOR, 4);
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        Attribute* normal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        Attribute* color = mesh.buffer.getAttribute(AttributeType::COLOR);
        const Vector4 tint(1.0f, 1.0f, 1.0f, std::clamp(settings.opacity, 0.0f, 1.0f));
        for (int z = 0; z <= cellsZ; z++) {
            for (int x = 0; x <= cellsX; x++) {
                const float lx = minX + static_cast<float>(x) * stepX, lz = minZ + static_cast<float>(z) * stepZ;
                float top = 0.0f;
                bool any = false;
                for (int dz = -1; dz <= 1; dz++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const jasperengine::terraincore::SurfaceSample sample = runtime->query(lx + 0.5f * stepX * static_cast<float>(dx), lz + 0.5f * stepZ * static_cast<float>(dz), false);
                        if (!sample.present) continue;
                        top = any ? std::max(top, sample.height) : sample.height;
                        any = true;
                    }
                }
                mesh.buffer.addVector3(position, origin + Vector3(lx, top + lift, lz));
                mesh.buffer.addVector2(texcoord, Vector2(static_cast<float>(x) / static_cast<float>(cellsX), static_cast<float>(z) / static_cast<float>(cellsZ)));
                mesh.buffer.addVector3(normal, Vector3(0.0f, 1.0f, 0.0f));
                mesh.buffer.addVector4(color, tint);
            }
        }
        std::vector<uint16_t> indices;
        indices.reserve(static_cast<size_t>(cellsX) * cellsZ * 6);
        const int row = cellsX + 1;
        for (int z = 0; z < cellsZ; z++) {
            for (int x = 0; x < cellsX; x++) {
                const uint16_t a = static_cast<uint16_t>(z * row + x);
                const uint16_t b = static_cast<uint16_t>(a + 1);
                const uint16_t c = static_cast<uint16_t>(a + row);
                const uint16_t d = static_cast<uint16_t>(c + 1);
                indices.insert(indices.end(), {a, c, b, b, c, d});
            }
        }
        mesh.indices.clear();
        mesh.indices.setValues(0, mesh.indices.getAttribute(AttributeType::INDEX), indices.size(), reinterpret_cast<char*>(indices.data()), sizeof(uint16_t));
        scene->getSystem<MeshSystem>()->calculateMeshAABB(mesh);
        if (mesh.loaded) mesh.needReload = true;
        sheet.worldGeometry = geometry;
    }
    setImage(sheet, rgba, width, height);
    sheet.worldImage = image;
    sheet.dirty = false;
#else
    (void)entity;
    (void)sheet;
    (void)settings;
    (void)changed;
#endif
}
