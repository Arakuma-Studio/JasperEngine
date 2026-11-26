// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What reads each vertex color channel of an entity (docs/specs/V1-vertex-colors.md R7, R72, R73,
// Table 2): the built-in Lit's Vertex Color section, library:vertex_blend's layers, a lattice binding's
// mask channel and the foliage wind mask; and the painted channels none of them reads, with the fix.
// A library shader or fork other than vertex_blend reads the raw stream as it likes (@require
// rawcolor): it is never warned about.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/PropertyCmd.h"
#include "component/LatticeModifierComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/VertexColorsComponent.h"
#include "subsystem/VertexColorScene.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    constexpr const char* kVcChannelLetters[4] = {"R", "G", "B", "A"};

    struct VcMaterialReads {
        std::string material;
        bool known = false;                     // a shader whose reading is known here
        vc::ChannelMask reads = 0;
        std::array<std::string, 4> uses;        // what each channel feeds
        std::string fix;                        // for a painted channel it ignores
    };

    std::string vcChannelList(vc::ChannelMask mask) {
        std::string out;
        for (int c = 0; c < 4; c++) {
            if (!(mask & (1u << c))) continue;
            if (!out.empty()) out += ", ";
            out += kVcChannelLetters[c];
        }
        return out;
    }

    int vcBlendLayers(const Material& material) {
        const std::vector<std::string>& features = material.shaderFeatures;
        auto has = [&](const char* define) { return std::find(features.begin(), features.end(), define) != features.end(); };
        if (has("LAYERS_FIVE")) return 5;
        if (has("LAYERS_FOUR")) return 4;
        if (has("LAYERS_THREE")) return 3;
        return 2;
    }

    VcMaterialReads vcReadsOf(const Material& material, const std::string& name) {
        VcMaterialReads out;
        out.material = name;
        const std::string& shader = material.shader.path;
        if (shader == "library:vertex_blend") {
            out.known = true;
            const int layers = vcBlendLayers(material);
            for (int c = 0; c + 1 < layers && c < 4; c++) {
                out.reads |= (vc::ChannelMask)(1u << c);
                out.uses[(size_t)c] = std::string("vertex_blend: Layer ") + kVcChannelLetters[c];
            }
            out.fix = "Set the material's Layers to Five so every painted channel blends a layer (Layers decide which of R, G, B, A it reads).";
            return out;
        }
        if (!shader.empty()) return out;
        out.known = true;
        const MaterialPbrExtensions& pbr = material.pbr;
        const MaterialPbrExtensions::VertexColor& vertexColor = pbr.vertexColor;
        const bool unlit = material.lighting == MaterialLighting::NONE;
        switch (vertexColor.use) {
            case VertexColorUse::OFF:
                out.fix = "Set the material's Vertex Color Use to Tint or Masks (it is Off, so the shader ignores vertex colors).";
                break;
            case VertexColorUse::TINT:
                out.reads = vc::CHANNELS_RGB;
                for (int c = 0; c < 3; c++) out.uses[(size_t)c] = "Lit: Tint";
                if (vertexColor.alphaOpacity) {
                    out.reads |= vc::CHANNEL_A;
                    out.uses[3] = "Lit: Opacity";
                }
                out.fix = "Turn on the material's Alpha multiplies opacity, or set its Vertex Color Use to Masks and map A.";
                break;
            case VertexColorUse::MASKS: {
                bool rgbMapped = false;
                for (int c = 0; c < 4; c++) {
                    const VertexColorTarget target = MaterialPbr::getVertexColorTarget(vertexColor, c);
                    if (target == VertexColorTarget::NONE) continue;
                    if (c < 3) rgbMapped = true;
                    if (unlit && target != VertexColorTarget::OPACITY && target != VertexColorTarget::ALBEDO_TINT) continue;
                    out.reads |= (vc::ChannelMask)(1u << c);
                    out.uses[(size_t)c] = std::string("Lit: ") + MaterialPbr::getVertexColorTargetLabel(target);
                }
                if (!rgbMapped) {
                    out.reads |= vc::CHANNELS_RGB;
                    for (int c = 0; c < 3; c++) out.uses[(size_t)c] = "Lit: Tint";
                }
                out.fix = "Map each painted channel to an input in the material's Vertex Color section (Use: Masks).";
                break;
            }
        }
        return out;
    }

    // per target material, and the other readers of the entity's channels
    struct VcEntityReads {
        std::vector<VcMaterialReads> materials;
        std::array<std::string, 4> others;
        vc::ChannelMask otherReads = 0;
    };

    void vcAddOther(VcEntityReads& reads, int channel, const std::string& use) {
        if (channel < 0 || channel > 3) return;
        reads.otherReads |= (vc::ChannelMask)(1u << channel);
        std::string& slot = reads.others[(size_t)channel];
        if (slot.find(use) == std::string::npos) slot += (slot.empty() ? "" : ", ") + use;
    }

    VcEntityReads vcEntityReads(Project* project, uint32_t sceneId, Entity entity) {
        VcEntityReads out;
        Scene* scene = vcd::sceneOf(project, sceneId);
        if (!scene) return out;
        std::set<std::string> seen;
        for (const vc::SceneTarget& target : vc::sceneTargets(scene, entity)) {
            const Material* material = vc::sceneTargetMaterial(scene, target);
            if (!material) continue;
            const std::string name = target.material.empty() ? "Submesh " + std::to_string(target.submesh) : target.material;
            if (!seen.insert(name).second) continue;
            out.materials.push_back(vcReadsOf(*material, name));
        }
        // a lattice binding scaled by a channel (LatticeModifierComponent.h)
        if (const LatticeModifierComponent* lattice = scene->findComponent<LatticeModifierComponent>(entity)) {
            for (const LatticeBinding& binding : lattice->bindings) {
                if (binding.mask == LatticeMaskChannel::NONE) continue;
                const int channel = ((int)binding.mask - 1) % 4;
                vcAddOther(out, channel, "Lattice mask");
            }
        }
        // a terrain's trees of this model bend by R (TerrainTreeWindMask::VertexColor)
        if (const ModelComponent* model = scene->findComponent<ModelComponent>(entity); model && !model->filename.path.empty()) {
            auto terrains = scene->getComponentArray<TerrainComponent>();
            for (size_t i = 0; i < terrains->size(); i++) {
                for (const TerrainTreePrototype& tree : terrains->getComponentFromIndex(i).treePrototypes) {
                    if (tree.windMask == TerrainTreeWindMask::VertexColor && tree.meshPath.path == model->filename.path) vcAddOther(out, 0, "Wind (foliage)");
                }
            }
        }
        return out;
    }

}

