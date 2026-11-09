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
#include "util/AssetDatabase.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"

#include "component/CameraComponent.h"
#include "device/DeviceSettings.h"
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
#include <map>
#include <optional>
#include <set>
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

    Vector2 pivotPixels(PivotPreset preset, float width, float height){
        switch (preset){
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
            case particles::RenderMode::Pixel: return "pixel";
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

    // ---- what is kept between calls (main thread)

    struct FileStamp {
        fs::file_time_type time{};
        std::uintmax_t size = 0;
        bool exists = false;
        bool operator==(const FileStamp&) const = default;
    };

    FileStamp stampOf(const fs::path& path){
        FileStamp stamp;
        std::error_code ec;
        stamp.time = fs::last_write_time(path, ec);
        if (ec)
            return stamp;
        stamp.size = fs::file_size(path, ec);
        stamp.exists = !ec;
        return stamp;
    }

    // what a scene holds, before it is known to be a pixel-art scene (that needs its layers' and
    // parents' cameras and its textures' import settings, which change apart from the file)
    struct SceneFacts {
        PixelArtScene scene;                   // every candidate drawable; pixelArt not decided
        std::vector<uint32_t> layers;          // its layer (child) scenes
        bool pixelCamera = false;              // its main camera has an enabled orthographic Pixel Perfect Camera
        std::vector<int> pivotPresets;         // per drawable: a Sprite's PivotPreset, -1 otherwise (the pivot needs the texture size)
        // library:sprite material identity -> one GameObject drawing it; an open scene's variants
        // are collected once it is a pixel-art scene, a closed one's while its probe exists
        std::map<std::string, Entity> spriteMaterials;
        bool variantsDone = false;
        const SceneProject* live = nullptr;    // an open scene (not kept)
    };

    struct CachedTexture {
        FileStamp image, meta;
        bool modeEnabled = false;
        bool read = false;
        PixelArtTexture texture;
    };

    struct CachedScene {
        FileStamp file;
        SceneFacts facts;
    };

    struct InputCache {
        fs::path project;
        std::map<std::string, CachedTexture> textures;
        std::map<std::string, CachedScene> scenes;
    };

    InputCache& inputCache(){
        static InputCache cache;
        return cache;
    }

    InputReads& reads(){
        static InputReads value;
        return value;
    }

    std::map<std::string, DrawableCollector>& collectors(){
        static std::map<std::string, DrawableCollector> registered;
        return registered;
    }

    // a texture's import settings, header and sheet facts, from its files
    PixelArtTexture readTexture(Project& project, const std::string& path, const fs::path& absolute, bool exists){
        PixelArtTexture t;
        t.path = path;
        t.exists = exists;
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
        // no import settings yet: judged as S14's inference will write them (only an image added
        // while Pixel Art Mode is on is imported as pixel art, and that one has its settings)
        std::string error;
        t.hasSettings = TextureImportSettings::load(absolute.string(), t.settings, &error);
        int width = 0, height = 0, channels = 0;
        if (t.exists && stbi_info(absolute.string().c_str(), &width, &height, &channels)){
            t.width = width;
            t.height = height;
            t.hasAlpha = channels == 2 || channels == 4;
        }
        return t;
    }

    // a texture's entry in the input, from the cache while its image and meta are unchanged
    PixelArtTexture& textureEntry(Project& project, PixelArtCheckInput& input, const std::string& path){
        auto it = input.textures.find(path);
        if (it != input.textures.end())
            return it->second;
        const fs::path absolute = project.getAssetsPath() / fs::path(path);
        const FileStamp image = stampOf(absolute);
        const FileStamp meta = stampOf(SetAssetMetaCmd::metaPathOf(absolute));
        const bool mode = project.getPixelArtSettings().enabled;
        CachedTexture& cached = inputCache().textures[path];
        if (!cached.read || !(cached.image == image) || !(cached.meta == meta) || cached.modeEnabled != mode){
            cached.image = image;
            cached.meta = meta;
            cached.modeEnabled = mode;
            cached.read = true;
            cached.texture = readTexture(project, path, absolute, image.exists);
            reads().texturesRead++;
        }
        return input.textures.emplace(path, cached.texture).first->second;
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

    // the shader variants library:sprite needs for each material identity of a scene
    void collectVariants(Project& project, const SceneProject& sp, SceneFacts& facts){
        for (const auto& [label, entity] : facts.spriteMaterials){
            ShaderVariantContent content;
            content.registry = sp.scene;
            content.entities = {entity};
            std::set<ShaderKey> keys;
            ShaderVariantCollector::collectScene(sp.scene, content, {}, keys, project.getProjectPath());
            std::set<std::string>& variants = facts.scene.spriteVariants[label];
            for (const ShaderKey& key : keys)
                if (key.type == (uint16_t)ShaderType::MESH)
                    variants.insert(ShaderPool::getShaderStr((ShaderType)key.type, key.properties, 0) + "|" +
                                    ShaderPool::getCustomShaderName(key.customId));
        }
        facts.variantsDone = true;
    }

    SceneFacts extractFacts(Project& project, const SceneProject& sp, const Scope& scope){
        Scene* scene = sp.scene;
        SceneFacts facts;
        PixelArtScene& out = facts.scene;
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
        out.renderScale = post.renderScale;
        out.dynamicResolution = post.dynamicResolutionEnabled;
        for (const ChildSceneRef& child : sp.childScenes)
            facts.layers.push_back(child.id);

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
                    PixelArtCameraSettings& c = out.camera.component;
                    c.enabled = ppc->enabled;
                    c.referenceWidth = ppc->referenceWidth;
                    c.referenceHeight = ppc->referenceHeight;
                    c.upscaleMode = ppc->upscaleMode;
                    c.upscaleRenderTexture = ppc->upscaleRenderTexture;
                    c.pixelSnapping = ppc->pixelSnapping;
                    c.rotationMode = ppc->rotationMode;
                    c.subpixelSmoothing = ppc->subpixelSmoothing;
                }
            }
        }
        facts.pixelCamera = out.camera.exists && out.camera.hasComponent && out.camera.component.enabled && !out.camera.perspective;

        // the selection scope keeps only the selected GameObjects
        auto selected = [&](Entity entity){
            if (!scope.selectionOnly)
                return true;
            return std::find(scope.selection.begin(), scope.selection.end(), std::make_pair(sp.id, (uint32_t)entity)) != scope.selection.end();
        };

        std::vector<PixelArtDrawable>& drawables = out.drawables;
        auto base = [&](Entity entity, DrawableKind kind){
            PixelArtDrawable d;
            d.kind = kind;
            d.scene = out.path;
            d.entity = entityPathOf(sp, entity, paths);
            d.entityId = entity;
            if (const Transform* transform = scene->findComponent<Transform>(entity)){
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
        auto push = [&](PixelArtDrawable d, int pivotPreset){
            drawables.push_back(std::move(d));
            facts.pivotPresets.push_back(pivotPreset);
        };
        for (Entity entity : sp.entities){
            if (!scene->isEntityCreated(entity) || !selected(entity))
                continue;
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity); sprite && mesh && mesh->numSubmeshes > 0){
                PixelArtDrawable d = base(entity, DrawableKind::Sprite);
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
                d.width = (float)sprite->width;
                d.height = (float)sprite->height;
                readPaletteSwap(material, d);
                if (material.shader.path == "library:sprite"){
                    d.spriteMaterial = spriteMaterialLabel(material);
                    facts.spriteMaterials.emplace(d.spriteMaterial, entity);
                }
                push(std::move(d), (int)sprite->pivotPreset);
            }else if (const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity); tilemap && mesh && mesh->numSubmeshes > 0){
                PixelArtDrawable d = base(entity, DrawableKind::Tilemap);
                const Material& material = mesh->submeshes[0].material;
                d.textureField = "mesh.submeshes[0].material.baseColorTexture";
                d.texture = material.baseColorTexture.getPath();
                d.minFilter = material.baseColorTexture.getMinFilter();
                d.magFilter = material.baseColorTexture.getMagFilter();
                d.pixelsPerUnit = tilemap->pixelsPerUnit;
                readPaletteSwap(material, d);
                push(std::move(d), -1);
            }
            if (const ImageComponent* image = scene->findComponent<ImageComponent>(entity)){
                const UIComponent* ui = scene->findComponent<UIComponent>(entity);
                if (ui && !ui->texture.empty() && !ui->texture.isFramebuffer()){
                    PixelArtDrawable d = base(entity, DrawableKind::Image);
                    d.textureField = "ui.texture";
                    d.texture = ui->texture.getPath();
                    d.minFilter = ui->texture.getMinFilter();
                    d.magFilter = ui->texture.getMagFilter();
                    if (const RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity)){
                        d.rectWidth = (float)rect->width;
                        d.rectHeight = (float)rect->height;
                    }
                    d.sliced = image->patchMarginLeft || image->patchMarginRight || image->patchMarginTop || image->patchMarginBottom;
                    push(std::move(d), -1);
                }
            }
            if (const TextComponent* text = scene->findComponent<TextComponent>(entity)){
                PixelArtDrawable d = base(entity, DrawableKind::Text);
                d.font = text->font[0].path;
                for (size_t i = 1; i < text->font.size(); i++)
                    if (!text->font[i].path.empty())
                        d.fallbackFonts.push_back(text->font[i].path);
                d.fontSize = text->fontSize;
                push(std::move(d), -1);
            }
            if (const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity)){
                PixelArtDrawable d = base(entity, DrawableKind::Particles);
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
                const particles::PixelArtModule& pixel = document.pixelArt;
                d.particlePixelArt = pixel.present && pixel.enabled;
                d.particleSnap = pixel.snap;
                d.particlePixelSizes = pixel.pixelSizes;
                d.particleIntegerSize = pixel.integerSize;
                d.particleFade = pixel.fade == particles::PixelFade::Dither ? "dither" : pixel.fade == particles::PixelFade::Cutout ? "cutout" : "alpha";
                const particles::MinMaxCurve& size = document.main.startSize;
                if (size.mode == particles::CurveMode::Constant){
                    d.particleSizes.push_back(size.constant);
                }else if (size.mode == particles::CurveMode::TwoConstants){
                    d.particleSizes.push_back(size.constant);
                    d.particleSizes.push_back(size.constantMax);
                }
                push(std::move(d), -1);
            }
        }
        // a later spec's drawables (S52's Sprite Batch): kept with the scene's own
        const DrawableBase drawableBase = [&](uint32_t entity, DrawableKind kind){ return base((Entity)entity, kind); };
        for (const auto& [id, collect] : collectors()){
            std::vector<PixelArtDrawable> collected;
            collect(sp, drawableBase, collected);
            for (PixelArtDrawable& d : collected)
                if (selected((Entity)d.entityId))
                    push(std::move(d), -1);
        }
        return facts;
    }

    // R9: a pixel-art scene and its pixel-art drawables, from the facts
    void classify(Project& project, SceneFacts& facts, bool layerOfPixelCamera, const PixelArtProjectInfo& info, PixelArtCheckInput& input){
        PixelArtScene out = facts.scene;
        // a Sprite without its own size takes its texture's, and its pivot follows
        for (size_t i = 0; i < out.drawables.size(); i++){
            PixelArtDrawable& d = out.drawables[i];
            if (i >= facts.pivotPresets.size() || facts.pivotPresets[i] < 0)
                continue;
            float width = d.width, height = d.height;
            if ((width <= 0 || height <= 0) && !d.texture.empty()){
                const PixelArtTexture& t = textureEntry(project, input, d.texture);
                width = (float)t.width;
                height = (float)t.height;
            }
            d.width = width;
            d.height = height;
            d.pivot = pivotPixels((PivotPreset)facts.pivotPresets[i], width, height);
        }
        // drawn through a Pixel Perfect Camera (its own, or the one of the scene it is a layer of
        // when it has no camera of its own), a 2D scene of a Pixel Art Mode project, or one that
        // draws pixel-art textures
        out.pixelArt = facts.pixelCamera || layerOfPixelCamera || ((info.modeEnabled || info.assumeMode) && out.is2D);
        for (const PixelArtDrawable& d : out.drawables)
            out.pixelArt = out.pixelArt || texturePixelArt(project, input, d);
        std::vector<PixelArtDrawable> all = std::move(out.drawables);
        out.drawables.clear();
        for (PixelArtDrawable& d : all){
            // a 2D scene of sprites or tilemaps is pixel-art content for pixel_art_mode even before the mode is on
            out.drawsSprites = out.drawsSprites || (out.is2D && (d.kind == DrawableKind::Sprite || d.kind == DrawableKind::Tilemap));
            if (!out.pixelArt && !texturePixelArt(project, input, d))
                continue;
            // the particle renderer's texture is pixel art too (R2: the preset converts it); a Text draws a font
            if (d.kind != DrawableKind::Text)
                addDrawableTexture(project, input, d, true);
            out.drawables.push_back(std::move(d));
        }
        if (!out.pixelArt)
            out.spriteVariants.clear();
        input.scenes.push_back(std::move(out));
    }

}

