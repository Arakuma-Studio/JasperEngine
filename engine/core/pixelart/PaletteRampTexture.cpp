// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteRampTexture.h"

#include "Log.h"
#include "pixelart/Palette.h"
#include "pool/TextureDataPool.h"
#include "pool/TexturePool.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    constexpr std::string_view kRampKey = "#ramp:";

    struct RampRegistry {
        std::mutex mutex;
        std::map<std::string, std::set<std::string>> idsByPalette;   // palette path -> texture ids
        std::map<std::string, std::string> pathById;
    };

    RampRegistry& rampRegistry() {
        static RampRegistry* instance = new RampRegistry();
        return *instance;
    }

}
}

bool pixelart::parsePaletteRampPath(std::string_view path, std::string* palette, std::string* ramp) {
    const size_t at = path.find(kRampKey);
    if (at == std::string_view::npos || at < 8 || path.substr(at - 8, 8) != ".palette") return false;
    const std::string_view name = path.substr(at + kRampKey.size());
    if (name.empty()) return false;
    if (palette) *palette = std::string(path.substr(0, at));
    if (ramp) *ramp = std::string(name);
    return true;
}

TextureData pixelart::makePaletteRampTextureData(const std::string& palette, const std::string& ramp, bool* missing) {
    std::shared_ptr<const Palette> source = PaletteLibrary::get(palette);
    std::vector<Rgba8> colors;
    if (source) colors = source->rampColors(ramp);
    if (missing) *missing = colors.empty();
    if (colors.empty()) {
        Log::warn("palette_ramp_missing: %s has no ramp '%s' (bound as white)", palette.c_str(), ramp.c_str());
        colors.push_back(Rgba8{255, 255, 255, 255});
    }
    const unsigned int size = (unsigned int)(colors.size() * 4);
    unsigned char* pixels = static_cast<unsigned char*>(std::malloc(size));
    if (!pixels) return TextureData();
    for (size_t i = 0; i < colors.size(); i++) {
        pixels[i * 4] = colors[i].r;
        pixels[i * 4 + 1] = colors[i].g;
        pixels[i * 4 + 2] = colors[i].b;
        pixels[i * 4 + 3] = colors[i].a;
    }
    // not owned: whoever stores it takes the pixels (the pool's copy, or setDataOwned)
    return TextureData((int)colors.size(), 1, size, ColorFormat::RGBA, 4, pixels);
}

void pixelart::registerPaletteRampTexture(const std::string& id, const std::string& path) {
    std::string palette, ramp;
    if (!parsePaletteRampPath(path, &palette, &ramp)) return;
    {
        RampRegistry& registry = rampRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.idsByPalette[palette].insert(id);
        registry.pathById[id] = path;
    }
    TextureDataPool::setGenerator(id, [palette, ramp]() { return makePaletteRampTextureData(palette, ramp); });
}

std::vector<std::string> pixelart::paletteRampTextureIds(const std::string& palette) {
    RampRegistry& registry = rampRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.idsByPalette.find(palette);
    if (it == registry.idsByPalette.end()) return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

void pixelart::reloadPaletteRampTextures(const std::string& palette) {
    for (const std::string& id : paletteRampTextureIds(palette)) {
        std::string path;
        {
            RampRegistry& registry = rampRegistry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            path = registry.pathById[id];
        }
        std::string paletteName, ramp;
        if (!parsePaletteRampPath(path, &paletteName, &ramp)) continue;
        auto data = std::make_shared<std::array<TextureData, 6>>();
        (*data)[0] = makePaletteRampTextureData(paletteName, ramp);
        (*data)[0].setDataOwned(true);
        TextureDataPool::replace(id, data);
        // stale, made again by the next getRender() with the new pixels
        TexturePool::invalidate(id);
    }
}
