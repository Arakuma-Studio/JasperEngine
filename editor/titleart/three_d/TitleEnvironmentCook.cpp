// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleEnvironmentCook.h"

#include "texture/TextureEncoders.h"
#include "texture/TextureFormatSelector.h"
#include "texture/TexturePrepare.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleTierFeatures.h"

#include <cstring>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct Cache {
        std::mutex mutex;
        std::map<std::string, std::vector<uint8_t>> files;
        TitleEnvironmentCookStats stats;
    };

    Cache& cache() {
        static Cache* c = new Cache();
        return *c;
    }

    const char* familyName(TextureFamily family) {
        switch (family) {
            case TextureFamily::Mobile: return "mobile";
            case TextureFamily::AndroidEtc2: return "android_etc2";
            case TextureFamily::Web: return "web";
            default: return "desktop";
        }
    }

}

bool editor::cookTitleEnvironment(const std::string& preset, titleart::TitleTier tier, TextureFamily family, std::vector<uint8_t>& ktx2,
                                  std::string& error) {
    const titleart::TitleEnvScene* scene = titleart::titleEnvironmentPreset(preset);
    if (!scene) {
        error = "no title environment '" + preset + "'";
        return false;
    }
    std::shared_ptr<const titleart::TitleHdri> hdri;
    if (!scene->procedural) {
        hdri = titleart::loadTitleHdri(scene->hdri);
        if (!hdri) {
            error = "the environment '" + preset + "' needs its HDRI (" + scene->hdri + "), which is not here";
            return false;
        }
    }
    titleart::Environment3D env;
    env.preset = preset;
    const int face = titleart::titleEnvironmentFace(env, 1.0, tier);

    // the format of the family's HDR row
    TextureFormatInputs inputs;
    inputs.family = family;
    inputs.settings.type = TextureImportType::HDR;
    inputs.settings.shape = TextureShape::Cube;
    inputs.settings.srgb = false;
    inputs.settings.mipmaps = true;
    inputs.floatSource = true;
    inputs.width = inputs.height = face;
    const TextureFormatChoice choice = TextureFormatSelector::select(inputs);
    const ITextureEncoder& encoder = textureEncoderFor(choice);

    const std::string key = preset + "|" + std::to_string(face) + "|" + familyName(family) + "|" + std::to_string(titleart::TITLE_ENV_BUILDER_VERSION) + "|" +
                            encoder.name() + "|" + encoder.version() + "|" + choice.label();
    {
        Cache& c = cache();
        std::lock_guard<std::mutex> lock(c.mutex);
        auto it = c.files.find(key);
        if (it != c.files.end()) {
            ktx2 = it->second;
            c.stats.hits++;
            return true;
        }
    }

    const titleart::TitleEnvCube cube = titleart::buildTitleEnvironmentCpu(*scene, hdri.get(), titleart::titleHdriYaw(preset), face);
    PreparedTexture prepared;
    prepared.layout = PreparedTexture::Layout::RGBAF;
    prepared.srgb = false;
    prepared.hasAlpha = false;
    prepared.width = prepared.height = prepared.logicalWidth = prepared.logicalHeight = face;
    prepared.faces.resize(6);
    for (size_t level = 0; level < cube.levels.size(); level++) {
        const int size = std::max(1, face >> level);
        for (int f = 0; f < 6; f++) {
            MipLevel m;
            m.width = m.height = size;
            m.pixels.resize((size_t)size * size * 16);
            float* out = reinterpret_cast<float*>(m.pixels.data());
            for (size_t i = 0; i < (size_t)size * size; i++) {
                const auto& v = cube.levels[level][(size_t)f * size * size + i];
                out[i * 4 + 0] = v[0];
                out[i * 4 + 1] = v[1];
                out[i * 4 + 2] = v[2];
                out[i * 4 + 3] = 1.0f;
            }
            prepared.faces[(size_t)f].push_back(std::move(m));
        }
    }
    TextureEncodeRequest request;
    request.prepared = &prepared;
    request.choice = choice;
    request.effort = TextureEffort::Normal;
    request.type = TextureImportType::HDR;
    request.keyValues = {{"JE.logical", "title_environment"},
                         {"JE.titleart.environment", preset},
                         {"JE.titleart.face", std::to_string(face)},
                         {"JE.titleart.builder", std::to_string(titleart::TITLE_ENV_BUILDER_VERSION)}};
    std::vector<uint8_t> out;
    if (!encoder.encode(request, out, error)) return false;
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.files[key] = out;
    c.stats.encodes++;
    ktx2 = std::move(out);
    return true;
}

TitleEnvironmentCookStats editor::titleEnvironmentCookStats() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);
    return c.stats;
}
