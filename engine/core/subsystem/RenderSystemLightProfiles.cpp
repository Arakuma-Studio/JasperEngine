// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S99 (docs/specs/S99-agent-feedback-area-lights.md R33, R36): the texture of the Area Lights'
// LTC tables and the lights' IES Profiles, which the USE_AREA_LIGHTS / USE_IES variants of the
// built-in Lit sample (includes/lights.glsl, includes/area_lights.glsl).

#include "RenderSystem.h"

#include "Log.h"
#include "Scene.h"
#include "component/LightComponent.h"
#include "io/Data.h"
#include "render/IesProfile.h"
#include "render/LtcLut.h"
#include "texture/TexturePrepare.h"

#include <algorithm>

using namespace jasperengine;

namespace {
    // the profile of an .ies file, read once a path (a file that does not read is said once and
    // stays null until the scene loads again)
    std::shared_ptr<IesProfile> readIesProfile(const std::string& path){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            Log::warn("IES Profile '%s': cannot open the file", path.c_str());
            return nullptr;
        }
        auto profile = std::make_shared<IesProfile>();
        std::string error;
        if (!IesProfile::parse(data.readString(), *profile, error)){
            Log::warn("IES Profile '%s': %s", path.c_str(), error.c_str());
            return nullptr;
        }
        return profile;
    }
}

void RenderSystem::updateLightProfiles(){
    static_assert(MAX_IES_PROFILES < 127, "IES Profile bands must fit 7 bits, 0 meaning none");

    // a scene that never had an Area Light nor a profile does nothing here
    if (!areaLightSeen && !iesProfileSeen){
        return;
    }

    auto lights = scene->getComponentArray<LightComponent>();
    std::vector<std::string> bands;
    std::vector<std::shared_ptr<IesProfile>> profiles;
    bands.reserve(lightProfileBands.size());
    for (size_t i = 0; i < lights->size(); i++){
        LightComponent& light = lights->getComponentFromIndex(i);
        light.iesProfileBand = -1;
        if ((light.type != LightType::SPOT && light.type != LightType::POINT) || light.iesProfile.empty())
            continue;
        const std::string& path = light.iesProfile.path;
        auto cached = iesProfileCache.find(path);
        if (cached == iesProfileCache.end())
            cached = iesProfileCache.emplace(path, readIesProfile(path)).first;
        if (!cached->second)
            continue;
        auto band = std::find(bands.begin(), bands.end(), path);
        if (band == bands.end()){
            if ((int)bands.size() >= MAX_IES_PROFILES){
                Log::warn("IES Profile '%s': a scene holds at most %i profiles; the light draws without it", path.c_str(), MAX_IES_PROFILES);
                continue;
            }
            bands.push_back(path);
            profiles.push_back(cached->second);
            band = bands.end() - 1;
        }
        light.iesProfileBand = (int)(band - bands.begin());
    }

    if (lightProfilesCreated && bands == lightProfileBands)
        return;

    const int width = LIGHT_PROFILE_WIDTH;
    const int rows = LIGHT_PROFILE_LUT_ROWS + IesProfile::GRID_ROWS * (int)bands.size();
    std::vector<uint16_t> texels((size_t)width * rows * 4, 0);
    // the LTC tables side by side
    static_assert(LIGHT_PROFILE_WIDTH == 2 * LTC_LUT_SIZE && LIGHT_PROFILE_LUT_ROWS == LTC_LUT_SIZE, "the LTC tables fill the first rows");
    static_assert(IesProfile::GRID_COLUMNS == LIGHT_PROFILE_WIDTH, "an IES Profile's grid is a row of the texture");
    for (int y = 0; y < LTC_LUT_SIZE; y++){
        for (int x = 0; x < LTC_LUT_SIZE; x++){
            for (int c = 0; c < 4; c++){
                texels[((size_t)y * width + x) * 4 + c] = ltcLut1[(y * LTC_LUT_SIZE + x) * 4 + c];
                texels[((size_t)y * width + LTC_LUT_SIZE + x) * 4 + c] = ltcLut2[(y * LTC_LUT_SIZE + x) * 4 + c];
            }
        }
    }
    for (size_t b = 0; b < profiles.size(); b++){
        for (int r = 0; r < IesProfile::GRID_ROWS; r++){
            const size_t row = (size_t)LIGHT_PROFILE_LUT_ROWS + b * IesProfile::GRID_ROWS + r;
            for (int x = 0; x < IesProfile::GRID_COLUMNS; x++){
                texels[(row * width + x) * 4] = floatToHalf(profiles[b]->grid[(size_t)r * IesProfile::GRID_COLUMNS + x]);
            }
        }
    }

    TextureRender next;
    void* data[1] = {texels.data()};
    size_t sizes[1] = {texels.size() * sizeof(uint16_t)};
    if (!next.createTexture("light-profiles", width, rows, ColorFormat::RGBA16F, TextureType::TEXTURE_2D, 1, data, sizes,
                            TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
        Log::error("Cannot create the light profiles texture (%ix%i)", width, rows);
        return;
    }
    if (lightProfilesCreated)
        lightProfiles.destroyTexture();
    lightProfiles = next;
    lightProfilesCreated = true;
    lightProfileBands = bands;
}
