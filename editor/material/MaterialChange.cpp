// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MaterialChange.h"

#include "MaterialPresets.h"
#include "command/type/MultiPropertyCmd.h"
#include "shader/ShaderManifest.h"

#include <type_traits>

using namespace jasperengine;

Material editor::applyMaterialPreset(const MaterialPreset& preset, const Material& material, const std::string& meshShader) {
    Material result = material;
    if (preset.apply) preset.apply(result, meshShader);
    // a lighting mode the preset's shader does not list would draw its default anyway: auto
    if (result.shader != material.shader && !result.shader.empty()) {
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(result.shader);
        if (!Catalog::supportsLighting(result.lighting, (manifest && manifest->present) ? manifest.get() : nullptr))
            result.lighting = MaterialLighting::AUTO;
    }
    return result;
}

std::string editor::meshShaderFor(Scene* scene, Entity entity) {
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (mesh && !mesh->customShader.empty()) return mesh->customShader;
    return scene ? scene->getDefaultCustomShader(ShaderType::MESH) : std::string();
}

bool editor::addMaterialChangeCommands(MultiPropertyCmd& multi, Project* project, uint32_t sceneId, Entity entity, unsigned int submesh,
                                       const Material& from, const Material& to, const std::function<void()>& onChanged) {
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material";
    if (from == to) return false;

    // a UV set or the name has no field path of its own: the whole material then
    if (from.name != to.name || from.baseColorTexCoord != to.baseColorTexCoord ||
            from.metallicRoughnessTexCoord != to.metallicRoughnessTexCoord || from.occlusionTexCoord != to.occlusionTexCoord ||
            from.emissiveTexCoord != to.emissiveTexCoord || from.normalTexCoord != to.normalTexCoord) {
        multi.addPropertyCmd<Material>(project, sceneId, entity, ComponentType::MeshComponent, prefix, to, onChanged);
        return true;
    }

    auto field = [&](const char* name, const auto& before, const auto& after) {
        using T = std::decay_t<decltype(after)>;
        if (before == after) return;
        if constexpr (std::is_enum_v<T>) {
            multi.addPropertyCmd<int>(project, sceneId, entity, ComponentType::MeshComponent, prefix + "." + name, (int)after, onChanged);
        } else {
            multi.addPropertyCmd<T>(project, sceneId, entity, ComponentType::MeshComponent, prefix + "." + name, after, onChanged);
        }
    };
    // the shader first: a property command reloads what the values after it follow
    field("shader", from.shader, to.shader);
    field("shaderParams", from.shaderParams, to.shaderParams);
    field("shaderTextures", from.shaderTextures, to.shaderTextures);
    field("shaderFeatures", from.shaderFeatures, to.shaderFeatures);
    field("baseColorFactor", from.baseColorFactor, to.baseColorFactor);
    field("metallicFactor", from.metallicFactor, to.metallicFactor);
    field("roughnessFactor", from.roughnessFactor, to.roughnessFactor);
    field("alphaCutoff", from.alphaCutoff, to.alphaCutoff);
    field("foliageTransmission", from.foliageTransmission, to.foliageTransmission);
    field("emissiveFactor", from.emissiveFactor, to.emissiveFactor);
    field("alphaMode", from.alphaMode, to.alphaMode);
    field("stochasticTiling", from.stochasticTiling, to.stochasticTiling);
    field("stochasticCellSize", from.stochasticCellSize, to.stochasticCellSize);
    field("stochasticBlendSharpness", from.stochasticBlendSharpness, to.stochasticBlendSharpness);
    field("stochasticContrast", from.stochasticContrast, to.stochasticContrast);
    field("stochasticRotation", from.stochasticRotation, to.stochasticRotation);
    field("baseColorTexture", from.baseColorTexture, to.baseColorTexture);
    field("emissiveTexture", from.emissiveTexture, to.emissiveTexture);
    field("metallicRoughnessTexture", from.metallicRoughnessTexture, to.metallicRoughnessTexture);
    field("occlusionTexture", from.occlusionTexture, to.occlusionTexture);
    field("normalTexture", from.normalTexture, to.normalTexture);
    field("blendMode", from.blendMode, to.blendMode);
    field("cullMode", from.cullMode, to.cullMode);
    field("depthWrite", from.depthWrite, to.depthWrite);
    field("depthTest", from.depthTest, to.depthTest);
    field("depthOffsetFactor", from.depthOffsetFactor, to.depthOffsetFactor);
    field("depthOffsetUnits", from.depthOffsetUnits, to.depthOffsetUnits);
    field("sortPriority", from.sortPriority, to.sortPriority);
    field("lighting", from.lighting, to.lighting);
    field("stencil", from.stencil, to.stencil);
    field("colorWrite", from.colorWrite, to.colorWrite);
    field("extraPass", *from.extraPass, *to.extraPass);
    return true;
}
