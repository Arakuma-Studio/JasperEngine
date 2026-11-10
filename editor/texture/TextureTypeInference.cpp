// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureTypeInference.h"

#include "Catalog.h"
#include "Project.h"
#include "asset/AssetIndex.h"
#include "asset/AssetFileWrite.h"
#include "shader/ShaderManifest.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool endsWith(const std::string& text, const std::string& suffix){
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    std::string lowerExtensionOf(const std::string& path){
        std::string extension = fs::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return extension;
    }

    // the part of a field locator after the scope: "MeshComponent.submeshes[0].material.normalTexture"
    std::string fieldOf(const DepEdge& edge){
        const FieldLocatorParts parts = parseFieldLocator(edge.field);
        if (!parts.component.empty())
            return parts.component + "." + parts.field;
        return parts.field.empty() ? edge.field : parts.field;
    }

    std::string describe(const DepEdge& edge, const Project& project){
        std::string where = fieldOf(edge);
        const std::string referrer = project.getAssetIndex().referrerPath(edge.referrer);
        if (!referrer.empty())
            where += " of " + referrer;
        return where;
    }

    // the component names of every entity of a scene or prefab file, by its file entity id
    std::map<uint64_t, std::set<std::string>> componentsOfFile(const fs::path& path){
        std::map<uint64_t, std::set<std::string>> out;
        try{
            const YAML::Node root = YAML::LoadFile(path.string());
            std::function<void(const YAML::Node&)> visit = [&](const YAML::Node& entities){
                if (!entities || !entities.IsSequence())
                    return;
                for (const YAML::Node& entity : entities){
                    if (!entity.IsMap() || !entity["entity"])
                        continue;
                    const uint64_t id = entity["entity"].as<uint64_t>(0);
                    if (const YAML::Node components = entity["components"]){
                        for (const auto& component : components)
                            out[id].insert(component.first.as<std::string>());
                    }
                    visit(entity["children"]);
                    visit(entity["entities"]);
                }
            };
            visit(root["entities"]);
        }catch (const std::exception&){
            out.clear();
        }
        return out;
    }

    // the slots a glTF's materials use an image in (its normalTexture, occlusionTexture...)
    std::vector<TextureSlot> gltfSlots(const fs::path& gltf, const std::string& imageRelativeToModel){
        std::vector<TextureSlot> slots;
        std::string text;
        if (lowerExtensionOf(gltf.string()) != ".gltf" || !assetio::readFile(gltf, text))
            return slots;
        const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
        if (document.is_discarded() || !document.is_object())
            return slots;
        const std::string wanted = fs::path(imageRelativeToModel).lexically_normal().generic_string();
        std::set<int> images;
        if (document.contains("images") && document["images"].is_array()){
            for (size_t i = 0; i < document["images"].size(); i++){
                const nlohmann::json& image = document["images"][i];
                if (image.is_object() && image.contains("uri") && image["uri"].is_string() &&
                    fs::path(image["uri"].get<std::string>()).lexically_normal().generic_string() == wanted)
                    images.insert((int)i);
            }
        }
        std::set<int> textures;
        if (document.contains("textures") && document["textures"].is_array()){
            for (size_t t = 0; t < document["textures"].size(); t++){
                const nlohmann::json& texture = document["textures"][t];
                if (texture.is_object() && texture.contains("source") && texture["source"].is_number_integer() &&
                    images.count(texture["source"].get<int>()))
                    textures.insert((int)t);
            }
        }
        if (!document.contains("materials") || !document["materials"].is_array())
            return slots;
        auto uses = [&](const nlohmann::json& material, const char* slot, const char* within = nullptr){
            const nlohmann::json* scope = &material;
            if (within){
                if (!material.contains(within) || !material[within].is_object())
                    return false;
                scope = &material[within];
            }
            return scope->contains(slot) && (*scope)[slot].is_object() && (*scope)[slot].contains("index") &&
                   (*scope)[slot]["index"].is_number_integer() && textures.count((*scope)[slot]["index"].get<int>()) > 0;
        };
        for (const nlohmann::json& material : document["materials"]){
            if (!material.is_object())
                continue;
            if (uses(material, "normalTexture")) slots.push_back(TextureSlot::Normal);
            if (uses(material, "occlusionTexture")) slots.push_back(TextureSlot::Occlusion);
            if (uses(material, "metallicRoughnessTexture", "pbrMetallicRoughness")) slots.push_back(TextureSlot::LinearData);
            if (uses(material, "baseColorTexture", "pbrMetallicRoughness") || uses(material, "emissiveTexture"))
                slots.push_back(TextureSlot::Color);
        }
        return slots;
    }

}

