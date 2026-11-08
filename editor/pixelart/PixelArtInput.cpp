// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtInput.h"

#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteSwapChecks.h"
#include "pixelart/aseprite/AsepriteImport.h"
#include "shader/ShaderVariantCollector.h"
#include "sprite/SpriteAtlasDocument.h"
#include "texture/TextureCookQueue.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"

#include "component/CameraComponent.h"
#include "component/ImageComponent.h"
#include "component/MeshComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PixelPerfectCameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/SpriteComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "pool/ShaderPool.h"

#include "stb_image.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
namespace fs = std::filesystem;

namespace {

    struct WorldValues {
        Matrix4 model;
        Vector3 position, scale = Vector3(1, 1, 1);
        Quaternion rotation;
        bool done = false;
    };

    // the world values from the local ones and the parent chain (what computeWorldTransform gives,
    // without relying on the scene's matrices being up to date: a scene read from its file was
    // never updated)
    const WorldValues& worldOf(Scene* scene, Entity entity, std::map<Entity, WorldValues>& cache, int depth = 0){
        WorldValues& out = cache[entity];
        if (out.done)
            return out;
        out.done = true;
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform)
            return out;
        const Matrix4 local = Matrix4::composeTRS(transform->position, transform->rotation, transform->scale);
        if (transform->parent != NULL_ENTITY && depth < 256 && scene->findComponent<Transform>(transform->parent)){
            const WorldValues parent = worldOf(scene, transform->parent, cache, depth + 1);
            WorldValues& self = cache[entity];
            self.model = parent.model.affineMultiply(local);
            self.position = parent.model.transformAffine(transform->position);
            self.scale = parent.scale * transform->scale;
            self.rotation = parent.rotation * transform->rotation;
            return self;
        }
        out.model = local;
        out.position = transform->position;
        out.scale = transform->scale;
        out.rotation = transform->rotation;
        return out;
    }

    std::string entityPathOf(const SceneProject& scene, Entity entity, EntityPath::Cache& cache){
        const std::string path = EntityPath::of(scene, entity, &cache);
        return path.empty() || path[0] != '/' ? "/" + path : path;
    }

    Vector2 pivotPixels(const SpriteComponent& sprite, float width, float height){
        switch (sprite.pivotPreset){
            case PivotPreset::BOTTOM_RIGHT: return Vector2(width, 0);
            case PivotPreset::TOP_RIGHT: return Vector2(width, height);
            case PivotPreset::TOP_LEFT: return Vector2(0, height);
            case PivotPreset::TOP_CENTER: return Vector2(width / 2.0f, height);
            case PivotPreset::BOTTOM_CENTER: return Vector2(width / 2.0f, 0);
            case PivotPreset::RIGHT_CENTER: return Vector2(width, height / 2.0f);
            case PivotPreset::LEFT_CENTER: return Vector2(0, height / 2.0f);
            case PivotPreset::CENTER:
            case PivotPreset::FRAME: return Vector2(width / 2.0f, height / 2.0f);
            case PivotPreset::BOTTOM_LEFT: default: return Vector2(0, 0);
        }
    }

    const char* renderModeName(particles::RenderMode mode){
        switch (mode){
            case particles::RenderMode::FacingCameraPosition: return "facing_camera_position";
            case particles::RenderMode::StretchedBillboard: return "stretched_billboard";
            case particles::RenderMode::HorizontalBillboard: return "horizontal_billboard";
            case particles::RenderMode::VerticalBillboard: return "vertical_billboard";
            case particles::RenderMode::WorldOriented: return "world_oriented";
            case particles::RenderMode::LocalOriented: return "local_oriented";
            case particles::RenderMode::Mesh: return "mesh";
            case particles::RenderMode::None: return "none";
            case particles::RenderMode::VelocityAligned: return "velocity_aligned";
            case particles::RenderMode::Billboard: default: return "billboard";
        }
    }

    std::string readText(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::string lowerExtension(const std::string& path){
        std::string extension = fs::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return extension;
    }

    bool isFontPath(const std::string& path){
        const std::string e = lowerExtension(path);
        return e == ".ttf" || e == ".otf" || e == ".bitmapfont" || e == ".fnt";
    }

    // a texture's import settings, header and sheet facts, read once per path
    PixelArtTexture& textureEntry(Project& project, PixelArtCheckInput& input, const std::string& path){
        auto it = input.textures.find(path);
        if (it != input.textures.end())
            return it->second;
        PixelArtTexture& t = input.textures[path];
        t.path = path;
        const fs::path absolute = project.getAssetsPath() / fs::path(path);
        std::error_code ec;
        t.exists = fs::is_regular_file(absolute, ec);
        const std::string extension = lowerExtension(path);
        if (extension == ".aseprite" || extension == ".ase"){
            t.aseprite = true;
            const YAML::Node node = SetAssetMetaCmd::read(absolute, "import.asepriteImporter");
            aseprite::AsepriteImportSettings settings;
            std::vector<PixelProblem> problems;
            if (node && !node.IsNull())
                aseprite::AsepriteImportSettings::fromYaml(node, settings, problems);
            t.asepriteCompression = settings.compression == aseprite::SheetCompression::Project ? "project" : "none";
            t.pixelArt = true;
            return t;
        }
        if (extension == ".spriteatlas"){
            t.atlas = true;
            if (t.exists){
                const sprite::SpriteAtlasParseResult parsed = sprite::parseSpriteAtlas(readText(absolute));
                if (parsed.ok())
                    t.atlasCompression = parsed.document->settings.compression;
            }
            // Auto is lossless in Pixel Art Mode (S45 R16)
            if (t.atlasCompression == "auto" && !project.getPixelArtSettings().enabled)
                t.atlasCompression = "auto_platform";
            return t;
        }
        std::string error;
        t.hasSettings = TextureImportSettings::load(absolute.string(), t.settings, &error);
        // no import settings yet: Pixel Art Mode's first import writes pixelArt (R1), so the
        // texture is judged as it will import
        if (!t.hasSettings && project.getPixelArtSettings().enabled){
            t.settings.pixelArt = true;
            t.settings.type = TextureImportType::Sprite;
        }
        int width = 0, height = 0, channels = 0;
        if (t.exists && stbi_info(absolute.string().c_str(), &width, &height, &channels)){
            t.width = width;
            t.height = height;
            t.hasAlpha = channels == 2 || channels == 4;
        }
        return t;
    }

    void addDrawableTexture(Project& project, PixelArtCheckInput& input, PixelArtDrawable& d, bool pixelArtDrawable){
        const std::string path = d.sheet ? d.sheetPath : d.texture;
        if (path.empty())
            return;
        PixelArtTexture& t = textureEntry(project, input, path);
        if (pixelArtDrawable)
            t.pixelArt = true;
        if (d.paletteSwap){
            t.paletteSwap = true;
            if (!d.paletteSwapSource.empty())
                t.paletteSwapSource = d.paletteSwapSource;
        }
    }

    bool texturePixelArt(Project& project, PixelArtCheckInput& input, const PixelArtDrawable& d){
        const std::string path = d.sheet ? d.sheetPath : d.texture;
        if (path.empty())
            return false;
        const PixelArtTexture& t = textureEntry(project, input, path);
        return t.aseprite || (t.hasSettings && t.settings.resolvedPixelArt());
    }

    void readPaletteSwap(const Material& material, PixelArtDrawable& d){
        if (!paletteSwapEnabled(material))
            return;
        d.paletteSwap = true;
        for (const auto& [uniform, binding] : material.shaderPalettes.get()){
            if (!binding.source.path.empty()){
                d.paletteSwapSource = binding.source.path;
                break;
            }
        }
    }

    std::string spriteMaterialLabel(const Material& material){
        std::vector<std::string> features = material.shaderFeatures;
        std::sort(features.begin(), features.end());
        std::string label = "library:sprite";
        if (!features.empty()){
            label += " [";
            for (size_t i = 0; i < features.size(); i++)
                label += (i ? ", " : "") + features[i];
            label += "]";
        }
        label += " blend " + std::to_string((int)material.blendMode);
        return label;
    }

    void extractScene(Project& project, const SceneProject& sp, const PixelArtProjectInfo& info, const Scope& scope,
                      PixelArtCheckInput& input){
        Scene* scene = sp.scene;
        PixelArtScene out;
        out.path = scenePathOf(project, sp);
        out.name = sp.name;
        out.is2D = sp.sceneType == SceneType::SCENE_2D;
        out.fixedResolution = scene->isFixedResolutionEnabled();
        out.fixedWidth = (int)scene->getFixedResolutionWidth();
        out.fixedHeight = (int)scene->getFixedResolutionHeight();
        const PostProcessingSettings& post = scene->getPostProcessing();
        out.fxaa = post.fxaaEnabled;
        out.msaaSamples = post.msaaSamples;
        out.blur = post.blurEnabled && post.blurIntensity > 0.0f;

        std::map<Entity, WorldValues> world;
        EntityPath::Cache paths;
        const Entity cameraEntity = sp.mainCamera;
        if (cameraEntity != NULL_ENTITY && cameraEntity != sp.defaultCamera && scene->isEntityCreated(cameraEntity) &&
            std::find(sp.entities.begin(), sp.entities.end(), cameraEntity) != sp.entities.end()){
            if (const CameraComponent* camera = scene->findComponent<CameraComponent>(cameraEntity)){
                out.camera.exists = true;
                out.camera.entity = entityPathOf(sp, cameraEntity, paths);
                out.camera.entityId = cameraEntity;
                out.camera.perspective = camera->type == CameraType::CAMERA_PERSPECTIVE;
                const WorldValues& w = worldOf(scene, cameraEntity, world);
                Quaternion q = w.rotation;
                q.normalize();
                const Vector3 axisX = q * Vector3(1, 0, 0);
                out.camera.roll = (float)(std::atan2((double)axisX.y, (double)axisX.x) * 57.29577951308232);
                if (const Transform* transform = scene->findComponent<Transform>(cameraEntity)){
                    Quaternion local = transform->rotation;
                    local.normalize();
                    const Vector3 euler = local.getEulerAngles(RotationOrder::ZYX);
                    out.camera.rotationEuler = Vector3(std::fabs(euler.x) < 1e-4f ? 0.0f : euler.x, std::fabs(euler.y) < 1e-4f ? 0.0f : euler.y,
                                                       std::fabs(euler.z) < 1e-4f ? 0.0f : euler.z);
                }
                if (const PixelPerfectCameraComponent* ppc = scene->findComponent<PixelPerfectCameraComponent>(cameraEntity)){
                    out.camera.hasComponent = true;
                    out.camera.component = *ppc;
                }
            }
        }

        // the selection scope keeps only the selected GameObjects
        auto selected = [&](Entity entity){
            if (!scope.selectionOnly)
                return true;
            return std::find(scope.selection.begin(), scope.selection.end(), std::make_pair(sp.id, (uint32_t)entity)) != scope.selection.end();
        };

        std::vector<PixelArtDrawable> drawables;
        std::map<std::string, Entity> spriteMaterials;
        for (Entity entity : sp.entities){
            if (!scene->isEntityCreated(entity) || !selected(entity))
                continue;
            const Transform* transform = scene->findComponent<Transform>(entity);
            auto base = [&](DrawableKind kind){
                PixelArtDrawable d;
                d.kind = kind;
                d.scene = out.path;
                d.entity = entityPathOf(sp, entity, paths);
                d.entityId = entity;
                if (transform){
                    d.hasTransform = true;
                    d.position = transform->position;
                    d.rotation = transform->rotation;
                    d.scale = transform->scale;
                    const WorldValues& w = worldOf(scene, entity, world);
                    d.worldPosition = w.position;
                    d.worldRotation = w.rotation;
                    d.worldScale = w.scale;
                    if (transform->parent != NULL_ENTITY && scene->findComponent<Transform>(transform->parent)){
                        const WorldValues& p = worldOf(scene, transform->parent, world);
                        d.parentWorldScale = p.scale;
                        d.parentWorldRotation = p.rotation;
                    }
                }
                return d;
            };
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity); sprite && mesh && mesh->numSubmeshes > 0){
                PixelArtDrawable d = base(DrawableKind::Sprite);
                const Material& material = mesh->submeshes[0].material;
                d.textureField = "mesh.submeshes[0].material.baseColorTexture";
                d.texture = material.baseColorTexture.getPath();
                d.minFilter = material.baseColorTexture.getMinFilter();
                d.magFilter = material.baseColorTexture.getMagFilter();
                if (!sprite->sheet.path.empty()){
                    d.sheet = true;
                    d.sheetPath = sprite->sheet.path;
                    d.texture.clear();
                }
                d.pixelsPerUnit = sprite->pixelsPerUnit;
                d.pixelRotation = sprite->pixelRotation;
                float width = (float)sprite->width, height = (float)sprite->height;
                if ((width <= 0 || height <= 0) && !d.texture.empty()){
                    const PixelArtTexture& t = textureEntry(project, input, d.texture);
                    width = (float)t.width;
                    height = (float)t.height;
                }
                d.width = width;
                d.height = height;
                d.pivot = pivotPixels(*sprite, width, height);
                readPaletteSwap(material, d);
                if (material.shader.path == "library:sprite"){
                    d.spriteMaterial = spriteMaterialLabel(material);
                    spriteMaterials.emplace(d.spriteMaterial, entity);
                }
                drawables.push_back(std::move(d));
            }else if (const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity); tilemap && mesh && mesh->numSubmeshes > 0){
                PixelArtDrawable d = base(DrawableKind::Tilemap);
                const Material& material = mesh->submeshes[0].material;
                d.textureField = "mesh.submeshes[0].material.baseColorTexture";
                d.texture = material.baseColorTexture.getPath();
                d.minFilter = material.baseColorTexture.getMinFilter();
                d.magFilter = material.baseColorTexture.getMagFilter();
                d.pixelsPerUnit = tilemap->pixelsPerUnit;
                readPaletteSwap(material, d);
                drawables.push_back(std::move(d));
            }
            if (scene->findComponent<ImageComponent>(entity)){
                const UIComponent* ui = scene->findComponent<UIComponent>(entity);
                const ImageComponent* image = scene->findComponent<ImageComponent>(entity);
                if (ui && !ui->texture.empty() && !ui->texture.isFramebuffer()){
                    PixelArtDrawable d = base(DrawableKind::Image);
                    d.textureField = "ui.texture";
                    d.texture = ui->texture.getPath();
                    d.minFilter = ui->texture.getMinFilter();
                    d.magFilter = ui->texture.getMagFilter();
                    if (const RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity)){
                        d.rectWidth = (float)rect->width;
                        d.rectHeight = (float)rect->height;
                    }
                    d.sliced = image->patchMarginLeft || image->patchMarginRight || image->patchMarginTop || image->patchMarginBottom;
                    drawables.push_back(std::move(d));
                }
            }
            if (const TextComponent* text = scene->findComponent<TextComponent>(entity)){
                PixelArtDrawable d = base(DrawableKind::Text);
                d.font = text->font[0].path;
                d.fontSize = text->fontSize;
                drawables.push_back(std::move(d));
            }
            if (const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity)){
                PixelArtDrawable d = base(DrawableKind::Particles);
                particles::ParticleEffectDocument document = particles->inlineEffect;
                if (!particles->effect.path.empty()){
                    d.particleEffectAsset = true;
                    d.particleEffect = particles->effect.path;
                    std::vector<particles::Diagnostic> diagnostics;
                    particles::ParticleEffectDocument loaded;
                    if (particles::ParticleEffectCodec::read(readText(project.getAssetsPath() / fs::path(particles->effect.path)), loaded, diagnostics))
                        document = loaded;
                }
                d.texture = document.renderer.texture;
                d.particleRenderMode = renderModeName(document.renderer.renderMode);
                const particles::MinMaxCurve& size = document.main.startSize;
                if (size.mode == particles::CurveMode::Constant){
                    d.particleSizes.push_back(size.constant);
                }else if (size.mode == particles::CurveMode::TwoConstants){
                    d.particleSizes.push_back(size.constant);
                    d.particleSizes.push_back(size.constantMax);
                }
                drawables.push_back(std::move(d));
            }
        }

        // R9: a scene drawn through a Pixel Perfect Camera, a 2D scene of a Pixel Art Mode
        // project, or one that draws pixel-art textures
        out.pixelArt = (out.camera.exists && out.camera.hasComponent && out.camera.component.enabled && !out.camera.perspective) ||
                       ((info.modeEnabled || info.assumeMode) && out.is2D);
        bool anyPixelTexture = false;
        for (const PixelArtDrawable& d : drawables)
            anyPixelTexture = anyPixelTexture || texturePixelArt(project, input, d);
        out.pixelArt = out.pixelArt || anyPixelTexture;
        for (PixelArtDrawable& d : drawables){
            const bool pixel = out.pixelArt || texturePixelArt(project, input, d);
            if (!pixel)
                continue;
            if (d.kind != DrawableKind::Particles && d.kind != DrawableKind::Text)
                addDrawableTexture(project, input, d, true);
            out.drawables.push_back(std::move(d));
        }
        // a 2D scene of sprites or tilemaps is pixel-art content for pixel_art_mode even before the mode is on
        for (const PixelArtDrawable& d : drawables)
            out.drawsSprites = out.drawsSprites || (out.is2D && (d.kind == DrawableKind::Sprite || d.kind == DrawableKind::Tilemap));

        // sprite_variants: library:sprite materials, one representative GameObject each
        if (out.pixelArt){
            for (const auto& [label, entity] : spriteMaterials){
                ShaderVariantContent content;
                content.registry = scene;
                content.entities = {entity};
                std::set<ShaderKey> keys;
                ShaderVariantCollector::collectScene(scene, content, {}, keys, project.getProjectPath());
                std::set<std::string>& variants = out.spriteVariants[label];
                for (const ShaderKey& key : keys)
                    if (key.type == (uint16_t)ShaderType::MESH)
                        variants.insert(ShaderPool::getShaderStr((ShaderType)key.type, key.properties, 0) + "|" +
                                        ShaderPool::getCustomShaderName(key.customId));
            }
        }
        input.scenes.push_back(std::move(out));
    }

}

