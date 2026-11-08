// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T4 R7-R13, D4-D6, D14: a Terrain World's draws. A page per resident tile (height as 16 bits in RG8,
// read with texelFetch on every backend, its blend and hole maps), the overview as one height and one
// macro colour texture, each view's selection over the whole world, and the draw items the colour,
// depth and G-buffer passes bind one after the other on the terrain's two grids. RenderSystem.cpp
// only branches here.

#include "RenderSystem.h"

#include "subsystem/TerrainWorldRender.h"

using namespace jasperengine;

#if JASPERENGINE_TERRAIN_WORLD

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "component/Transform.h"
#include "profiler/Profile.h"
#include "render/TextureRender.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"

#include <algorithm>
#include <cstring>
#include <map>

using namespace jasperengine::terrain;

struct TerrainWorldRender::Impl {
    struct Page {
        TextureRender height;
        TextureRender blend[MAX_TERRAIN_BLENDMAPS];
        bool blendMade[MAX_TERRAIN_BLENDMAPS] = {};
        TextureRender holes;
        bool holesMade = false;
        uint64_t bytes = 0;
    };

    struct Item {
        bool overview = false;
        TileCoord tile;
        uint32_t first[2] = {0, 0};
        uint32_t count[2] = {0, 0};
    };

    struct View {
        std::vector<Item> items;
    };

    std::map<uint32_t, std::unique_ptr<Page>> pages;
    TextureRender overviewHeight;
    TextureRender overviewColour;
    uint32_t overviewRev = UINT32_MAX;
    bool overviewMade = false;
    std::map<const TerrainView*, View> views;

    ~Impl() {
        for (auto& [key, page] : pages) destroyPage(*page);
        overviewHeight.destroyTexture();
        overviewColour.destroyTexture();
    }

    static void destroyPage(Page& page) {
        page.height.destroyTexture();
        for (TextureRender& blend : page.blend) blend.destroyTexture();
        page.holes.destroyTexture();
    }
};

TerrainWorldRender::TerrainWorldRender(TerrainWorldRuntime& runtime): impl(std::make_unique<Impl>()) {
    (void)runtime;
}

TerrainWorldRender::~TerrainWorldRender() = default;

bool jasperengine::terrainWorldHasHoles(const TerrainComponent& terrain) {
    return terrain.world && terrain.world->manifest().maps.hasHoles;
}

namespace {

    // D3: big-endian 16-bit samples in RG8 rows, from a map of little-endian ones (gutter included)
    void heightsToRG8(const TileMap& height, std::vector<uint8_t>& out) {
        out.resize(static_cast<size_t>(height.width) * height.height * 2);
        for (size_t i = 0; i < static_cast<size_t>(height.width) * height.height; i++) {
            const uint16_t value = static_cast<uint16_t>(height.bytes[i * 2] | (height.bytes[i * 2 + 1] << 8));
            encodeHeightRG8(value, out.data() + i * 2);
        }
    }

    bool makeTexture(TextureRender& texture, const char* label, GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size,
                     TextureFilter filter) {
        texture.destroyTexture();
        const TextureLevel level{width, height, data, size};
        return texture.createTextureLevels(label, TextureType::TEXTURE_2D, format, width, height, 1, std::span<const TextureLevel>(&level, 1), filter,
                                           filter, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE);
    }

    // the 20 floats of u_vs_terrainParams with HAS_TERRAIN_WORLD: the block's 8, then the page's
    struct WorldTerrainBlock {
        float eyeAndTerrain[8];
        float worldPage[4];
        float worldPageSize[4];
        float worldTile[4];
    };

}