const char* TextureTypeInference::slotName(TextureSlot slot){
    switch (slot){
        case TextureSlot::Normal: return "normal";
        case TextureSlot::Occlusion: return "occlusion";
        case TextureSlot::LinearData: return "linear data";
        case TextureSlot::Sprite: return "sprite";
        case TextureSlot::UI: return "ui";
        case TextureSlot::CpuReadable: return "CPU-read";
        case TextureSlot::Cube: return "cubemap";
        case TextureSlot::SourceFile: return "source file";
        case TextureSlot::Color: default: return "colour";
    }
}

std::vector<TextureSlot> TextureTypeInference::classify(const DepEdge& edge, const EdgeContext& context){
    std::vector<TextureSlot> slots;
    const std::string field = fieldOf(edge);
    const FieldLocatorParts parts = parseFieldLocator(edge.field);

    if (edge.kind == DepEdgeKind::UIImage){
        // HTML src / CSS url(): RmlUi reads the file's pixels itself
        slots.push_back(TextureSlot::UI);
        slots.push_back(TextureSlot::SourceFile);
        return slots;
    }
    if (edge.kind == DepEdgeKind::Other && context.referrerPath && lowerExtensionOf(context.referrerPath(edge.referrer)) == ".tileset"){
        // a tileset's image (S50): its tiles draw as sprites do, so it imports as a sprite sheet
        // does (no mips; point filtering in Pixel Art Mode)
        slots.push_back(TextureSlot::Sprite);
        return slots;
    }
    if (edge.kind == DepEdgeKind::ModelUri){
        // a glTF's external image, by the slots of the materials that sample it: the model loader
        // takes its cook by path (MeshSystem), so the source need not ship
        if (context.modelSlots){
            const std::vector<TextureSlot> model = context.modelSlots(edge);
            slots.insert(slots.end(), model.begin(), model.end());
        }
        if (slots.empty())
            slots.push_back(TextureSlot::Color);
        return slots;
    }

    // manifest textures: what the shader's @texture line says of the uniform
    static const std::regex shaderTexture(R"(shaderTextures\{([^}]+)\}$)");
    std::smatch match;
    if (std::regex_search(field, match, shaderTexture)){
        bool cube = false;
        const TextureSlot slot = context.shaderTextureKind ? context.shaderTextureKind(edge, match[1].str(), cube) : TextureSlot::Color;
        slots.push_back(slot);
        if (cube)
            slots.push_back(TextureSlot::Cube);
        return slots;
    }

    if (endsWith(field, "normalTexture")){
        slots.push_back(TextureSlot::Normal);
    }else if (endsWith(field, "occlusionTexture")){
        slots.push_back(TextureSlot::Occlusion);
    }else if (endsWith(field, "metallicRoughnessTexture") || endsWith(field, "roughnessTexture") ||
               endsWith(field, "metallicTexture") || endsWith(field, "heightTexture")){
        slots.push_back(TextureSlot::LinearData);
    }else if (parts.component == "UIComponent" || parts.component == "ButtonComponent" || parts.component == "ImageComponent"){
        slots.push_back(TextureSlot::UI);
    }else if (parts.component == "PointsComponent"){
        slots.push_back(TextureSlot::Sprite);
    }else if (parts.component == "TerrainComponent" && (parts.field == "heightMap" || endsWith(parts.field, "densityMap"))){
        slots.push_back(TextureSlot::LinearData);
        slots.push_back(TextureSlot::CpuReadable);
    }else if (parts.component == "TerrainComponent" && parts.field.rfind("blendMap", 0) == 0){
        // splat weights: data, filtered per channel
        slots.push_back(TextureSlot::LinearData);
    }else if (parts.component == "LightComponent" && parts.field == "spotMask"){
        slots.push_back(TextureSlot::CpuReadable);
    }else if (field == "postProcessing.lutTexture" || endsWith(field, "lutTexture")){
        slots.push_back(TextureSlot::CpuReadable);
    }else{
        slots.push_back(TextureSlot::Color);
    }

    // terrain surface layers are read from their source file to build the detail array
    if (parts.component == "TerrainComponent" && parts.field.find("surfaceLayers") == 0)
        slots.push_back(TextureSlot::SourceFile);

    // a mesh texture of a sprite, tilemap or particle entity
    if (parts.component == "MeshComponent" && parts.hasEntity && context.componentsOf){
        const std::set<std::string> components = context.componentsOf(edge.referrer, parts.entity);
        static const std::string sprite = Catalog::getComponentName(ComponentType::SpriteComponent, true);
        static const std::string tilemap = Catalog::getComponentName(ComponentType::TilemapComponent, true);
        static const std::string particles = Catalog::getComponentName(ComponentType::ParticleSystemComponent, true);
        if (components.count(sprite) || components.count(tilemap) || components.count(particles))
            slots.push_back(TextureSlot::Sprite);
    }
    return slots;
}