std::vector<vc::Warning> editor::vertexcolor::materialWarnings(Project* project, uint32_t sceneId, Entity entity, vc::ChannelMask painted) {
    std::vector<vc::Warning> warnings;
    const VcEntityReads reads = vcEntityReads(project, sceneId, entity);
    for (const VcMaterialReads& material : reads.materials) {
        if (!material.known) continue;
        const vc::ChannelMask ignored = (vc::ChannelMask)(painted & ~(material.reads | reads.otherReads) & vc::CHANNELS_RGBA);
        if (!ignored) continue;
        const std::string channels = vcChannelList(ignored);
        warnings.push_back({"material_ignores_channels", "Material '" + material.material + "' does not read vertex color channel" +
                                                             (channels.size() > 1 ? "s " : " ") + channels + ". " + material.fix});
    }
    return warnings;
}

std::array<std::string, 4> editor::vertexcolor::channelUses(Project* project, uint32_t sceneId, Entity entity) {
    std::array<std::string, 4> out;
    const VcEntityReads reads = vcEntityReads(project, sceneId, entity);
    const VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    for (size_t c = 0; c < 4; c++) {
        std::vector<std::string> uses;
        auto add = [&](const std::string& use) {
            if (!use.empty() && std::find(uses.begin(), uses.end(), use) == uses.end()) uses.push_back(use);
        };
        for (const VcMaterialReads& material : reads.materials) add(material.uses[c]);
        if (!reads.others[c].empty()) add(reads.others[c]);
        std::string joined;
        for (const std::string& use : uses) joined += (joined.empty() ? "" : ", ") + use;
        const std::string name = component ? component->channelNames[c] : std::string();
        if (name.empty()) out[c] = joined;
        else out[c] = joined.empty() ? name : name + " (" + joined + ")";
    }
    return out;
}