std::string jasperengine::editor::pixelart::scenePathOf(const Project& project, const SceneProject& scene){
    if (scene.filepath.empty())
        return scene.name;
    return project.normalizeToProjectRelative(scene.filepath).generic_string();
}

std::vector<uint32_t> jasperengine::editor::pixelart::scopeScenes(Project& project, const Scope& scope, std::vector<PixelProblem>& errors){
    std::vector<uint32_t> order;
    const uint32_t start = project.getStartSceneId();
    std::vector<SceneProject>& scenes = project.getScenes();
    if (scope.scenes.empty()){
        // the build list (start scene first, then the saved scenes in project order) and open scenes
        for (const SceneProject& scene : scenes)
            if (scene.id == start && !scene.filepath.empty())
                order.push_back(scene.id);
        for (const SceneProject& scene : scenes)
            if (scene.id != start && (!scene.filepath.empty() || scene.opened))
                order.push_back(scene.id);
        return order;
    }
    for (const std::string& ref : scope.scenes){
        const SceneProject* found = nullptr;
        for (const SceneProject& scene : scenes){
            const std::string path = scenePathOf(project, scene);
            const std::string stem = fs::path(path).stem().generic_string();
            if (ref == path || ref == scene.name || ref == stem || ref == std::to_string(scene.id) || ref == path + ".scene"){
                found = &scene;
                break;
            }
        }
        if (!found){
            errors.push_back({"not_found", "scenes", "No scene \"" + ref + "\" in the project.", "name a scene by its path (list_scenes)"});
            continue;
        }
        if (std::find(order.begin(), order.end(), found->id) == order.end())
            order.push_back(found->id);
    }
    return order;
}