TextureInference TextureTypeInference::fromUses(const std::vector<TextureReferenceUse>& uses, const std::string& fileName){
    TextureInference out;
    out.uses = uses;
    const std::string extension = lowerExtensionOf(fileName);
    const TextureReferenceUse* normal = nullptr;
    const TextureReferenceUse* sprite = nullptr;
    const TextureReferenceUse* ui = nullptr;
    const TextureReferenceUse* occlusion = nullptr;
    const TextureReferenceUse* linear = nullptr;
    std::set<std::string> sourceReaders;
    for (const TextureReferenceUse& use : uses){
        switch (use.slot){
            case TextureSlot::Normal: if (!normal) normal = &use; break;
            case TextureSlot::Sprite: if (!sprite) sprite = &use; break;
            case TextureSlot::UI: if (!ui) ui = &use; break;
            case TextureSlot::Occlusion: if (!occlusion) occlusion = &use; break;
            case TextureSlot::LinearData: if (!linear) linear = &use; break;
            case TextureSlot::Cube: out.shape = TextureShape::Cube; break;
            case TextureSlot::SourceFile: sourceReaders.insert(use.where); break;
            case TextureSlot::CpuReadable:
                if (!out.cpuReadable){
                    out.cpuReadable = true;
                    out.cpuReason = use.where;
                }
                break;
            case TextureSlot::Color: default: break;
        }
    }
    out.readsSource = !sourceReaders.empty();
    // every reference reads the source file itself: the export keeps it and cooks nothing
    out.sourceOnly = !uses.empty() && std::all_of(uses.begin(), uses.end(), [&](const TextureReferenceUse& use){
        return sourceReaders.count(use.where) > 0;
    });

    if (extension == ".hdr" || extension == ".exr"){
        out.type = TextureImportType::HDR;
        out.reason = extension + " source";
        out.linear = true;
    }else if (normal){
        out.type = TextureImportType::Normal;
        out.reason = "normal slot: " + normal->where;
        out.linear = true;
    }else if (sprite){
        out.type = TextureImportType::Sprite;
        out.reason = "sprite: " + sprite->where;
    }else if (ui){
        out.type = TextureImportType::UI;
        out.reason = "UI image: " + ui->where;
    }else if (occlusion){
        out.type = TextureImportType::SingleChannel;
        out.reason = "occlusion slot: " + occlusion->where;
        out.linear = true;
    }else{
        out.type = TextureImportType::Default;
        out.reason = uses.empty() ? "no references" : "colour: " + uses.front().where;
        if (linear){
            out.linear = true;
            out.reason = "linear data slot: " + linear->where;
        }
    }
    return out;
}

std::vector<DepEdge> TextureTypeInference::referenceEdges(Project& project, const std::string& projectRelative){
    const AssetDatabase::Resolution resolution = project.getAssetDatabase().lookupPath(projectRelative);
    return project.getAssetIndex().referencesTo(resolution.found() ? resolution.guid : Guid(), projectRelative, false, 4096);
}

TextureInference TextureTypeInference::infer(Project& project, const std::string& projectRelative){
    return fromEdges(project, projectRelative, referenceEdges(project, projectRelative));
}