void RenderSystem::updateTerrainWorldPages(Entity entity, TerrainComponent& terrain){
    TerrainWorldRuntime* runtime = terrain.world;
    if (!runtime || !runtime->started())
        return;
    if (!runtime->render)
        runtime->render = std::make_unique<TerrainWorldRender>(*runtime);
    TerrainWorldRender::Impl& state = *runtime->render->impl;
    JE_PROFILE_SCOPE(TerrainWorldSystem::PROFILE_SCOPE);
    bool changed = false;

    // the overview, made again whole when it changes (R52: sg_update_image cannot patch part of it)
    const WorldOverview& overview = runtime->getOverview();
    if (state.overviewRev != runtime->overviewRevision()){
        state.overviewRev = runtime->overviewRevision();
        state.overviewMade = false;
        if (!overview.empty()){
            std::vector<uint8_t> rg(overview.heights.size() * 2);
            for (size_t i = 0; i < overview.heights.size(); i++)
                encodeHeightRG8(overview.heights[i], rg.data() + i * 2);
            JE_GPU_ALLOC_SCOPE(Terrain, "Terrain World overview");
            state.overviewMade = makeTexture(state.overviewHeight, "terrain-world-overview", GpuTextureFormat::RG8, overview.width(), overview.height(),
                                             rg.data(), rg.size(), TextureFilter::NEAREST) &&
                                 makeTexture(state.overviewColour, "terrain-world-macro", GpuTextureFormat::RGBA8, overview.width(), overview.height(),
                                             overview.colour.data(), overview.colour.size(), TextureFilter::LINEAR);
        }
        changed = true;
    }

    // pages of tiles that lost theirs (evicted, removed) go
    TileStreamer& streamer = runtime->tiles();
    for (auto it = state.pages.begin(); it != state.pages.end();){
        const TileState tileState = streamer.state(TileCoord::fromKey(it->first));
        if (tileState != TileState::Resident && tileState != TileState::Uploading){
            TerrainWorldRender::Impl::destroyPage(*it->second);
            it = state.pages.erase(it);
            changed = true;
        }else{
            ++it;
        }
    }

    // R13: this frame's uploads within the byte budget, at least one; a tile draws once all its
    // images are up (spec Deviations: immutable images on every backend)
    const WorldLattice& lattice = runtime->lattice();
    for (const TileCoord& tile : streamer.planUploads()){
        std::shared_ptr<const WorldTile> data = streamer.data(tile);
        if (!data || data->height.empty()){
            streamer.uploadRefused(tile);
            continue;
        }
        std::unique_ptr<TerrainWorldRender::Impl::Page>& slot = state.pages[tile.key()];
        auto page = std::make_unique<TerrainWorldRender::Impl::Page>();
        JE_GPU_ALLOC_SCOPE(Terrain, "Terrain World pages");
        std::vector<uint8_t> rg;
        heightsToRG8(data->height, rg);
        bool ok = makeTexture(page->height, "terrain-world-page", GpuTextureFormat::RG8, data->height.width, data->height.height, rg.data(), rg.size(),
                              TextureFilter::NEAREST);
        page->bytes = rg.size();
        for (int m = 0; m < MAX_TERRAIN_BLENDMAPS && m < static_cast<int>(data->blend.size()) && ok; m++){
            const TileMap& blend = data->blend[m];
            if (blend.empty())
                continue;
            ok = makeTexture(page->blend[m], "terrain-world-blend", GpuTextureFormat::RGBA8, blend.width, blend.height, blend.bytes.data(), blend.bytes.size(),
                             TextureFilter::LINEAR);
            page->blendMade[m] = ok;
            page->bytes += blend.bytes.size();
        }
        if (ok && !data->holes.empty()){
            ok = makeTexture(page->holes, "terrain-world-holes", GpuTextureFormat::R8, data->holes.width, data->holes.height, data->holes.bytes.data(),
                             data->holes.bytes.size(), TextureFilter::NEAREST);
            page->holesMade = ok;
            page->bytes += data->holes.bytes.size();
        }
        if (!ok){
            TerrainWorldRender::Impl::destroyPage(*page);
            streamer.uploadRefused(tile);
            continue;
        }
        if (slot)
            TerrainWorldRender::Impl::destroyPage(*slot);
        const uint64_t bytes = page->bytes;
        slot = std::move(page);
        streamer.uploaded(tile, bytes);
        changed = true;
    }
    (void)lattice;
    // D9: what draws changed, so the next update selects again. Flagged here rather than by the pump,
    // which skips frames closer than its dedupe: those would draw the old selection, or none
    if (changed){
        if (Transform* transform = scene->findComponent<Transform>(entity))
            transform->needUpdate = true;
    }
}

