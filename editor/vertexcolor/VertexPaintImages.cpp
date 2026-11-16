// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintImages.h"

#include "Project.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "subsystem/VertexColorScene.h"
#include "texture/TextureData.h"
#include "util/FileUtils.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;

namespace {

    // 8-bit pixels as the op engine's linear floats
    std::shared_ptr<vc::Image> imageOf(TextureData& data, bool colorData) {
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

    std::filesystem::path resolve(Project* project, const std::string& path) {
        const std::filesystem::path given = FileUtils::pathFromUtf8(path);
        if (given.is_absolute()) return given;
        std::error_code ec;
        const std::filesystem::path inProject = project->getProjectPath() / given;
        if (std::filesystem::exists(inProject, ec)) return inProject;
        return project->getAssetsPath() / given;
    }

    std::shared_ptr<vc::Image> constant(const Vector4& value) {
        auto image = std::make_shared<vc::Image>();
        image->width = 1;
        image->height = 1;
        image->pixels = {value};
        return image;
    }

}

std::shared_ptr<const vc::Image> editor::vertexcolor::loadPaintImage(Project* project, const std::string& assetPath, bool colorData, std::string* error) {
    if (!project || assetPath.empty()) {
        if (error) *error = "Choose a texture.";
        return nullptr;
    }
    const std::filesystem::path file = resolve(project, assetPath);
    TextureData data;
    if (!data.loadTextureFromFile(FileUtils::pathToUtf8(file).c_str())) {
        if (error) *error = assetPath + " could not be read as an image.";
        return nullptr;
    }
    std::shared_ptr<vc::Image> image = imageOf(data, colorData);
    if (!image && error) *error = assetPath + " is not an 8-bit RGBA or grayscale image.";
    return image;
}

const char* editor::vertexcolor::materialSlotName(MaterialSlot slot) {
    switch (slot) {
        case MaterialSlot::Albedo: return "Albedo";
        case MaterialSlot::Emissive: return "Emissive";
        case MaterialSlot::Occlusion: return "Occlusion";
    }
    return "Albedo";
}

std::shared_ptr<const vc::Image> editor::vertexcolor::materialImage(Project* project, uint32_t sceneId, const TargetRef& target, MaterialSlot slot, std::string* error) {
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
        switch (slot) {
            case MaterialSlot::Albedo:
                texture = &material.baseColorTexture;
                factor = material.baseColorFactor;
                break;
            case MaterialSlot::Emissive:
                texture = &material.emissiveTexture;
                factor = Vector4(material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z, 1.0f);
                break;
            case MaterialSlot::Occlusion:
                texture = &material.occlusionTexture;
                break;
        }
        std::shared_ptr<vc::Image> image;
        const bool colorData = slot != MaterialSlot::Occlusion;
        // the pixels the editor still holds, else the file again
        if (texture->hasData() && texture->getData().getData()) image = imageOf(texture->getData(), colorData);
        if (!image && !texture->getPath().empty()) {
            std::string why;
            std::shared_ptr<const vc::Image> loaded = loadPaintImage(project, texture->getPath(), colorData, &why);
            if (loaded) image = std::make_shared<vc::Image>(*loaded);
        }
        if (!image) return constant(factor);
        for (Vector4& p : image->pixels) p = Vector4(p.x * factor.x, p.y * factor.y, p.z * factor.z, p.w * factor.w);
        return image;
    }
    if (error) *error = "The target's material is not loaded.";
    return nullptr;
}
