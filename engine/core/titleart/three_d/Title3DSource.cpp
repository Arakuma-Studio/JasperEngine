// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Title3DSource.h"

#include "titleart/render/TitleContent.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleMesh.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::three_d;

namespace {

    const Json& charsOf(const Json& layer) {
        static const Json none = Json::object();
        auto it = layer.find("chars");
        return it != layer.end() && it->is_object() ? *it : none;
    }

}

bool jasperengine::titleart::three_d::layerCharRigged(const Json& layer, uint32_t charIndex) {
    const Json& chars = charsOf(layer);
    auto it = chars.find(std::to_string(charIndex));
    return it != chars.end() && charIsRigged(*it);
}

TitleSource3D jasperengine::titleart::three_d::collectTitleSource(const Json& layer, const TitleArtDoc& doc, TitleContentProvider& content,
                                                                  const AssetResolver* assets, std::vector<TitleProblem>* problems) {
    TitleSource3D source;
    const LayerType type = layerTypeOf(layer);
    source.size = layerSourceSize(layer);
    source.text = type == LayerType::Text;
    if (source.text) source.shear = layer.value("shear", 0.0);
    const Json& style = layer.contains("style_3d") ? layer["style_3d"] : Json();
    source.orb = type == LayerType::Shape && layer.value("shape", std::string()) == "orb" && style.is_object() &&
                 style.value("mode", std::string()) == "orb";

    // rigged characters are laid out without their 2D adjustments: those become their matrix
    Json laidOut = layer;
    bool anyRigged = false;
    if (source.text) {
        for (auto& [key, adjust] : laidOut["chars"].items()) {
            if (!charIsRigged(adjust)) continue;
            anyRigged = true;
            adjust["offset"] = Json::array({0, 0});
            adjust["scale"] = 1;
            adjust["h_scale"] = 1;
            adjust["v_scale"] = 1;
            adjust["rotation"] = 0;
        }
    }
    TitleContentProvider::Outlines outlines;
    if (!content.outlines(anyRigged ? laidOut : layer, doc, assets, outlines)) return source;
    if (problems) problems->insert(problems->end(), outlines.problems.begin(), outlines.problems.end());
    for (size_t i = 0; i < outlines.glyphs.size(); i++) {
        geometry::GlyphOutline& g = outlines.glyphs[i];
        TitleGlyph3D glyph;
        glyph.path = std::move(g.path);
        glyph.charIndex = g.charIndex;
        glyph.pivot = g.pivot;
        if (source.text) {
            if (i < outlines.spans.size()) {
                glyph.charBox = outlines.spans[i].charBox;
                glyph.lineBox = outlines.spans[i].lineBox;
            } else {
                glyph.charBox = glyph.lineBox = glyph.path.controlBounds();
            }
            glyph.rigged = layerCharRigged(layer, glyph.charIndex);
        }
        source.glyphs.push_back(std::move(glyph));
    }
    return source;
}

std::map<uint32_t, CharBlock3D> jasperengine::titleart::three_d::collectCharBlocks(const Json& layer, size_t bandCount) {
    std::map<uint32_t, CharBlock3D> out;
    if (layerTypeOf(layer) != LayerType::Text) return out;
    const uint32_t size = 3 + 2 * (uint32_t)bandCount;
    uint32_t next = size;
    std::vector<std::pair<uint32_t, const Json*>> keyed;
    for (const auto& [key, adjust] : charsOf(layer).items()) {
        if (!charHasMaterialBlock(adjust)) continue;
        keyed.push_back({(uint32_t)std::stoul(key), &adjust});
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [index, adjust] : keyed) {
        if (next + size > TITLE_3D_MAX_SLOTS) break;
        CharBlock3D block;
        block.slot = (uint16_t)next;
        const Json& bands = adjust->contains("bands_3d") ? (*adjust)["bands_3d"] : Json();
        for (size_t i = 0; i < bandCount; i++) block.bands.push_back(bands.is_array() && i < bands.size() && bands[i].is_object());
        out[index] = std::move(block);
        next += size;
    }
    return out;
}

TitleSlotMaterials jasperengine::titleart::three_d::resolveSlotMaterials(const Json& layer, const Title3D& st, const TitleMesh& mesh) {
    TitleSlotMaterials out;
    std::vector<std::string> keys;
    auto indexOf = [&](const std::string& key, const TitleMaterial3D& m) -> uint8_t {
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i] == key) return (uint8_t)i;
        }
        if (out.materials.size() >= 16) return 0;
        keys.push_back(key);
        out.materials.push_back(m);
        return (uint8_t)(out.materials.size() - 1);
    };
    const Json& chars = charsOf(layer);
    std::map<uint16_t, std::pair<std::string, const Json*>> blocks;
    for (const auto& [ch, block] : collectCharBlocks(layer, st.bands.size())) {
        auto it = chars.find(std::to_string(ch));
        if (it != chars.end()) blocks[block.slot] = {std::to_string(ch), &*it};
    }
    auto member = [](const Json& o, const char* key) -> const Json& {
        static const Json none;
        auto it = o.find(key);
        return it != o.end() ? *it : none;
    };
    out.slotMaterial.resize(mesh.slots.size());
    for (size_t s = 0; s < mesh.slots.size(); s++) {
        const TitleMeshSlot& slot = mesh.slots[s];
        const size_t band = (size_t)std::max<int16_t>(0, slot.band);
        std::string key = "face";
        TitleMaterial3D m = st.face;
        switch (slot.role) {
            case TitleSlotRole::Face: break;
            case TitleSlotRole::Bevel:
                if (st.bevelMaterial) {
                    key = "bevel";
                    m = *st.bevelMaterial;
                }
                break;
            case TitleSlotRole::Side:
                if (st.side) {
                    key = "side";
                    m = *st.side;
                }
                break;
            case TitleSlotRole::BandFront:
            case TitleSlotRole::BandSide:
                if (band < st.bands.size()) {
                    const Band3D& b = st.bands[band];
                    const bool side = slot.role == TitleSlotRole::BandSide && b.side;
                    key = "band" + std::to_string(band) + (side ? "s" : "");
                    m = side ? *b.side : b.material;
                }
                break;
        }
        auto block = blocks.find(slot.block);
        if (block != blocks.end()) {
            const Json& a = *block->second.second;
            const std::string prefix = "char" + block->second.first + ":";
            if ((slot.role == TitleSlotRole::Face || slot.role == TitleSlotRole::Bevel) && member(a, "face_3d").is_object()) {
                key = prefix + "face";
                m = decodeMaterial3D(member(a, "face_3d"));
            } else if (slot.role == TitleSlotRole::Side && member(a, "side_3d").is_object()) {
                key = prefix + "side";
                m = decodeMaterial3D(member(a, "side_3d"));
            } else if (slot.role == TitleSlotRole::BandFront || slot.role == TitleSlotRole::BandSide) {
                const Json& bands = member(a, "bands_3d");
                if (bands.is_array() && band < bands.size() && bands[band].is_object()) {
                    key = prefix + "band" + std::to_string(band);
                    m = decodeMaterial3D(bands[band]);
                }
            }
        }
        out.slotMaterial[s] = indexOf(key, m);
    }
    return out;
}