void RenderSystem::selectTerrainWorld(TerrainComponent& terrain, const Transform& transform, CameraComponent& camera, const Transform& cameraTransform, TerrainView& view){
    TerrainWorldRuntime* runtime = terrain.world;
    for (int s = 0; s < 2; s++){
        view.nodesbuffer[s].clear();
        view.nodeIndices[s].clear();
    }
    view.needUpdateNodesBuffer = true;
    view.nodesEyePos = cameraTransform.worldPosition;
    if (!runtime || !runtime->started())
        return;
    if (!runtime->render)
        runtime->render = std::make_unique<TerrainWorldRender>(*runtime);
    TerrainWorldRender::Impl& state = *runtime->render->impl;
    TerrainWorldRender::Impl::View& viewState = state.views[&view];
    viewState.items.clear();
    // R10: before the overview exists the world draws nothing
    if (!state.overviewMade)
        return;
    JE_PROFILE_SCOPE(TerrainWorldSystem::PROFILE_SCOPE);

    const Vector3 origin = transform.worldPosition;
    WorldSelectParams params;
    params.lattice = runtime->lattice();
    params.maxHeight = runtime->maxHeight();
    params.lodDistance = runtime->tiles().budgets().lodDistance;
    params.farClip = camera.farClip;
    params.eyeX = cameraTransform.worldPosition.x - origin.x;
    params.eyeY = cameraTransform.worldPosition.y - origin.y;
    params.eyeZ = cameraTransform.worldPosition.z - origin.z;
    for (int p = 0; p < 6; p++){
        const Plane& plane = camera.frustumPlanes[p];
        // the far plane of a camera without one, as isInsideCamera skips it
        if (p == FRUSTUM_PLANE_FAR && camera.farClip == 0)
            continue;
        // world to the entity's local space: a World only translates (R4)
        params.planes[p][0] = plane.normal.x;
        params.planes[p][1] = plane.normal.y;
        params.planes[p][2] = plane.normal.z;
        params.planes[p][3] = plane.d + plane.normal.dotProduct(origin);
    }
    params.maxNearTiles = terrainWorldPlatformBudget().nearTiles;
    params.maxNodesPerGrid = static_cast<int>(TERRAIN_WORLD_MAX_NODES);
    params.page = [&](TileCoord tile) -> const WorldTile* {
        const auto page = state.pages.find(tile.key());
        if (page == state.pages.end() || runtime->tiles().state(tile) != TileState::Resident)
            return nullptr;
        return runtime->resident(tile).get();
    };
    WorldSelection selection;
    selectWorld(params, runtime->occupancy(), selection);

    TerrainWorldStats& stats = runtime->mutableStats();
    stats.nodesVisited = selection.visited;
    stats.nodesSelected = selection.selectedNodes();
    runtime->noteLatePages(selection.missingPages);
    // R42's LOD Level overlay: the main view's cut, kept only while the overlay asks for it (R37)
    if (runtime->recordingDrawnNodes() && &view == &terrain.views[0]){
        std::vector<TerrainWorldDrawnNode> drawn;
        auto keep = [&](const WorldSelectNode& node, bool overview){
            drawn.push_back(TerrainWorldDrawnNode{node.x, node.z, node.size, node.level, overview});
        };
        for (const WorldTileDraws& draws : selection.near){
            for (const WorldSelectNode& node : draws.full) keep(node, false);
            for (const WorldSelectNode& node : draws.half) keep(node, false);
        }
        for (const WorldSelectNode& node : selection.overviewFull) keep(node, true);
        for (const WorldSelectNode& node : selection.overviewHalf) keep(node, true);
        runtime->setDrawnNodes(std::move(drawn));
    }

    // one buffer per grid for the whole view, each item a slice of it bound by byte offset (R39)
    auto append = [&](int grid, const WorldSelectNode& node){
        view.nodesbuffer[grid].addVector2(AttributeType::TERRAINNODEPOSITION, Vector2(node.x, node.z));
        view.nodesbuffer[grid].addFloat(AttributeType::TERRAINNODESIZE, node.size);
        view.nodesbuffer[grid].addFloat(AttributeType::TERRAINNODERANGE, node.range);
        view.nodesbuffer[grid].addFloat(AttributeType::TERRAINNODERESOLUTION, node.resolution);
    };
    for (const WorldTileDraws& draws : selection.near){
        TerrainWorldRender::Impl::Item item;
        item.tile = draws.tile;
        item.first[0] = view.nodesbuffer[0].getCount();
        item.first[1] = view.nodesbuffer[1].getCount();
        for (const WorldSelectNode& node : draws.full) append(0, node);
        for (const WorldSelectNode& node : draws.half) append(1, node);
        item.count[0] = static_cast<uint32_t>(draws.full.size());
        item.count[1] = static_cast<uint32_t>(draws.half.size());
        viewState.items.push_back(item);
    }
    if (!selection.overviewFull.empty() || !selection.overviewHalf.empty()){
        TerrainWorldRender::Impl::Item item;
        item.overview = true;
        item.first[0] = view.nodesbuffer[0].getCount();
        item.first[1] = view.nodesbuffer[1].getCount();
        for (const WorldSelectNode& node : selection.overviewFull) append(0, node);
        for (const WorldSelectNode& node : selection.overviewHalf) append(1, node);
        item.count[0] = static_cast<uint32_t>(selection.overviewFull.size());
        item.count[1] = static_cast<uint32_t>(selection.overviewHalf.size());
        viewState.items.push_back(item);
    }
}