TextureInference TextureTypeInference::fromEdges(const Project& project, const std::string& projectRelative, const std::vector<DepEdge>& edges){
    const AssetIndex& index = project.getAssetIndex();

    std::map<Guid, std::map<uint64_t, std::set<std::string>>> componentCache;
    EdgeContext context;
    context.componentsOf = [&](const Guid& referrer, uint64_t entity) -> std::set<std::string> {
        auto it = componentCache.find(referrer);
        if (it == componentCache.end()){
            const std::string path = index.referrerPath(referrer);
            it = componentCache.emplace(referrer, path.empty() ? std::map<uint64_t, std::set<std::string>>()
                                                               : componentsOfFile(project.getProjectPath() / path)).first;
        }
        auto found = it->second.find(entity);
        return found == it->second.end() ? std::set<std::string>() : found->second;
    };
    context.shaderTextureKind = [&](const DepEdge& edge, const std::string& uniform, bool& cube) -> TextureSlot {
        cube = false;
        // the shader of the same material or component: its edge shares the field's prefix
        const std::string prefix = edge.field.substr(0, edge.field.find("shaderTextures{"));
        for (const DepEdge& sibling : index.fileEdgesOf(edge.referrer)){
            if (sibling.kind != DepEdgeKind::MaterialShader && sibling.field != prefix + "customShader" && sibling.field != prefix + "shader")
                continue;
            if (sibling.field.compare(0, prefix.size(), prefix) != 0)
                continue;
            std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(sibling.targetPath);
            if (!manifest)
                continue;
            for (const ShaderManifestTexture& texture : manifest->textures){
                if (texture.uniform != uniform)
                    continue;
                cube = texture.cube;
                return texture.defaultTexture == ShaderTextureDefault::NORMAL ? TextureSlot::Normal : TextureSlot::Color;
            }
        }
        return TextureSlot::Color;
    };
    context.referrerPath = [&](const Guid& referrer){ return index.referrerPath(referrer); };
    context.modelSlots = [&](const DepEdge& edge) -> std::vector<TextureSlot> {
        const std::string model = index.referrerPath(edge.referrer);
        if (model.empty())
            return {};
        const fs::path modelPath = project.getProjectPath() / model;
        const fs::path relative = (project.getProjectPath() / projectRelative).lexically_relative(modelPath.parent_path());
        return gltfSlots(modelPath, relative.generic_string());
    };

    std::vector<TextureReferenceUse> uses;
    // a cube face of six files is a 2D texture; one file in a cube slot is a cross
    for (const DepEdge& edge : edges){
        for (TextureSlot slot : classify(edge, context))
            uses.push_back({slot, describe(edge, project)});
        const FieldLocatorParts parts = parseFieldLocator(edge.field);
        if ((parts.component == "SkyboxComponent" || parts.component == "ReflectionProbeComponent") && parts.field == "texture"){
            // the other faces of this field point elsewhere: count every edge of the referrer's field
            int faces = 0;
            for (const DepEdge& sibling : index.fileEdgesOf(edge.referrer))
                faces += sibling.field == edge.field ? 1 : 0;
            if (faces <= 1)
                uses.push_back({TextureSlot::Cube, describe(edge, project)});
        }
    }
    // a cube slot's file is a cross (4:3 or 3:4); anything else is a face of six files (an edit
    // the index has not seen yet, an unsaved scene): a 2D texture
    const bool cube = std::any_of(uses.begin(), uses.end(), [](const TextureReferenceUse& use){ return use.slot == TextureSlot::Cube; });
    int width = 0, height = 0, channels = 0;
    // (the ratio test of TextureData::splitCubeMapCross)
    if (cube && stbi_info((project.getProjectPath() / projectRelative).string().c_str(), &width, &height, &channels) && height > 0 &&
            (uint64_t)width * 100 / (uint64_t)height != 133 && (uint64_t)width * 100 / (uint64_t)height != 75){
        uses.erase(std::remove_if(uses.begin(), uses.end(), [](const TextureReferenceUse& use){ return use.slot == TextureSlot::Cube; }),
                   uses.end());
    }
    return fromUses(uses, projectRelative);
}

std::vector<std::string> TextureTypeInference::conflicts(TextureImportType type, const TextureInference& references){
    std::vector<std::string> out;
    for (const TextureReferenceUse& use : references.uses){
        const bool colourSlot = use.slot == TextureSlot::Color;
        if (type == TextureImportType::Normal && colourSlot)
            out.push_back("a colour slot binds this normal map: " + use.where);
        else if (type != TextureImportType::Normal && use.slot == TextureSlot::Normal)
            out.push_back(std::string("a normal slot binds this ") + textureImportTypeName(type) + " texture: " + use.where);
        else if (type == TextureImportType::SingleChannel && (colourSlot || use.slot == TextureSlot::Sprite || use.slot == TextureSlot::UI))
            out.push_back("a colour slot binds this single-channel texture: " + use.where);
        else if (type == TextureImportType::HDR && (use.slot == TextureSlot::UI || use.slot == TextureSlot::Sprite))
            out.push_back("an HDR texture is used as a sprite or UI image: " + use.where);
    }
    return out;
}