std::string jasperengine::editor::pixelart::scenePathOf(const Project& project, const SceneProject& scene){
    if (scene.filepath.empty())
        return scene.name;
    return project.normalizeToProjectRelative(scene.filepath).generic_string();
}

void jasperengine::editor::pixelart::registerDrawableCollector(std::string id, DrawableCollector collector){
    collectors()[std::move(id)] = std::move(collector);
    forgetInputCache();
}

void jasperengine::editor::pixelart::unregisterDrawableCollector(const std::string& id){
    collectors().erase(id);
    forgetInputCache();
}

void jasperengine::editor::pixelart::forgetInputCache(){
    inputCache().textures.clear();
    inputCache().scenes.clear();
}

InputReads jasperengine::editor::pixelart::lastInputReads(){
    return reads();
}

std::vector<uint32_t> jasperengine::editor::pixelart::scopeScenes(Project& project, const Scope& scope, std::vector<PixelProblem>& errors){
    std::vector<uint32_t> order;
    if (scope.noScenes)
        return order;
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
    reads() = InputReads();
    InputCache& cache = inputCache();
    if (cache.project != project.getProjectPath()){
        forgetInputCache();
        cache.project = project.getProjectPath();
    }
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
    info.textureSettings = textureProjectSettingsOf(project);
    const DeviceSettings& devices = project.getDeviceSettings();
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++)
        for (int t = 0; t < DEVICE_TIER_COUNT; t++)
            if (const std::optional<float>& cap = devices.tier((DevicePlatformGroup)g, (DeviceTier)t).get(DeviceOverride::RenderScale); cap && *cap < 1.0f)
                info.renderScaleCaps.push_back({DeviceNames::group((DevicePlatformGroup)g), DeviceNames::tier((DeviceTier)t), *cap});

    // the palettes: the Default Palette now, Palette Swap sources as drawables name them
    const std::string defaultPalette = defaultPalettePath(project);
    if (!defaultPalette.empty()){
        const ResolvedPalette resolved = resolvePaletteName(project, defaultPalette);
        if (resolved.ok){
            info.defaultPalette = resolved.assetPath.empty() ? resolved.path : resolved.assetPath;
            input.palettes[info.defaultPalette] = resolved.palette.colors;
        }
    }

    // a scene's facts: an open scene's live (with its unsaved edits), a closed one's from its file
    // through a probe, kept while the file is unchanged
    std::map<uint32_t, SceneFacts> known;
    auto closedFacts = [&](const fs::path& file, const std::function<SceneProject*()>& probeOf, const std::string& name) -> std::optional<SceneFacts> {
        const std::string key = file.lexically_normal().generic_string();
        const FileStamp stamp = stampOf(file);
        auto hit = cache.scenes.find(key);
        if (hit != cache.scenes.end() && hit->second.file == stamp){
            SceneFacts facts = hit->second.facts;
            facts.scene.name = name;
            return facts;
        }
        SceneProject* probe = probeOf();
        if (!probe)
            return std::nullopt;
        probe->name = name;
        SceneFacts facts = extractFacts(project, *probe, Scope());
        // the variants need the probe's scene: collected now, whether or not it turns out pixel art
        collectVariants(project, *probe, facts);
        project.releaseSceneProbe(probe);
        reads().scenesDecoded++;
        cache.scenes[key] = {stamp, facts};
        return facts;
    };
    auto factsOf = [&](uint32_t id, bool report) -> SceneFacts* {
        auto it = known.find(id);
        if (it != known.end())
            return &it->second;
        SceneProject* sp = project.getScene(id);
        if (!sp)
            return nullptr;
        if (sp->scene){
            SceneFacts facts = extractFacts(project, *sp, scope);
            facts.live = sp;
            return &known.emplace(id, std::move(facts)).first->second;
        }
        if (sp->filepath.empty())
            return nullptr;
        const fs::path file = sp->filepath.is_relative() ? project.getProjectPath() / sp->filepath : sp->filepath;
        std::optional<SceneFacts> facts = closedFacts(file, [&](){ return project.loadSceneProbe(id); }, sp->name);
        if (!facts){
            if (report)
                errors.push_back({"unreadable", "scenes", "Scene \"" + sp->name + "\" could not be read from its file.", "open it to see its load errors"});
            return nullptr;
        }
        return &known.emplace(id, std::move(*facts)).first->second;
    };
    // a layer scene without a camera of its own is drawn under the main camera of the scene it is
    // a layer of (R9's "a layer scene's camera")
    auto underPixelCamera = [&](uint32_t id, const SceneFacts& facts){
        if (facts.scene.camera.exists)
            return false;
        for (const SceneProject& other : project.getScenes()){
            if (other.id == id)
                continue;
            // the project's link (a closed scene's too), else what the closed scene's file holds
            const bool linked = std::any_of(other.childScenes.begin(), other.childScenes.end(), [&](const ChildSceneRef& c){ return c.id == id; });
            if (!linked && other.scene)
                continue;
            SceneFacts* parent = factsOf(other.id, false);
            if (parent && parent->pixelCamera && (linked || std::find(parent->layers.begin(), parent->layers.end(), id) != parent->layers.end()))
                return true;
        }
        return false;
    };

    for (uint32_t id : scopeScenes(project, scope, errors)){
        const SceneFacts* found = factsOf(id, true);
        if (!found)
            continue;
        SceneFacts facts = *found;
        const bool layer = underPixelCamera(id, facts);
        const size_t at = input.scenes.size();
        classify(project, facts, layer, info, input);
        // an open scene's variants, once it is a pixel-art scene
        if (input.scenes[at].pixelArt && facts.live && !facts.variantsDone){
            collectVariants(project, *facts.live, facts);
            input.scenes[at].spriteVariants = facts.scene.spriteVariants;
        }
    }
    // the Project scope: scene files the project does not list too
    if (scope.allScenes){
        std::set<std::string> listed;
        for (const SceneProject& scene : project.getScenes())
            if (!scene.filepath.empty())
                listed.insert(project.sceneFileKey(scene.filepath));
        std::vector<std::string> files;
        project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& asset){
            if (asset.type == AssetType::Scene && !listed.count(project.sceneFileKey(asset.path)))
                files.push_back(asset.path);
        });
        std::sort(files.begin(), files.end());
        for (const std::string& relative : files){
            const fs::path file = project.getProjectPath() / fs::path(relative);
            std::optional<SceneFacts> facts = closedFacts(file, [&](){ return project.loadSceneFileProbe(file); }, fs::path(relative).stem().string());
            if (!facts){
                errors.push_back({"unreadable", "scenes", "Scene file \"" + relative + "\" could not be read.", "open it to see its load errors"});
                continue;
            }
            facts->scene.path = relative;
            for (PixelArtDrawable& d : facts->scene.drawables)
                d.scene = relative;
            classify(project, *facts, false, info, input);
        }
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
                if (scope.pixelArtPathsOnly)
                    continue;
                if (std::find(input.fontPaths.begin(), input.fontPaths.end(), path) == input.fontPaths.end())
                    input.fontPaths.push_back(path);
                continue;
            }
            PixelArtTexture& t = textureEntry(project, input, path);
            if (scope.pixelArtPathsOnly && !(t.aseprite || (t.hasSettings && t.settings.resolvedPixelArt()))){
                input.textures.erase(path);
                continue;
            }
            t.pixelArt = true;
        }
    }else if (!scope.selectionOnly && !scope.noScenes){
        for (const std::string& path : projectTextureSources(project, false)){
            const std::string extension = lowerExtension(path);
            // a sheet counts where a pixel-art drawable uses it, and in the Project scope
            if (extension == ".aseprite" || extension == ".ase"){
                if (scope.allScenes)
                    textureEntry(project, input, path).pixelArt = true;
                continue;
            }
            const fs::path absolute = project.getAssetsPath() / fs::path(path);
            if (!fs::exists(SetAssetMetaCmd::metaPathOf(absolute)))
                continue;
            const bool inInput = input.textures.count(path) > 0;
            PixelArtTexture& t = textureEntry(project, input, path);
            const bool layoutType = t.hasSettings && (t.settings.resolvedType() == TextureImportType::Sprite ||
                                                      t.settings.resolvedType() == TextureImportType::UI);
            if ((t.hasSettings && t.settings.resolvedPixelArt()) || (scope.assumeModeOn && layoutType))
                t.pixelArt = true;
            else if (!inInput)
                input.textures.erase(path);
        }
    }
    std::sort(input.fontPaths.begin(), input.fontPaths.end());
    analyzeFonts(input);

    for (auto& [path, texture] : input.textures){
        if (!texture.paletteSwapSource.empty() && !input.palettes.count(texture.paletteSwapSource)){
            const ResolvedPalette resolved = resolvePaletteName(project, texture.paletteSwapSource);
            if (resolved.ok)
                input.palettes[texture.paletteSwapSource] = resolved.palette.colors;
        }
    }
    return input;
}