bool RenderSystem::drawTerrainWorldItems(ObjectRender& render, MeshComponent& mesh, unsigned int submeshIndex, TerrainComponent& terrain, TerrainView& view,
                                         TerrainWorldPass pass, int slotTerrain){
    TerrainWorldRuntime* runtime = terrain.world;
    if (!runtime || !runtime->render || submeshIndex > 1)
        return false;
    TerrainWorldRender::Impl& state = *runtime->render->impl;
    const auto viewIt = state.views.find(&view);
    if (viewIt == state.views.end())
        return false;
    Submesh& submesh = mesh.submeshes[submeshIndex];
    const std::shared_ptr<ShaderRender>& shader = pass == TerrainWorldPass::COLOR ? submesh.shader
                                                 : (pass == TerrainWorldPass::GBUFFER ? submesh.gbufferShader : submesh.depthShader);
    if (!shader)
        return false;
    ShaderData& shaderData = shader->shaderData;
    const std::pair<int, int> heightSlot = shaderData.getTextureIndex(TextureShaderType::HEIGHTMAP);
    const std::pair<int, int> holeSlot = shaderData.getTextureIndex(TextureShaderType::TERRAINHOLES);
    const std::pair<int, int> baseSlot = pass == TerrainWorldPass::COLOR ? shaderData.getTextureIndex(TextureShaderType::BASECOLOR) : std::pair<int, int>(-1, -1);
    std::pair<int, int> blendSlots[MAX_TERRAIN_BLENDMAPS];
    for (int m = 0; m < MAX_TERRAIN_BLENDMAPS; m++)
        blendSlots[m] = shaderData.getTextureIndex(static_cast<TextureShaderType>(static_cast<int>(TextureShaderType::BLENDMAP) + m));

    const WorldLattice& lattice = runtime->lattice();
    const float tileSize = lattice.tileSize();
    const TerrainWorldComponent* worldComponent = scene->findComponent<TerrainWorldComponent>(runtime->getEntity());
    // R9: the overview casts no shadow unless asked; a near tile does
    const bool overviewInShadows = worldComponent && worldComponent->overviewCastsShadows;
    const bool shadowPass = pass == TerrainWorldPass::SHADOW;
    const size_t stride = view.nodesbuffer[submeshIndex].getStride();
    TextureRender* materialBase = submesh.material.baseColorTexture.getRender(&emptyWhite);

    WorldTerrainBlock block;
    std::memcpy(block.eyeAndTerrain, &terrain.eyePos, sizeof(block.eyeAndTerrain));
    const float eye[3] = {view.nodesEyePos.x, view.nodesEyePos.y, view.nodesEyePos.z};
    std::memcpy(block.eyeAndTerrain, eye, sizeof(eye));
    // heights decode over the world's range (R2), not the Terrain's unused one
    block.eyeAndTerrain[4] = runtime->maxHeight();
    const float skirt = std::max(1.0f, runtime->manifest().skirtDepth * runtime->maxHeight());
    const WorldOverview& overview = runtime->getOverview();

    int draws = 0;
    for (const TerrainWorldRender::Impl::Item& item : viewIt->second.items){
        const uint32_t count = item.count[submeshIndex];
        if (count == 0)
            continue;
        if (shadowPass && item.overview && !overviewInShadows)
            continue;
        if (item.overview){
            render.addTexture(heightSlot, ShaderStageType::VERTEX, &state.overviewHeight);
            for (int m = 0; m < MAX_TERRAIN_BLENDMAPS; m++)
                if (blendSlots[m].first != -1) render.addTexture(blendSlots[m], ShaderStageType::FRAGMENT, &emptyBlack);
            if (holeSlot.first != -1) render.addTexture(holeSlot, ShaderStageType::FRAGMENT, &emptyWhite);
            if (baseSlot.first != -1) render.addTexture(baseSlot, ShaderStageType::FRAGMENT, &state.overviewColour);
            const float step = tileSize / static_cast<float>(overview.stride);
            const float page[4] = {static_cast<float>(overview.low.x) * tileSize, static_cast<float>(overview.low.z) * tileSize, step, 1.0f};
            const float size[4] = {static_cast<float>(overview.width()), static_cast<float>(overview.height()), skirt, tileSize};
            const float tile[4] = {page[0], page[1], static_cast<float>(lattice.blendQuadCount()), 0.0f};
            std::memcpy(block.worldPage, page, sizeof(page));
            std::memcpy(block.worldPageSize, size, sizeof(size));
            std::memcpy(block.worldTile, tile, sizeof(tile));
        }else{
            const auto pageIt = state.pages.find(item.tile.key());
            if (pageIt == state.pages.end())
                continue;
            TerrainWorldRender::Impl::Page& page = *pageIt->second;
            render.addTexture(heightSlot, ShaderStageType::VERTEX, &page.height);
            for (int m = 0; m < MAX_TERRAIN_BLENDMAPS; m++)
                if (blendSlots[m].first != -1) render.addTexture(blendSlots[m], ShaderStageType::FRAGMENT, page.blendMade[m] ? &page.blend[m] : &emptyBlack);
            if (holeSlot.first != -1) render.addTexture(holeSlot, ShaderStageType::FRAGMENT, page.holesMade ? &page.holes : &emptyWhite);
            if (baseSlot.first != -1) render.addTexture(baseSlot, ShaderStageType::FRAGMENT, materialBase ? materialBase : &emptyWhite);
            const float originX = static_cast<float>(item.tile.x) * tileSize, originZ = static_cast<float>(item.tile.z) * tileSize;
            // texel 0 is the gutter, a sample before the tile's corner
            const float pageBlock[4] = {originX - lattice.spacing, originZ - lattice.spacing, lattice.spacing, 0.0f};
            const float size[4] = {static_cast<float>(lattice.heightSide()), static_cast<float>(lattice.heightSide()), skirt, tileSize};
            const float tile[4] = {originX, originZ, static_cast<float>(lattice.blendQuadCount()), 0.0f};
            std::memcpy(block.worldPage, pageBlock, sizeof(pageBlock));
            std::memcpy(block.worldPageSize, size, sizeof(size));
            std::memcpy(block.worldTile, tile, sizeof(tile));
        }
        render.replaceVertexBuffer(terrain.views[0].nodesbuffer[submeshIndex].getRender(), view.nodesbuffer[submeshIndex].getRender(),
                                   static_cast<size_t>(item.first[submeshIndex]) * stride);
        if (slotTerrain != -1)
            render.applyUniformBlock(slotTerrain, sizeof(WorldTerrainBlock), &block);
        render.draw(0, submesh.vertexCount, count);
        draws++;
    }
    int viewIndex = 0;
    for (int v = 0; v < MAX_TERRAIN_VIEWS; v++)
        if (&terrain.views[v] == &view) viewIndex = v;
    const int passIndex = pass == TerrainWorldPass::COLOR ? 0 : (pass == TerrainWorldPass::GBUFFER ? 2 : 1);
    runtime->mutableStats().draws[viewIndex][passIndex] = draws;
    return true;
}

#else

// the module is out: no Terrain has a world, so RenderSystem never reaches these
bool jasperengine::terrainWorldHasHoles(const TerrainComponent&) {
    return false;
}

void RenderSystem::updateTerrainWorldPages(Entity, TerrainComponent&){}

void RenderSystem::selectTerrainWorld(TerrainComponent&, const Transform&, CameraComponent&, const Transform&, TerrainView&){}

bool RenderSystem::drawTerrainWorldItems(ObjectRender&, MeshComponent&, unsigned int, TerrainComponent&, TerrainView&, TerrainWorldPass, int){
    return false;
}

#endif