PixelArtCheckInput jasperengine::editor::pixelart::inputFromProject(Project& project, const Scope& scope, std::vector<PixelProblem>& errors){
    PixelArtCheckInput input;
    input.assetsRoot = project.getAssetsPath();
    PixelArtProjectInfo& info = input.project;
    const PixelArtSettings& settings = project.getPixelArtSettings();
    info.modeEnabled = settings.enabled;
    info.assumeMode = scope.assumeModeOn;
    info.canvasWidth = (int)project.getCanvasWidth();
    info.canvasHeight = (int)project.getCanvasHeight();
    info.scaling = project.getScalingMode();
    info.pixelsPerUnit = project.getPixelsPerUnit();
    info.defaultFilterMode = project.getDefaultFilterMode();
    info.windowWidth = (int)project.getWindowWidth();
    info.windowHeight = (int)project.getWindowHeight();
    info.screens = settings.screens();
    info.upscaleMode = settings.upscaleMode;

    // the palettes: the Default Palette now, Palette Swap sources as drawables name them
    const std::string defaultPalette = defaultPalettePath(project);
    if (!defaultPalette.empty()){
        const ResolvedPalette resolved = resolvePaletteName(project, defaultPalette);
        if (resolved.ok){
            info.defaultPalette = resolved.assetPath.empty() ? resolved.path : resolved.assetPath;
            input.palettes[info.defaultPalette] = resolved.palette.colors;
        }
    }

    for (uint32_t id : scopeScenes(project, scope, errors)){
        SceneProject* sp = project.getScene(id);
        if (!sp)
            continue;
        if (sp->scene){
            extractScene(project, *sp, info, scope, input);
            continue;
        }
        SceneProject* probe = project.loadSceneProbe(id);
        if (!probe){
            errors.push_back({"unreadable", "scenes", "Scene \"" + sp->name + "\" could not be read from its file.", "open it to see its load errors"});
            continue;
        }
        probe->name = sp->name;
        extractScene(project, *probe, info, scope, input);
        project.releaseSceneProbe(probe);
    }

    // textures and fonts the scope names; else every texture whose import settings say pixel art
    if (!scope.paths.empty()){
        for (const std::string& raw : scope.paths){
            std::string path = raw;
            const std::string assetsPrefix = project.normalizeToProjectRelative(project.getAssetsPath()).generic_string() + "/";
            if (!assetsPrefix.empty() && assetsPrefix != "/" && path.rfind(assetsPrefix, 0) == 0)
                path = path.substr(assetsPrefix.size());
            std::error_code ec;
            if (!fs::is_regular_file(project.getAssetsPath() / fs::path(path), ec)){
                errors.push_back({"not_found", "paths", "No file \"" + raw + "\" in the assets.", "give an assets-relative texture or font path"});
                continue;
            }
            if (isFontPath(path)){
                if (std::find(input.fontPaths.begin(), input.fontPaths.end(), path) == input.fontPaths.end())
                    input.fontPaths.push_back(path);
                continue;
            }
            textureEntry(project, input, path).pixelArt = true;
        }
    }else if (!scope.selectionOnly){
        for (const std::string& path : projectTextureSources(project, false)){
            const std::string extension = lowerExtension(path);
            if (extension == ".aseprite" || extension == ".ase")
                continue;   // a sheet counts where a pixel-art drawable uses it
            TextureImportSettings settings;
            const fs::path absolute = project.getAssetsPath() / fs::path(path);
            if (!fs::exists(SetAssetMetaCmd::metaPathOf(absolute)))
                continue;
            const bool stored = TextureImportSettings::load(absolute.string(), settings);
            const bool layoutType = stored && (settings.resolvedType() == TextureImportType::Sprite || settings.resolvedType() == TextureImportType::UI);
            if ((stored && settings.resolvedPixelArt()) || (scope.assumeModeOn && layoutType))
                textureEntry(project, input, path).pixelArt = true;
        }
    }
    std::sort(input.fontPaths.begin(), input.fontPaths.end());

    for (auto& [path, texture] : input.textures){
        if (!texture.paletteSwapSource.empty() && !input.palettes.count(texture.paletteSwapSource)){
            const ResolvedPalette resolved = resolvePaletteName(project, texture.paletteSwapSource);
            if (resolved.ok)
                input.palettes[texture.paletteSwapSource] = resolved.palette.colors;
        }
    }
    return input;
}
