// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T1 R23, DC10: a height edit keeps the node tree. Only the ranges over the edit and their ancestors
// are computed again, and what follows the surface reads the revision. The hook sits in the terrain
// loop of MeshSystem::update, guarded by the terrain's edit state.

#include "MeshSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/TerrainComponent.h"
#include "object/TerrainEdit.h"
#include "texture/Texture.h"
#include "render/TextureRender.h"
#include "pool/TexturePool.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace jasperengine;
using jasperengine::terrain::TexelRect;

namespace {

    // A node reads its leaves' texels a margin past its square (getTerrainHeightRangeArea), and
    // this is asked of nodes at every level: two texels keep rounding on the safe side
    constexpr int NODE_TEXEL_MARGIN = 2;

    TexelRect nodeTexels(const TerrainComponent& terrain, const TerrainNode& node, int mapWidth, int mapHeight) {
        const float half = terrain.terrainSize * 0.5f;
        const float size = node.size;
        const float left = node.position.x - size * 0.5f + half;
        const float top = node.position.y - size * 0.5f + half;
        const int firstX = static_cast<int>(std::floor(mapWidth * left / terrain.terrainSize)) - NODE_TEXEL_MARGIN;
        const int lastX = static_cast<int>(std::floor(mapWidth * (left + size) / terrain.terrainSize)) + NODE_TEXEL_MARGIN;
        const int firstY = static_cast<int>(std::floor(mapHeight * top / terrain.terrainSize)) - NODE_TEXEL_MARGIN;
        const int lastY = static_cast<int>(std::floor(mapHeight * (top + size) / terrain.terrainSize)) + NODE_TEXEL_MARGIN;
        return TexelRect{firstX, firstY, lastX - firstX + 1, lastY - firstY + 1};
    }


    // T1 DC9: one paced upload of an edited map into its stream image; false when the frame's
    // staging budget refused it
    bool uploadEditedTerrainMap(Texture& texture, TerrainEditState::Upload& upload) {
        // a map not drawn yet, a framebuffer or a cooked one: its next draw makes it from the pixels
        std::shared_ptr<TextureRender> render = texture.empty() || texture.isFramebuffer() ? nullptr : TexturePool::get(texture.getRenderId());
        if (!render || !render->isCreated() || !texture.hasData() || !texture.getData().getData()) {
            texture.invalidateRender();
            return true;
        }
        TextureData& data = texture.getData();
        const ColorFormat format = data.getColorFormat();
        if (format != ColorFormat::RED16 && format != ColorFormat::RED && format != ColorFormat::RGBA) {
            texture.invalidateRender();
            return true;
        }
        const int width = data.getWidth();
        const int height = data.getHeight();
        // the first upload makes the stream image in place of the texture the map was drawn with
        // (those holding it bind it again: noteRebuilt), and every texel is new to it
        if (!render->isDynamicMap() || render->getWidth() != width || render->getHeight() != height) {
            if (!render->createDynamicMapTexture(texture.getId(), width, height, format)) {
                texture.invalidateRender();
                return true;
            }
            render->noteRebuilt();
            upload.shadow.clear();
            upload.dirty = TexelRect{0, 0, width, height};
        }
        const ColorFormat uploadFormat = render->getUploadFormat();
        if (uploadFormat == format) {
            return render->updateTexture(data.getData(), static_cast<size_t>(data.getSize()));
        }
        // R45: GLES3 and WebGL2 heights go up as floats; only the changed rows are converted
        const TerrainUploadFormat shadowFormat = uploadFormat == ColorFormat::R32F ? TerrainUploadFormat::Float32 : TerrainUploadFormat::Half;
        const size_t texelBytes = shadowFormat == TerrainUploadFormat::Float32 ? 4 : 2;
        const size_t shadowBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * texelBytes;
        if (upload.shadow.size() != shadowBytes) {
            upload.shadow.assign(shadowBytes, 0);
            upload.dirty = TexelRect{0, 0, width, height};
        }
        const TexelRect rect = terrain::intersect(upload.dirty, TexelRect{0, 0, width, height});
        if (!rect.empty()) {
            convertTerrainHeightRect(static_cast<const uint16_t*>(data.getData()), width, rect, shadowFormat, upload.shadow.data());
            upload.dirty = TexelRect{};
        }
        return render->updateTexture(upload.shadow.data(), upload.shadow.size());
    }
}

