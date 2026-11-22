// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExtractSun.h"

#include "lighting/HdriSun.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "component/LightComponent.h"
#include "component/SkyboxComponent.h"
#include "component/Transform.h"
#include "math/Quaternion.h"
#include "texture/TextureData.h"
#include "texture/TextureImportSettings.h"
#include "util/Color.h"
#include "util/FileUtils.h"
#include "util/LightPhotometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // one decoded sky image as linear RGBA floats
    struct ExtractSunImage {
        std::vector<float> rgba;
        int width = 0;
        int height = 0;
        bool hdr = false;
    };

    float extractSunSRGBToLinear(uint8_t value){
        const float c = (float)value / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    bool decodeExtractSunImage(const std::filesystem::path& path, ExtractSunImage& out, std::string& error){
        TextureData image;
        if (!image.loadTextureFromFile(path.generic_string().c_str()) || !image.getData()){
            error = "cannot decode " + path.filename().generic_string();
            return false;
        }
        image.setDataOwned(true);
        out.width = (int)image.getWidth();
        out.height = (int)image.getHeight();
        const size_t texels = (size_t)out.width * (size_t)out.height;
        out.rgba.resize(texels * 4);
        if (image.getColorFormat() == ColorFormat::RGBA32F){
            const float* pixels = (const float*)image.getData();
            std::copy(pixels, pixels + texels * 4, out.rgba.begin());
            out.hdr = true;
        }else{
            // an LDR image: its sRGB code values as linear light (a sun clipped at 1 is rarely distinct)
            std::vector<uint8_t> rgba8;
            if (!image.toRgba8(rgba8) || rgba8.size() < texels * 4){
                error = "cannot read the texels of " + path.filename().generic_string();
                return false;
            }
            for (size_t i = 0; i < texels * 4; i++)
                out.rgba[i] = (i % 4 == 3) ? (float)rgba8[i] / 255.0f : extractSunSRGBToLinear(rgba8[i]);
        }
        image.releaseImageData();
        return true;
    }

    // the scene's main Directional Light: its brightest (as drawn), the first on a tie
    Entity extractSunMainLight(Scene* scene){
        auto lights = scene->getComponentArray<LightComponent>();
        Entity best = NULL_ENTITY;
        float brightest = -1.0f;
        for (size_t i = 0; i < lights->size(); i++){
            const LightComponent& light = lights->getComponentFromIndex(i);
            if (light.type != LightType::DIRECTIONAL)
                continue;
            const float engine = LightPhotometry::toEngineIntensity(light.intensity, light.intensityUnit, scene->getPhysicalLightEV100());
            const float brightness = engine * (0.2126f * light.color.x + 0.7152f * light.color.y + 0.0722f * light.color.z);
            if (brightness > brightest){
                brightest = brightness;
                best = lights->getEntity(i);
            }
        }
        return best;
    }

}

