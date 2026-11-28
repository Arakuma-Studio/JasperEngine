// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintImages.h"

#include "Project.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "picking/PickGeometry.h"
#include "subsystem/VertexColorScene.h"
#include "texture/TextureData.h"
#include "texture/TextureImportSettings.h"
#include "util/FileUtils.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <filesystem>
#include <optional>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace {

    // 8-bit pixels as the op engine's linear floats
    std::shared_ptr<vc::Image> paintImageOf(TextureData& data, bool colorData) {
        const int width = data.getWidth();
        const int height = data.getHeight();
        const unsigned char* bytes = static_cast<const unsigned char*>(data.getData());
        if (width <= 0 || height <= 0 || !bytes) return nullptr;
        const ColorFormat format = data.getColorFormat();
        int channels = 0;
        if (format == ColorFormat::RGBA || format == ColorFormat::SRGB8A8) channels = 4;
        else if (format == ColorFormat::RED) channels = 1;
        else return nullptr;
        auto image = std::make_shared<vc::Image>();
        image->width = width;
        image->height = height;
        image->pixels.resize((size_t)width * (size_t)height);
        auto decode = [colorData](unsigned char c) {
            const float v = (float)c / 255.0f;
            return colorData ? vc::srgbToLinear(v) : v;
        };
        for (size_t i = 0; i < image->pixels.size(); i++) {
            const unsigned char* p = bytes + i * (size_t)channels;
            if (channels == 1) {
                const float v = decode(p[0]);
                image->pixels[i] = Vector4(v, v, v, 1.0f);
            } else {
                image->pixels[i] = Vector4(decode(p[0]), decode(p[1]), decode(p[2]), (float)p[3] / 255.0f);
            }
        }
        return image;
    }

    std::filesystem::path paintFileOf(Project* project, const std::string& path) {
        const std::filesystem::path given = FileUtils::pathFromUtf8(path);
        if (given.is_absolute()) return given;
        std::error_code ec;
        const std::filesystem::path inProject = project->getProjectPath() / given;
        if (std::filesystem::exists(inProject, ec)) return inProject;
        return project->getAssetsPath() / given;
    }

    // A model's map whose CPU pixels went with its upload (Texture::setReleaseDataAfterLoad): the
    // decoded image the Model still holds (ModelPool keeps its glTF), found through the target's
    // glTF primitive and its material's slot. Null for slots glTF has not, or merged meshes.
    std::shared_ptr<vc::Image> gltfImage(Scene* scene, const TargetRef& target, const std::string& slot, bool colorData) {
        const ModelComponent* model = scene->findComponent<ModelComponent>(target.entity);
        if (!model || !model->gltfModel || target.key.node < 0 || target.key.primitive < 0) return nullptr;
        const tinygltf::Model& gltf = *model->gltfModel;
        if ((size_t)target.key.node >= gltf.nodes.size()) return nullptr;
        const int meshIndex = gltf.nodes[(size_t)target.key.node].mesh;
        if (meshIndex < 0 || (size_t)meshIndex >= gltf.meshes.size()) return nullptr;
        const auto& primitives = gltf.meshes[(size_t)meshIndex].primitives;
        if ((size_t)target.key.primitive >= primitives.size()) return nullptr;
        const int materialIndex = primitives[(size_t)target.key.primitive].material;
        if (materialIndex < 0 || (size_t)materialIndex >= gltf.materials.size()) return nullptr;
        const tinygltf::Material& material = gltf.materials[(size_t)materialIndex];
        int textureIndex = -1;
        if (slot == "albedo") textureIndex = material.pbrMetallicRoughness.baseColorTexture.index;
        else if (slot == "metallic_roughness") textureIndex = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        else if (slot == "emissive") textureIndex = material.emissiveTexture.index;
        else if (slot == "occlusion") textureIndex = material.occlusionTexture.index;
        if (textureIndex < 0 || (size_t)textureIndex >= gltf.textures.size()) return nullptr;
        const int imageIndex = gltf.textures[(size_t)textureIndex].source;
        if (imageIndex < 0 || (size_t)imageIndex >= gltf.images.size()) return nullptr;
        const tinygltf::Image& decoded = gltf.images[(size_t)imageIndex];
        const int channels = decoded.component;
        if (decoded.bits != 8 || channels < 1 || channels > 4 || decoded.width <= 0 || decoded.height <= 0 ||
            decoded.image.size() < (size_t)decoded.width * (size_t)decoded.height * (size_t)channels)
            return nullptr;
        auto image = std::make_shared<vc::Image>();
        image->width = decoded.width;
        image->height = decoded.height;
        image->pixels.resize((size_t)decoded.width * (size_t)decoded.height);
        auto value = [colorData](unsigned char c) {
            const float v = (float)c / 255.0f;
            return colorData ? vc::srgbToLinear(v) : v;
        };
        for (size_t i = 0; i < image->pixels.size(); i++) {
            const unsigned char* p = decoded.image.data() + i * (size_t)channels;
            const float r = value(p[0]);
            const float g = channels >= 3 ? value(p[1]) : r;
            const float b = channels >= 3 ? value(p[2]) : r;
            const float a = channels == 4 ? (float)p[3] / 255.0f : (channels == 2 ? (float)p[1] / 255.0f : 1.0f);
            image->pixels[i] = Vector4(r, g, b, a);
        }
        return image;
    }

    // R41: color data is what the texture's import decodes as sRGB (its setting, or its type's default)
    bool importedAsColor(const std::filesystem::path& file) {
        TextureImportSettings settings;
        TextureImportSettings::load(FileUtils::pathToUtf8(file), settings);
        return settings.srgb.value_or(TextureImportSettings::defaultSrgb(settings.resolvedType()));
    }

    std::shared_ptr<vc::Image> constant(const Vector4& value) {
        auto image = std::make_shared<vc::Image>();
        image->width = 1;
        image->height = 1;
        image->pixels = {value};
        return image;
    }

}

