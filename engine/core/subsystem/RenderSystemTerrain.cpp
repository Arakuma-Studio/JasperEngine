// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The terrain's material on the GPU (T3 D1): the render system's half of what engine/core/terrain
// decides, kept out of RenderSystem.cpp, which holds only the calls.

#include "RenderSystem.h"

#include "Log.h"
#include "object/TerrainEdit.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

using namespace jasperengine;

// T3 R18, R20, R29: a variant with 'Thl' reads the hole map in whichever pass this binds. The loaded
// variant may outlive the map (the last fill, until its plain variant is built), and a map that fails
// to load or is not R8 has no texels for the CPU queries: both bind white, which is solid, so the
// drawn surface never disagrees with them.
bool RenderSystem::loadTerrainHoleTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData){
    const std::pair<int, int> slotTex = shaderData.getTextureIndex(TextureShaderType::TERRAINHOLES);
    if (slotTex.first == -1){
        return true;
    }

    TextureRender* textureRender = &emptyWhite;
    if (!terrain.holeMap.empty()){
        const ResourceLoadState state = prepareTerrainHoleMap(terrain);
        if (state == ResourceLoadState::Loading){
            return false;
        }
        const bool r8 = state == ResourceLoadState::Finished && terrain.holeMap.hasData() &&
                        terrain.holeMap.getData().getChannels() == 1 &&
                        TextureData::getBytesPerChannel(terrain.holeMap.getData().getColorFormat()) == 1;
        if (r8){
            textureRender = terrain.holeMap.getRender(&emptyWhite);
            if (!textureRender || !textureRender->isCreated()){
                return false;
            }
        }else if (!terrain.holeMapWarned){
            terrain.holeMapWarned = true;
            Log::warn("Terrain hole map '%s' failed to load or is not 8-bit single channel: it draws no holes", terrain.holeMap.getPath(0).c_str());
        }
    }

    render.addTexture(slotTex, ShaderStageType::FRAGMENT, textureRender);
    return true;
}
