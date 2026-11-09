// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TerrainQualityPanel.h"

#include "imgui.h"
#include "mcp/TerrainMaterialInfo.h"
#include "terrain/TerrainLayers.h"
#include "texture/TextureData.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

std::string editor::terrainCostStripText(const terraincore::MaterialCost& cost) {
    return "Fetches per pixel: Flat " + std::to_string(cost.flat) + " \xC2\xB7 Slope " + std::to_string(cost.slope) + " \xC2\xB7 Worst " +
           std::to_string(cost.worst) + " \xC2\xB7 Textures " + std::to_string(cost.textureSlots) + "/" + std::to_string(terraincore::TERRAIN_TEXTURE_SLOT_LIMIT);
}

bool editor::terrainCostOverBudget(const terraincore::MaterialCost& cost, terraincore::TerrainQuality tier, const terraincore::TerrainMaterialInputs& inputs, std::string& reason) {
    if (cost.textureSlots > terraincore::TERRAIN_TEXTURE_SLOT_LIMIT) {
        reason = "More texture slots than the portable 16: leave a lighting feature or a base map out.";
        return true;
    }
    const int budget = terraincore::terrainTierInfo(tier).worstFetches;
    const int extras = (inputs.holes ? 1 : 0) + (inputs.macroNormal && terraincore::terrainTierInfo(tier).macroNormal ? 1 : 0);
    if (budget > 0 && cost.worst > budget + extras) {
        reason = "Above this tier's budget of " + std::to_string(budget + extras) + " reads.";
        return true;
    }
    if (budget == 0 && !inputs.limitLayersPerPixel && inputs.layerCount > 4) {
        reason = "Most of this is " + std::to_string(inputs.layerCount) + " layers read at one pixel: Limit Layers Per Pixel keeps the strongest 4.";
        return cost.worst > 51;
    }
    return false;
}

void editor::drawTerrainCostStrip(const TerrainComponent& terrainComponent, int& shown) {
    const terraincore::TerrainMaterialInputs inputs = mcp::terrainMaterialInputs(terrainComponent, true);
    const terraincore::TerrainQuality effective = mcp::terrainEffectiveTier(terrainComponent).quality;
    const terraincore::TerrainQuality tier = shown < 0 ? effective : static_cast<terraincore::TerrainQuality>(shown);
    const terraincore::MaterialCost cost = terraincore::materialCost(inputs, tier);

    std::string reason;
    const bool over = terrainCostOverBudget(cost, tier, inputs, reason);
    if (over) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.3f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", terrainCostStripText(cost).c_str());
    if (over) ImGui::PopStyleColor();
    if (over && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reason.c_str());

    const std::string preview = shown < 0 ? std::string("Effective (") + terraincore::terrainQualityName(effective) + ")" : std::string(terraincore::terrainQualityName(tier));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##TerrainCostTier", preview.c_str())) {
        if (ImGui::Selectable("Effective", shown < 0)) shown = -1;
        for (int i = 0; i < terraincore::TERRAIN_QUALITY_COUNT; i++) {
            if (ImGui::Selectable(terraincore::terrainQualityName(static_cast<terraincore::TerrainQuality>(i)), shown == i)) shown = i;
        }
        ImGui::EndCombo();
    }
}

std::vector<std::string> editor::terrainQualityWarnings(const TerrainComponent& terrainComponent, const Material& material) {
    std::vector<std::string> warnings;
    const terraincore::TerrainQuality tier = mcp::terrainEffectiveTier(terrainComponent).quality;
    if (!terrainComponent.holeMap.empty() && !material.occlusionTexture.empty()) {
        warnings.push_back("Holes take the base material's Occlusion map's slot: it reads as 1. The layers' own occlusion still applies.");
    }
    if (terrainComponent.macroNormal && terraincore::terrainTierInfo(tier).macroNormal && !material.normalTexture.empty()) {
        warnings.push_back("The Macro Normal Map replaces the base material's Normal map.");
    }
    if (!terrainComponent.holeMap.empty() && !material.shader.empty()) {
        warnings.push_back("A custom shader that does not read TERRAIN_HOLES draws over the holes.");
    }
    // a terrain with no layer draws its base material alone (spec Deviations)
    if (terrainComponent.macroMode != TerrainMacroMode::OFF && terrainComponent.surfaceLayers.empty()) {
        warnings.push_back("Macro Variation applies once the terrain has a layer.");
    }
    if (terrainComponent.macroMode == TerrainMacroMode::BASE_MAP && material.baseColorTexture.empty()) {
        warnings.push_back("Macro Variation Base Map has no base colour texture to read: it has no effect.");
    }
    return warnings;
}