std::shared_ptr<const vc::Image> loadPaintImage(Project* project, const std::string& assetPath, std::optional<bool> colorData, std::string* error) {
    if (!project || assetPath.empty()) {
        if (error) *error = "Choose a texture.";
        return nullptr;
    }
    const std::filesystem::path file = paintFileOf(project, assetPath);
    TextureData data;
    if (!data.loadTextureFromFile(FileUtils::pathToUtf8(file).c_str())) {
        if (error) *error = assetPath + " could not be read as an image.";
        return nullptr;
    }
    std::shared_ptr<vc::Image> image = paintImageOf(data, colorData.has_value() ? *colorData : importedAsColor(file));
    if (!image && error) *error = assetPath + " is not an 8-bit RGBA or grayscale image.";
    return image;
}

const char* materialSlotName(MaterialSlot slot) {
    switch (slot) {
        case MaterialSlot::Albedo: return "Albedo";
        case MaterialSlot::Emissive: return "Emissive";
        case MaterialSlot::Occlusion: return "Occlusion";
        case MaterialSlot::MetallicRoughness: return "Metallic Roughness";
    }
    return "Albedo";
}

std::shared_ptr<const vc::Image> materialImage(Project* project, uint32_t sceneId, const TargetRef& target, MaterialSlot slot, std::string* error,
                                                                    int* texCoord) {
    static const char* const names[] = {"albedo", "emissive", "occlusion", "metallic_roughness"};
    return materialImage(project, sceneId, target, std::string(names[(int)slot]), error, texCoord);
}

std::shared_ptr<const vc::Image> materialImage(Project* project, uint32_t sceneId, const TargetRef& target, const std::string& slot,
                                                                    std::string* error, int* texCoord) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (!scene) return nullptr;
    for (const vc::SceneTarget& sceneTarget : vc::sceneTargets(scene, target.entity)) {
        if (sceneTarget.key != target.key) continue;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(sceneTarget.meshEntity);
        if (!mesh || sceneTarget.submesh >= mesh->numSubmeshes) break;
        const Material& material = mesh->submeshes[sceneTarget.submesh].material;
        const Texture* texture = nullptr;
        Vector4 factor(1.0f, 1.0f, 1.0f, 1.0f);
        // unset: a shader texture, decoded as its import settings say
        std::optional<bool> colorData;
        int uv = 0;
        if (slot == "albedo") {
            texture = &material.baseColorTexture;
            factor = material.baseColorFactor;
            colorData = true;
            uv = material.baseColorTexCoord;
        } else if (slot == "emissive") {
            texture = &material.emissiveTexture;
            factor = Vector4(material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z, 1.0f);
            colorData = true;
            uv = material.emissiveTexCoord;
        } else if (slot == "occlusion") {
            texture = &material.occlusionTexture;
            colorData = false;
            uv = material.occlusionTexCoord;
        } else if (slot == "metallic_roughness") {
            texture = &material.metallicRoughnessTexture;
            colorData = false;
            uv = material.metallicRoughnessTexCoord;
        } else {
            std::string names;
            for (const auto& [name, named] : material.shaderTextures) {
                if (name == slot) texture = &named;
                names += ", " + name;
            }
            if (!texture) {
                if (error) *error = "\"" + slot + "\" is not a slot of " + sceneTarget.name + "'s material: albedo, emissive, occlusion, metallic_roughness" +
                                    names + ".";
                return nullptr;
            }
        }
        if (texCoord) *texCoord = std::clamp(uv, 0, 1);
        const std::string path = texture->empty() ? std::string() : texture->getPath();
        const bool color = colorData.has_value() ? *colorData : (!path.empty() && importedAsColor(paintFileOf(project, path)));
        std::shared_ptr<vc::Image> image;
        // the pixels the editor still holds, else the file again
        if (texture->hasData() && texture->getData().getData()) image = paintImageOf(texture->getData(), color);
        if (!image && !texture->empty()) image = gltfImage(scene, target, slot, color);
        if (!image && !path.empty()) {
            std::string why;
            std::shared_ptr<const vc::Image> loaded = loadPaintImage(project, path, color, &why);
            if (loaded) image = std::make_shared<vc::Image>(*loaded);
        }
        if (!image) {
            if (!texture->empty()) {
                if (error) *error = "the " + slot + " map of " + sceneTarget.name + " could not be read; give texture with the image file instead.";
                return nullptr;
            }
            return constant(factor);
        }
        for (Vector4& p : image->pixels) p = Vector4(p.x * factor.x, p.y * factor.y, p.z * factor.z, p.w * factor.w);
        return image;
    }
    if (error) *error = "The target's material is not loaded.";
    return nullptr;
}

bool pickTriangleRange(Scene* scene, Entity meshEntity, uint32_t submesh, uint32_t& first, uint32_t& count) {
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    if (!mesh) return false;
    PickGeometryLayout layout;
    picking::resolveSources(*mesh, layout);
    if (submesh >= layout.submeshes.size()) return false;
    first = 0;
    for (uint32_t i = 0; i < submesh; i++) {
        const PickSubmeshSource& source = layout.submeshes[i];
        if (source.valid) first += picking::primitiveTriangles(source.primitive, source.drawCount);
    }
    const PickSubmeshSource& own = layout.submeshes[submesh];
    // the op engine's views are triangle lists: a strip's numbers are not theirs
    if (!own.valid || own.primitive != PrimitiveType::TRIANGLES) return false;
    count = picking::primitiveTriangles(own.primitive, own.drawCount);
    return true;
}

}  // namespace jasperengine::editor::vertexcolor