bool MeshSystem::refitTerrainNode(TerrainComponent& terrain, size_t nodeIndex, int rectX, int rectY, int rectW, int rectH, int mapWidth, int mapHeight) {
    TerrainNode& node = terrain.nodes[nodeIndex];
    if (terrain::intersect(nodeTexels(terrain, node, mapWidth, mapHeight), TexelRect{rectX, rectY, rectW, rectH}).empty()) return false;

    if (!node.hasChilds) {
        const float half = terrain.terrainSize * 0.5f;
        getTerrainHeightRangeArea(terrain, node.position.x - node.size * 0.5f + half, node.position.y - node.size * 0.5f + half,
                                  node.size, node.size, node.minHeight, node.maxHeight);
        return true;
    }

    bool refitted = false;
    for (size_t child : node.childs) {
        refitted = refitTerrainNode(terrain, child, rectX, rectY, rectW, rectH, mapWidth, mapHeight) || refitted;
    }
    if (refitted) {
        const TerrainNode* c = terrain.nodes.data();
        node.maxHeight = std::max(std::max(c[node.childs[0]].maxHeight, c[node.childs[1]].maxHeight), std::max(c[node.childs[2]].maxHeight, c[node.childs[3]].maxHeight));
        node.minHeight = std::min(std::min(c[node.childs[0]].minHeight, c[node.childs[1]].minHeight), std::min(c[node.childs[2]].minHeight, c[node.childs[3]].minHeight));
    }
    return refitted;
}

void MeshSystem::refitTerrainNodes(TerrainComponent& terrain, int rectX, int rectY, int rectW, int rectH) {
    // A tree waiting to be built will read the map as it then is
    if (rectW <= 0 || rectH <= 0 || terrain.needUpdateTerrain || terrain.numNodes == 0 || terrain.nodes.empty()) return;
    if (terrain.heightMap.empty() || terrain.heightMap.isFramebuffer() || !terrain.heightMap.hasData()) return;

    TextureData& data = terrain.heightMap.getData();
    if (data.getWidth() <= 0 || data.getHeight() <= 0 || !data.getData()) return;

    for (int root = 0; root < terrain.rootGridSize * terrain.rootGridSize; root++) {
        refitTerrainNode(terrain, terrain.grid[root], rectX, rectY, rectW, rectH, data.getWidth(), data.getHeight());
    }
}

void MeshSystem::flushTerrainEdit(TerrainComponent& terrain, double nowSeconds) {
    TerrainEditState& state = *terrain.editState;
    if (!state.refit.empty()) {
        refitTerrainNodes(terrain, state.refit.x, state.refit.y, state.refit.w, state.refit.h);
        state.refit = TexelRect{};
    }
    // R79: a game's edits move the revision (and the foliage it flags) at the stroke rate too
    const bool paced = state.strokes > 0 || state.runtimeDetached;
    if (state.surfaceMoved && (!paced || nowSeconds - state.lastRevisionAt >= TerrainEditState::REVISION_SECONDS)) {
        state.surfaceMoved = false;
        state.lastRevisionAt = nowSeconds;
        terrain.surfaceRevision++;
        // instances sit on the surface: they wait for the revision, not for every stamp
        terrain.needUpdateFoliage = true;
    }

    // DC9: whole-image uploads, paced by the frame's budget
    state.frame++;
    const size_t budget = state.uploadBudget > 0 ? state.uploadBudget : static_cast<size_t>(Engine::defaultTerrainUploadBytes());
    for (TerrainEditState::Upload& upload : state.uploads) {
        if (!upload.waiting) continue;
        Texture* texture = terrainMapTexture(terrain, upload.map);
        if (!texture) {
            upload.waiting = false;
            continue;
        }
        const size_t bytes = texture->hasData() ? texture->getData().getSize() : 0;
        if (!upload.retry && upload.lastFrame != 0 && state.frame - upload.lastFrame < terrainUploadInterval(bytes, budget)) continue;
        if (!uploadEditedTerrainMap(*texture, upload)) {
            // R46: refused for this frame's staging budget; the next frame tries again
            upload.retry = true;
            continue;
        }
        upload.retry = false;
        upload.lastFrame = state.frame;
        upload.waiting = false;
        upload.dirty = TexelRect{};
    }
}