bool editor::vertexcolor::fixMaterialWarning(Project* project, uint32_t sceneId, Entity entity, const vc::Warning& warning, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Scene* scene = vcd::sceneOf(project, sceneId);
    if (!scene) return fail("scene " + std::to_string(sceneId) + " is not open.");
    // the warning names its material and channels (materialWarnings' text)
    const std::string open = "Material '", close = "' does not read vertex color channel";
    const size_t nameAt = warning.message.find(open);
    const size_t nameEnd = warning.message.find(close);
    if (warning.code != "material_ignores_channels" || nameAt != 0 || nameEnd == std::string::npos) {
        return fail("not a material_ignores_channels warning of materialWarnings(): nothing to fix.");
    }
    const std::string name = warning.message.substr(open.size(), nameEnd - open.size());
    vc::ChannelMask ignored = 0;
    for (size_t at = nameEnd + close.size(); at < warning.message.size() && warning.message[at] != '.'; at++) {
        for (int c = 0; c < 4; c++) {
            if (warning.message[at] == kVcChannelLetters[c][0] && warning.message[at - 1] == ' ') ignored |= (vc::ChannelMask)(1u << c);
        }
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    bool found = false;
    std::set<std::pair<Entity, unsigned int>> seen;
    for (const vc::SceneTarget& target : vc::sceneTargets(scene, entity)) {
        const Material* current = vc::sceneTargetMaterial(scene, target);
        if (!current || !seen.insert({target.meshEntity, target.submesh}).second) continue;
        if ((target.material.empty() ? "Submesh " + std::to_string(target.submesh) : target.material) != name) continue;
        found = true;
        const Material& material = *current;
        const std::string prefix = "submeshes[" + std::to_string(target.submesh) + "].material.";
        auto set = [&](const std::string& property, auto value) {
            history->addCommandNoMerge(new PropertyCmd<decltype(value)>(project, sceneId, target.meshEntity, ComponentType::MeshComponent, prefix + property, value));
        };
        if (material.shader.path == "library:vertex_blend") {
            // the highest channel decides: R needs two layers, A five
            int needed = 0;
            for (int c = 0; c < 4; c++) {
                if (ignored & (1u << c)) needed = c + 2;
            }
            if (needed <= vcBlendLayers(material)) continue;
            static const char* const layerNames[] = {"LAYERS_TWO", "LAYERS_THREE", "LAYERS_FOUR", "LAYERS_FIVE"};
            std::vector<std::string> features;
            for (const std::string& feature : material.shaderFeatures) {
                if (feature.rfind("LAYERS_", 0) != 0) features.push_back(feature);
            }
            features.push_back(layerNames[needed - 2]);
            set("shaderFeatures", features);
            continue;
        }
        if (!material.shader.path.empty()) continue;
        const MaterialPbrExtensions& pbr = material.pbr;
        const MaterialPbrExtensions::VertexColor vertexColor = pbr.vertexColor;
        if (vertexColor.use == VertexColorUse::MASKS) {
            // an unmapped channel shows as a grey tint, A as opacity: visible, and a start to remap
            static const char* const channelFields[] = {"r", "g", "b", "a"};
            for (int c = 0; c < 4; c++) {
                if (!(ignored & (1u << c)) || MaterialPbr::getVertexColorTarget(vertexColor, c) != VertexColorTarget::NONE) continue;
                set(std::string("pbr.vertexColor.") + channelFields[c], (int)(c == 3 ? VertexColorTarget::OPACITY : VertexColorTarget::ALBEDO_TINT));
            }
            continue;
        }
        if (vertexColor.use == VertexColorUse::OFF) set("pbr.vertexColor.use", (int)VertexColorUse::TINT);
        if ((ignored & vc::CHANNEL_A) && !vertexColor.alphaOpacity) set("pbr.vertexColor.alphaOpacity", true);
    }
    if (!found) return fail("no material '" + name + "' on " + scene->getEntityName(entity) + " now.");
    history->groupSince(mark, "Fix Vertex Color Material (" + name + ")");
    for (const vc::Warning& left : materialWarnings(project, sceneId, entity, ignored)) {
        if (left.message.rfind(open + name + close, 0) == 0) return fail(left.message);
    }
    return true;
}