bool ExtractSun::apply(Project* project, uint32_t sceneId, bool removeSunDisc, ExtractSunResult& out, std::string& error){
    out = ExtractSunResult();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene){
        error = "The scene is not open: open_scene it first.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    auto skies = scene->getComponentArray<SkyboxComponent>();
    if (skies->size() == 0){
        error = "The scene has no Skybox: create_entity type skybox with an HDR (.hdr, .exr) texture first.";
        return false;
    }
    // the scene's sky is its first Skybox (SkyboxComponent.h)
    const SkyboxComponent& sky = skies->getComponentFromIndex(0);

    // its image files: a cross in one file, or six faces (+X, -X, +Y, -Y, +Z, -Z)
    std::vector<std::string> paths;
    for (size_t f = 0; f < 6; f++)
        paths.push_back(sky.texture.getPath(f));
    const bool cross = !paths[0].empty() && std::all_of(paths.begin() + 1, paths.end(), [](const std::string& p){ return p.empty(); });
    const bool faces = std::none_of(paths.begin(), paths.end(), [](const std::string& p){ return p.empty(); });
    if (!cross && !faces){
        error = "The Skybox's texture is not an image file of the project (the default sky has no sun): set its Texture to an HDR "
                "(.hdr, .exr) cubemap cross or six faces first.";
        return false;
    }
    if (cross)
        paths.resize(1);

    std::vector<ExtractSunImage> decoded(paths.size());
    std::vector<hdrisun::SkyImage> images;
    bool hdr = true;
    for (size_t i = 0; i < paths.size(); i++){
        const std::filesystem::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(paths[i]);
        if (!decodeExtractSunImage(absolute, decoded[i], error)){
            error = "Extract Sun: " + error + ".";
            return false;
        }
        hdr = hdr && decoded[i].hdr;
        ExtractSunImage& image = decoded[i];
        if (cross){
            if (!hdrisun::viewsOf(image.rgba.data(), image.width, image.height, true, images)){
                error = "Extract Sun: " + paths[i] + " is not a 4:3 or 3:4 cubemap cross.";
                return false;
            }
        }else{
            hdrisun::SkyImage view;
            view.rgba = image.rgba.data();
            view.width = image.width;
            view.height = image.height;
            view.layout = hdrisun::SkyLayout::CubeFace;
            view.face = (int)i;
            images.push_back(view);
        }
    }
    if (removeSunDisc && !hdr){
        error = "Remove Sun Disc needs an HDR sky (.hdr, .exr images): an 8-bit image clips the sun. Extract the sun without it.";
        return false;
    }

    hdrisun::SunResult sun;
    if (!hdrisun::findSun(images, sun, error)){
        error = "Extract Sun: " + error + ".";
        return false;
    }

    // as the sky draws: turned about Y by its rotation (RenderSystem::skyViewProjection) and
    // tinted by its colour (the IBL's environment colour)
    const Vector3 towardsSun = (Quaternion(0.0f, sky.rotation, 0.0f) * sun.direction).normalized();
    const Vector3 tint = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
    const Vector3 irradiance(sun.irradiance.x * tint.x, sun.irradiance.y * tint.y, sun.irradiance.z * tint.z);
    const float engineIntensity = std::max(irradiance.x, std::max(irradiance.y, irradiance.z));
    if (!(engineIntensity > 0.0f)){
        error = "Extract Sun: no distinct sun: its disc is no brighter than the sky around it (or the Skybox's Color is black).";
        return false;
    }
    out.direction = -towardsSun;
    out.color = irradiance / engineIntensity;
    out.engineIntensity = engineIntensity;
    out.peakRatio = sun.peakRatio;
    out.discDegrees = sun.discDegrees;

    // Remove Sun Disc on every image of the sky (a face without the disc cooks as it was)
    CommandHistory* projectHistory = project->getProjectCommandHistory();
    InteractiveEdit metaEdit;
    bool metaWritten = false;
    if (removeSunDisc){
        metaEdit = projectHistory->beginInteractive("Remove Sun Disc");
        for (const std::string& path : paths){
            const std::filesystem::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(path);
            const YAML::Node stored = SetAssetMetaCmd::read(absolute, TextureImportSettings::metaKey);
            YAML::Node node = stored && stored.IsMap() ? YAML::Clone(stored) : YAML::Node(YAML::NodeType::Map);
            if (!node["version"]) node["version"] = TextureImportSettings::currentVersion;
            node["removeSunDisc"] = true;
            TextureImportSettings parsed;
            if (!TextureImportSettings::parse(node, parsed, &error)){
                metaEdit.cancel();
                error = path + ": " + error;
                return false;
            }
            Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(absolute), TextureImportSettings::metaKey, parsed.serialize());
            if (command->isNoOp()){
                delete command;
                continue;
            }
            if (!projectHistory->addCommand(command)){
                metaEdit.cancel();
                error = "Couldn't write the import settings of " + path + ".";
                return false;
            }
            metaWritten = true;
            out.sunDiscTextures.push_back(project->normalizeToProjectRelative(absolute).generic_string());
        }
    }

    const float ev100 = scene->getPhysicalLightEV100();
    CommandHistory* sceneHistory = CommandHandle::get(sceneId);
    const Entity existing = extractSunMainLight(scene);
    bool sceneWritten = false;
    if (existing == NULL_ENTITY){
        auto* create = new CreateEntityCmd(project, sceneId, "Directional Light", EntityCreationType::DIRECTIONAL_LIGHT);
        create->addProperty<Vector3>(ComponentType::Transform, "position", Vector3(0.0f, 10.0f, 0.0f));
        create->addProperty<Vector3>(ComponentType::LightComponent, "direction", out.direction);
        create->addProperty<Vector3>(ComponentType::LightComponent, "color", out.color);
        create->addProperty<float>(ComponentType::LightComponent, "intensity", engineIntensity);
        // the point of a sun of its own: it casts the shadows the sky's IBL cannot
        create->addProperty<bool>(ComponentType::LightComponent, "shadows", true);
        sceneWritten = sceneHistory->addCommandNoMerge(create);
        if (sceneWritten){
            out.light = create->getEntity();
            out.created = true;
        }
        out.unit = LightUnit::ENGINE;
        out.intensity = engineIntensity;
    }else{
        const LightComponent& light = scene->getComponent<LightComponent>(existing);
        const Transform* transform = scene->findComponent<Transform>(existing);
        // its direction is local: through its GameObject's world rotation
        const Vector3 local = transform ? (transform->worldRotation.inverse() * out.direction).normalized() : out.direction;
        out.unit = LightPhotometry::validUnit(light.intensityUnit, LightType::DIRECTIONAL);
        out.intensity = LightPhotometry::fromEngineIntensity(engineIntensity, out.unit, ev100);
        auto* multi = new MultiPropertyCmd();
        multi->addPropertyCmd<Vector3>(project, sceneId, existing, ComponentType::LightComponent, "direction", local);
        multi->addPropertyCmd<Vector3>(project, sceneId, existing, ComponentType::LightComponent, "color", out.color);
        if (out.unit != light.intensityUnit)
            multi->addPropertyCmd<int>(project, sceneId, existing, ComponentType::LightComponent, "intensityUnit", (int)out.unit);
        multi->addPropertyCmd<float>(project, sceneId, existing, ComponentType::LightComponent, "intensity", out.intensity);
        // the HDRI's colour, not a black body's
        if (light.useColorTemperature)
            multi->addPropertyCmd<bool>(project, sceneId, existing, ComponentType::LightComponent, "useColorTemperature", false);
        sceneWritten = sceneHistory->addCommandNoMerge(multi);
        out.light = existing;
    }
    if (!sceneWritten){
        if (removeSunDisc) metaEdit.cancel();
        error = existing == NULL_ENTITY ? "Couldn't create the Directional Light." : "Couldn't align the Directional Light.";
        return false;
    }
    if (removeSunDisc) metaEdit.commit();
    // the import settings (project history) and the light (the scene's): one undo step
    if (metaWritten && sceneHistory->top() && projectHistory->top())
        UndoTimeline::link({projectHistory, sceneHistory});
    return true;
}