std::string editor::terrainFeatureGreyReason(const char* feature, terraincore::TerrainQuality tier) {
    const terraincore::TerrainTierInfo& info = terraincore::terrainTierInfo(tier);
    const std::string name(feature);
    const std::string tierName = terraincore::terrainQualityName(tier);
    if (name == "macroNormal" && !info.macroNormal) return "The " + tierName + " tier does not draw a Macro Normal Map.";
    if (name == "resample" && !info.distanceResampling) return "The " + tierName + " tier does not draw Distance Resampling.";
    if (name == "limit" && tier != terraincore::TerrainQuality::HIGH) return "Medium and Low keep a fixed number of layers.";
    return std::string();
}

float editor::terrainLayerCoverage(TerrainComponent& terrainComponent, int layer) {
    const terraincore::LayerChannel channel = terraincore::terrainLayerChannel(layer);
    if (channel.map >= static_cast<int>(terrainComponent.blendMaps.size())) return 0.0f;
    Texture& map = terrainComponent.blendMaps[static_cast<size_t>(channel.map)];
    if (map.empty() || map.isFramebuffer() || !map.hasData() || !map.getData().getData()) return 0.0f;
    TextureData& data = map.getData();
    if (channel.channel >= data.getChannels()) return 0.0f;
    const uint8_t* pixels = static_cast<const uint8_t*>(data.getData());
    const size_t texels = static_cast<size_t>(data.getWidth()) * data.getHeight();
    // every 16th texel: a readout, drawn each frame
    size_t painted = 0;
    size_t read = 0;
    for (size_t t = 0; t < texels; t += 16) {
        const uint8_t value = pixels[t * data.getChannels() + channel.channel];
        painted += (channel.inverted ? value < 255 : value > 0) ? 1 : 0;
        read++;
    }
    return read > 0 ? 100.0f * static_cast<float>(painted) / static_cast<float>(read) : 0.0f;
}

void editor::drawTerrainPreviewDropdown(const std::string& popupId) {
    terraincore::TerrainQuality previewTier = terraincore::TerrainQuality::HIGH;
    const bool previewing = terraincore::TerrainQualitySettings::getEditorPreview(previewTier);
    const terraincore::TerrainDebugView view = terraincore::TerrainQualitySettings::getDebugView();
    if (ImGui::Button(("Terrain##" + popupId).c_str())) {
        ImGui::OpenPopup(popupId.c_str());
    }
    if (previewing || view != terraincore::TerrainDebugView::SHADED) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", previewing ? terraincore::terrainQualityName(previewTier) : "View");
    }
    if (ImGui::BeginPopup(popupId.c_str())) {
        ImGui::TextDisabled("Preview Quality");
        if (ImGui::MenuItem("As Project", nullptr, !previewing)) terraincore::TerrainQualitySettings::clearEditorPreview();
        for (int i = 0; i < terraincore::TERRAIN_QUALITY_COUNT; i++) {
            const terraincore::TerrainQuality tier = static_cast<terraincore::TerrainQuality>(i);
            static const char* const labels[] = {"High", "Medium", "Low"};
            if (ImGui::MenuItem(labels[i], nullptr, previewing && previewTier == tier)) terraincore::TerrainQualitySettings::setEditorPreview(tier);
        }
        ImGui::Separator();
        ImGui::TextDisabled("View");
        static const char* const views[] = {"Shaded", "Layer Count", "Fetch Cost", "Holes"};
        for (int i = 0; i < 4; i++) {
            if (ImGui::MenuItem(views[i], nullptr, static_cast<int>(view) == i)) {
                terraincore::TerrainQualitySettings::setDebugView(static_cast<terraincore::TerrainDebugView>(i));
            }
        }
        ImGui::EndPopup();
    }
}

void editor::clearTerrainPreview() {
    terraincore::TerrainQualitySettings::clearEditorPreview();
    terraincore::TerrainQualitySettings::setDebugView(terraincore::TerrainDebugView::SHADED);
}
