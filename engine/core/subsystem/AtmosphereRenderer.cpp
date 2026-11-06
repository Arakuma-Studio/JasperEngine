// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AtmosphereRenderer.h"

#include "RenderSystem.h"
#include "Scene.h"
#include "Log.h"
#include "Engine.h"
#include "device/QualityResolver.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "component/SkyboxComponent.h"
#include "component/LightComponent.h"
#include "component/CameraComponent.h"
#include "component/Transform.h"
#include "pool/ShaderPool.h"
#include "render/AtmosphereNoise.h"
#include "jobs/JobSystem.h"
#include "render/BlueNoise64.h"
#include "render/SystemRender.h"
#include "util/Color.h"
#include "util/Angle.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

using namespace jasperengine;

TextureRender AtmosphereRenderer::noiseShape;
TextureRender AtmosphereRenderer::noiseDetail;
TextureRender AtmosphereRenderer::noiseWeather;
TextureRender AtmosphereRenderer::blueNoise;
TextureRender AtmosphereRenderer::empty3D;
bool AtmosphereRenderer::forceLowPrecision = std::getenv("JASPERENGINE_ATMOSPHERE_RGBA8") != nullptr;

namespace {

    constexpr float EARTH_RADIUS = 6360000.0f;
    // the clouds fade in over this once the noise is baked
    constexpr double CLOUDS_FADE_IN_SECONDS = 0.5;

    const char* const PASS_LABELS[] = {
        "Atmosphere/DepthDownsample", "Atmosphere/VolumetricFog", "Atmosphere/Denoise", "Atmosphere/CloudsScreen",
        "Atmosphere/CloudsSkyMap", "Atmosphere/SkyComposite", "Atmosphere/VolumetricComposite"
    };

    // the shader of each AtmosphereRenderer::ProgramKind
    const ShaderType PROGRAM_TYPES[] = {
        ShaderType::ATMOSPHERE_DEPTH, ShaderType::VOLUMETRIC_FOG, ShaderType::ATMOSPHERE_DENOISE,
        ShaderType::VOLUMETRIC_CLOUDS, ShaderType::SKY_COMPOSITE, ShaderType::VOLUMETRIC_COMPOSITE
    };

    std::unordered_map<std::string, IBLEnvironment::HorizonRing>& horizonRings(){
        static std::unordered_map<std::string, IBLEnvironment::HorizonRing> rings;
        return rings;
    }

    // the ring's colour at a cube azimuth (radians), between its sectors' centres
    Vector3 ringAt(const IBLEnvironment::HorizonRing& ring, float azimuth){
        const float n = (float)IBLEnvironment::HORIZON_AZIMUTHS;
        float position = azimuth / (2.0f * 3.14159265358979f) * n;
        position -= std::floor(position / n) * n;
        const int k0 = (int)std::floor(position) % IBLEnvironment::HORIZON_AZIMUTHS;
        const int k1 = (k0 + 1) % IBLEnvironment::HORIZON_AZIMUTHS;
        const float f = position - std::floor(position);
        return ring[k0] * (1.0f - f) + ring[k1] * f;
    }

    // a fullscreen pass of an atmosphere target: no depth, no blending
    PipelineState offscreenState(bool floatFormat){
        PipelineState state;
        state.target.color = floatFormat ? ColorFormat::RGBA16F : ColorFormat::RGBA;
        state.target.depth = DepthStencilFormat::NONE;
        state.target.sampleCount = 1;
        state.blend = BlendMode::NONE;
        state.depthCompare = CompareFunc::ALWAYS;
        state.depthWrite = false;
        return state;
    }

    void bindTexture(ObjectRender& render, ShaderData& data, const char* name, TextureRender* texture){
        const std::pair<int, int> slot = data.getTextureIndexByName(name);
        if (slot.first >= 0 && texture)
            render.addTexture(slot, ShaderStageType::FRAGMENT, texture);
    }

    void applyBlock(ObjectRender& render, ShaderData& data, const char* name, unsigned int size, const void* block){
        const int slot = data.getUniformBlockIndexByName(name);
        if (slot >= 0)
            render.applyUniformBlock(slot, size, const_cast<void*>(block));
    }

    uint64_t hashCombine(uint64_t h, float value){
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        h ^= bits;
        h *= 1099511628211ULL;
        return h;
    }

    // the clip-space rows of the inverse projection the passes turn depth into view distance
    // with (RenderSystem::fillSceneInputs' rule: the projection out of the view-projection)
    void inverseProjectionRows(const CameraComponent& camera, Vector4& rowZ, Vector4& rowW){
        const Matrix4 inverseView = camera.viewMatrix.inverse();
        const Matrix4 inverseProjection = (camera.viewProjectionMatrix * inverseView).inverse();
        const Vector4 z = inverseProjection.row(2);
        rowZ = Vector4(-z.x, -z.y, -z.z, -z.w);
        rowW = inverseProjection.row(3);
    }

    Vector3 cameraForward(const CameraComponent& camera){
        // worldDirection is the view's backward axis (RenderSystem::fillFrameBlock)
        Vector3 forward(-camera.worldDirection.x, -camera.worldDirection.y, -camera.worldDirection.z);
        return forward.length() > 0.0f ? forward.normalized() : Vector3(0.0f, 0.0f, -1.0f);
    }

}

size_t AtmosphereViewTargets::getBytes() const{
    size_t bytes = 0;
    auto add = [&bytes](const Framebuffer& target, size_t texelBytes){
        Framebuffer& fb = const_cast<Framebuffer&>(target);
        if (fb.isCreated())
            bytes += (size_t)fb.getWidth() * fb.getHeight() * texelBytes;
    };
    const size_t colorBytes = compressed ? 4 : 8;
    for (const Framebuffer& target : depth)
        add(target, 4);
    for (const Framebuffer& target : fog)
        add(target, colorBytes);
    for (const Framebuffer& target : clouds)
        add(target, colorBytes);
    for (const Framebuffer& target : skyMap)
        add(target, colorBytes);
    return bytes;
}

size_t AtmosphereViewTargets::getScreenBytes() const{
    size_t bytes = 0;
    auto add = [&bytes](const Framebuffer& target, size_t texelBytes){
        Framebuffer& fb = const_cast<Framebuffer&>(target);
        if (fb.isCreated())
            bytes += (size_t)fb.getWidth() * fb.getHeight() * texelBytes;
    };
    const size_t colorBytes = compressed ? 4 : 8;
    for (const Framebuffer& target : depth)
        add(target, 4);
    for (const Framebuffer& target : fog)
        add(target, colorBytes);
    for (const Framebuffer& target : clouds)
        add(target, colorBytes);
    return bytes;
}

void AtmosphereViewTargets::releaseUnused(){
    if (!used[0]){
        depth[0].destroy();
        depth[1].destroy();
    }
    if (!used[1]){
        fog[0].destroy();
        fog[1].destroy();
    }
    if (!used[2]){
        clouds[0].destroy();
        clouds[1].destroy();
    }
    if (!used[3]){
        for (Framebuffer& target : skyMap)
            target.destroy();
        skyMapSize = 0;
        skyMapCurrent = -1;
        skyMapPrevious = -1;
        skyMapBuilding = 0;
        skyMapRow = 0;
    }
    for (bool& flag : used)
        flag = false;
}

void AtmosphereViewTargets::destroy(){
    for (bool& flag : used)
        flag = false;
    releaseUnused();
}

AtmosphereRenderer::AtmosphereRenderer(RenderSystem* renderSystem): rs(renderSystem){
    for (Vector3& color : horizon)
        color = Vector3(0.0f, 0.0f, 0.0f);
    clearHeightFog(passFog);
}

AtmosphereRenderer::~AtmosphereRenderer(){
}

void AtmosphereRenderer::setForceLowPrecisionTargets(bool force){
    forceLowPrecision = force;
}

bool AtmosphereRenderer::isForceLowPrecisionTargets(){
    return forceLowPrecision;
}

std::vector<ShaderType> AtmosphereRenderer::passShaderTypes(bool heightFog, bool volumetricFog, bool clouds){
    std::vector<ShaderType> types;
    // the sky composite: the sky fill and the Sky Map clouds
    if (heightFog || clouds)
        types.push_back(ShaderType::SKY_COMPOSITE);
    // the reduced-resolution passes: the volumetric fog, the Screen clouds
    if (volumetricFog || clouds){
        types.push_back(ShaderType::ATMOSPHERE_DEPTH);
        types.push_back(ShaderType::ATMOSPHERE_DENOISE);
        types.push_back(ShaderType::VOLUMETRIC_COMPOSITE);
    }
    if (volumetricFog)
        types.push_back(ShaderType::VOLUMETRIC_FOG);
    if (clouds)
        types.push_back(ShaderType::VOLUMETRIC_CLOUDS);
    return types;
}

void AtmosphereRenderer::storeHorizonRing(const std::string& textureId, const IBLEnvironment::HorizonRing& ring){
    horizonRings()[textureId] = ring;
}

bool AtmosphereRenderer::useFloatTargets() const{
    const RenderFeatures& features = SystemRender::getFeatures();
    return features.renderRGBA16F && features.filterRGBA16F && !forceLowPrecision;
}

AtmosphereQuality AtmosphereRenderer::viewQuality() const{
    // the game's runtime override, else the editor's Game view preview, else the project's
    // tier for this platform
    if (Engine::isAtmosphereQualityOverridden())
        return Engine::getAtmosphereQuality();
    if (rs->rv().atmosphereQuality >= 0)
        return (AtmosphereQuality)rs->rv().atmosphereQuality;
    // the device tier may turn the volumetrics off (QualityResolver)
    return QualityResolver::volumetrics(Engine::getAtmosphereQualityDefault());
}

void AtmosphereRenderer::clearHeightFog(height_fog_t& block){
    block.invViewProjection.identity();
    block.screenToNdc = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    block.color_colorMode = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    block.extinction_base_H_maxOpacity = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
    block.start_volumetric_maxDist_inscatter = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    block.sunDir_anisotropy = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    block.sunColor_skyFillSin = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    for (Vector4& ring : block.horizonRing)
        ring = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
}

void AtmosphereRenderer::update(double dt){
    Scene* scene = rs->scene;
    time += dt;

    FogComponent* sceneFog = scene->findComponentFromIndex<FogComponent>(0);
    fog = sceneFog;
    fogPresent = sceneFog != nullptr;
    heightFogActive = sceneFog && sceneFog->type == FogType::HEIGHT;

    // the first clouds of the scene, on an active GameObject
    clouds = nullptr;
    auto cloudsArray = scene->getComponentArray<VolumetricCloudsComponent>();
    if (cloudsArray->size() > 0 && scene->isEntityActiveInHierarchy(cloudsArray->getEntity(0)))
        clouds = &cloudsArray->getComponentFromIndex(0);

    depthPrograms = (heightFogActive && fog->volumetric) || clouds;

    // the Local Volumetric Fog volumes, in world space (only volumetric fog reads them)
    volumes.clear();
    if (heightFogActive && fog->volumetric){
        auto volumeArray = scene->getComponentArray<LocalVolumetricFogComponent>();
        for (size_t i = 0; i < volumeArray->size(); i++){
            const Entity entity = volumeArray->getEntity(i);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !scene->isEntityActiveInHierarchy(entity))
                continue;
            const LocalVolumetricFogComponent& component = volumeArray->getComponentFromIndex(i);
            FogVolumeData volume;
            volume.entity = entity;
            volume.fog = component;
            const Matrix4 frame = Matrix4::translateMatrix(transform->worldPosition) * transform->worldRotation.getRotationMatrix();
            volume.worldToVolume = frame.inverse();
            volume.halfExtents = Vector3(std::abs(component.size.x * transform->worldScale.x), std::abs(component.size.y * transform->worldScale.y),
                                         std::abs(component.size.z * transform->worldScale.z)) * 0.5f;
            volume.center = transform->worldPosition;
            volume.boundingRadius = volume.halfExtents.length();
            volumes.push_back(volume);
        }
    }

    // the noise and the weather drift with the wind (sampled at p + offset)
    if (fog)
        fogNoiseOffset = fogNoiseOffset - fog->wind * (float)dt;
    if (clouds){
        const float angle = Angle::defaultToRad(clouds->windDirection);
        const Vector2 drift = Vector2(std::sin(angle), std::cos(angle)) * (clouds->windSpeed * (float)dt);
        cloudShapeOffset = cloudShapeOffset - drift;
        cloudWeatherOffset = cloudWeatherOffset - drift;
    }

    // the bake runs once started (planPass starts it when clouds or a noise first draw)
    AtmosphereNoise& noise = AtmosphereNoise::get();
    // Without workers the main thread bakes a slice a frame: half the time the rest of the frame
    // took (4 to 250 ms), so the bake takes at most a third of the main thread whatever the frame
    // rate (a slow frame, as in a debug WebGL build on a software rasteriser, would otherwise
    // stretch a fixed slice's bake to many minutes)
    if (noise.isStarted() && !noise.isReady()){
        auto nowMs = [](){
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        const double restOfFrame = bakeSliceEnd < 0.0 ? 8.0 : nowMs() - bakeSliceEnd;
        noise.advance(std::clamp(restOfFrame * 0.5, 4.0, 250.0));
        bakeSliceEnd = nowMs();
    }
    if (noise.isReady() && !noiseShape.isCreated()){
        static bool logged = false;
        if (!logged){
            logged = true;
            Log::print("Atmosphere: cloud noise baked in %.0f ms (%s)", noise.getBakeMilliseconds(),
                      noise.isThreaded() ? "background jobs" : "main thread, time-sliced");
        }
        const std::vector<uint8_t>& shape = noise.getShape();
        const std::vector<uint8_t>& detail = noise.getDetail();
        const std::vector<uint8_t>& weather = noise.getWeather();
        noiseShape.createTexture3D("atmosphere|shape", AtmosphereNoise::SHAPE_SIZE, AtmosphereNoise::SHAPE_SIZE, AtmosphereNoise::SHAPE_SIZE,
                                   shape.data(), shape.size(), TextureFilter::LINEAR, TextureWrap::REPEAT);
        noiseDetail.createTexture3D("atmosphere|detail", AtmosphereNoise::DETAIL_SIZE, AtmosphereNoise::DETAIL_SIZE, AtmosphereNoise::DETAIL_SIZE,
                                    detail.data(), detail.size(), TextureFilter::LINEAR, TextureWrap::REPEAT);
        void* weatherData[1] = {(void*)weather.data()};
        size_t weatherSize[1] = {weather.size()};
        noiseWeather.createTexture("atmosphere|weather", AtmosphereNoise::WEATHER_SIZE, AtmosphereNoise::WEATHER_SIZE, ColorFormat::RGBA,
                                   TextureType::TEXTURE_2D, 1, weatherData, weatherSize, TextureFilter::LINEAR, TextureFilter::LINEAR,
                                   TextureWrap::REPEAT, TextureWrap::REPEAT);
    }
    if (noiseShape.isCreated() && noiseReadyTime < 0.0)
        noiseReadyTime = time;

    updateHorizon();
}

void AtmosphereRenderer::updateHorizon(){
    horizonSource = "fog_color";
    if (!fog)
        return;
    const Vector3 fogColor = fog->color;
    for (Vector3& color : horizon)
        color = fogColor;

    Scene* scene = rs->scene;
    auto skies = scene->getComponentArray<SkyboxComponent>();
    if (skies->size() == 0)
        return;
    SkyboxComponent& sky = skies->getComponentFromIndex(0);
    if (!sky.texture.empty() && sky.texture.getNumFaces() == 6){
        auto ring = horizonRings().find(sky.texture.getId());
        if (ring != horizonRings().end()){
            // the Skybox's colour tints the sky and its rotation turns it about +Y (sky.frag,
            // ibl.glsl rotateEnv: world azimuth a shows the cube's a - rotation); each entry is
            // the 90 degree sector around its azimuth
            const Vector3 tint = Color::sRGBToLinear(Vector3(sky.color.x, sky.color.y, sky.color.z));
            const float rotation = Angle::defaultToRad(sky.rotation);
            const float quarter = 0.5f * 3.14159265358979f;
            for (int i = 0; i < 4; i++){
                const float centre = (float)i * quarter - rotation;
                Vector3 sum(0.0f, 0.0f, 0.0f);
                const float weights[5] = {0.5f, 1.0f, 1.0f, 1.0f, 0.5f};
                for (int s = 0; s < 5; s++)
                    sum += ringAt(ring->second, centre + ((float)s - 2.0f) * 0.25f * quarter) * weights[s];
                horizon[i] = sum * 0.25f * tint;
            }
            horizonSource = "cubemap";
        }
        return;
    }
    // a library or fork sky declaring a horizon colour (procedural_sky, gradient_sky)
    if (sky.shaderManifest){
        if (const ShaderManifestProperty* property = sky.shaderManifest->findProperty("horizonColor")){
            Vector4 value = ShaderUniforms::has(sky.shaderUniforms, "horizonColor") ? ShaderUniforms::get(sky.shaderUniforms, "horizonColor")
                                                                                    : property->defaultValue;
            for (Vector3& color : horizon)
                color = Vector3(value.x, value.y, value.z);
            horizonSource = "property";
        }
    }
}

void AtmosphereRenderer::fillHeightFog(height_fog_t& block, const CameraComponent& camera, const Transform& cameraTransform,
                                       const Rect& viewport, unsigned int targetHeight, bool flipped, float volumetricDistance) const{
    clearHeightFog(block);
    // colour mode -1: the scene fog is not a height fog (the fogged meshes take the distance fog)
    block.color_colorMode.w = -1.0f;

    // the camera's own clip space (no target flip) and the fragment -> camera ndc mapping of
    // the pass (RenderSystem::fillSceneInputs' screenParams, as ndc)
    block.invViewProjection = camera.viewProjectionMatrix.inverse();
    const bool gl = Engine::isOpenGL();
    const float width = std::max(viewport.getWidth(), 1.0f);
    const float height = std::max(viewport.getHeight(), 1.0f);
    const float rowStart = gl ? viewport.getY() : (float)targetHeight - viewport.getY() - viewport.getHeight();
    const bool topFirst = gl ? flipped : !flipped;
    const Vector4 screenToUV(1.0f / width, topFirst ? -1.0f / height : 1.0f / height,
                             -viewport.getX() / width, topFirst ? 1.0f + rowStart / height : -rowStart / height);
    block.screenToNdc = Vector4(2.0f * screenToUV.x, 2.0f * screenToUV.y, 2.0f * screenToUV.z - 1.0f, 2.0f * screenToUV.w - 1.0f);

    const frame_t& frame = rs->passFrame;
    const Vector3 sunDirection(frame.mainLightDirection.x, frame.mainLightDirection.y, frame.mainLightDirection.z);
    const float sunIntensity = frame.mainLightDirection.w;
    block.sunDir_anisotropy = Vector4(sunDirection, 0.0f);
    block.sunColor_skyFillSin = Vector4(Vector3(frame.mainLightColor.x, frame.mainLightColor.y, frame.mainLightColor.z) * sunIntensity, 0.0f);

    if (!heightFogActive)
        return;
#ifdef JASPERENGINE_EDITOR
    // the editor view's Fog toggle: no fog, the sun kept for the clouds
    if (rs->rv().disableFog)
        return;
#endif
    const HeightFogMedium medium = AtmosphereMath::makeMedium(*fog);
    block.color_colorMode = Vector4(fog->color, fog->colorMode == FogColorMode::SKY ? 1.0f : 0.0f);
    block.extinction_base_H_maxOpacity = Vector4(medium.extinction, medium.baseHeight, medium.scaleHeight, medium.maxOpacity);
    block.start_volumetric_maxDist_inscatter = Vector4(medium.startDistance, volumetricDistance, medium.maxDistance,
                                                       std::max(fog->directionalInscattering, 0.0f));
    block.sunDir_anisotropy.w = fog->anisotropy;
    for (int i = 0; i < 4; i++)
        block.horizonRing[i] = Vector4(horizon[i], 0.0f);
    if (fog->skyFill && fog->skyFillAngle > 0.0f){
        block.sunColor_skyFillSin.w = std::sin(Angle::defaultToRad(std::min(fog->skyFillAngle, 90.0f)));
        // the most the sky takes: the fog's opacity along a level ray at the camera's height
        const float horizontal = 1.0f - std::exp(-AtmosphereMath::opticalDepth(medium, cameraTransform.worldPosition, Vector3(1.0f, 0.0f, 0.0f),
                                                                            0.0f, medium.maxDistance));
        block.horizonRing[0].w = std::min(horizontal, medium.maxOpacity);
    }
}

bool AtmosphereRenderer::cloudsReady(){
    AtmosphereNoise& noise = AtmosphereNoise::get();
    // on the job system's workers, else (no workers: web without threads) time-sliced in update()
    if (!noise.isStarted())
        noise.start(!jobs::system().isSingleThreaded());
    return noiseShape.isCreated();
}

AtmosphereRenderer::PassPlan AtmosphereRenderer::planPass(const CameraComponent& camera, bool mainCamera, bool skyComposite) const{
    PassPlan plan;
    plan.mainCamera = mainCamera;
    bool fogShown = heightFogActive;
    bool cloudsShown = clouds != nullptr && !rs->rv().disableClouds;
#ifdef JASPERENGINE_EDITOR
    // the editor view's Fog toggle
    fogShown = fogShown && !rs->rv().disableFog;
#endif
    const AtmosphereQuality quality = viewQuality();
    const bool skyPass = skyComposite && camera.depthTest;

    plan.skyFill = skyPass && fogShown && fog->skyFill && fog->skyFillAngle > 0.0f;
    if (cloudsShown && quality != AtmosphereQuality::OFF){
        const AtmosphereQuality capped = AtmosphereTiers::apply(quality, clouds->qualityOverride);
        const CloudsRendering mode = AtmosphereTiers::resolveClouds(clouds->rendering, capped);
        plan.screenClouds = mainCamera && mode == CloudsRendering::SCREEN;
        // other cameras (mirrors, render-to-texture, probes) see the Sky Map clouds
        plan.skyMapClouds = skyPass && !plan.screenClouds;
    }
    if (fogShown && fog->volumetric && mainCamera && quality != AtmosphereQuality::OFF)
        plan.volumetricFog = AtmosphereTiers::apply(quality, fog->qualityOverride) != AtmosphereQuality::OFF;
    plan.skyComposite = plan.skyFill || plan.skyMapClouds;
    return plan;
}

bool AtmosphereRenderer::loadProgram(ProgramKind kind){
    const ShaderType* types = PROGRAM_TYPES;
    Program& program = programs[kind];
    if (program.loaded)
        return true;
    // asked again until it is created: a background build finishes in a later get()
    program.shader = ShaderPool::get(types[kind], 0);
    if (!program.shader || !program.shader->isCreated()){
        // still building (a failed build is not waited for)
        if (!ShaderPool::isShaderBuildFailed(types[kind], 0, 0))
            waiting = true;
        return false;
    }

    if (!blueNoise.isCreated()){
        void* data[1] = {(void*)blueNoise64};
        size_t size[1] = {sizeof(blueNoise64)};
        blueNoise.createTexture("atmosphere|bluenoise", BLUE_NOISE_64_SIZE, BLUE_NOISE_64_SIZE, ColorFormat::RED, TextureType::TEXTURE_2D, 1,
                                data, size, TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::REPEAT, TextureWrap::REPEAT);
    }
    if (!empty3D.isCreated()){
        static const uint8_t white[4] = {255, 255, 255, 255};
        empty3D.createTexture3D("atmosphere|empty3d", 1, 1, 1, white, sizeof(white), TextureFilter::NEAREST, TextureWrap::REPEAT);
    }

    ObjectRender& render = program.render;
    render.beginLoad(PrimitiveType::TRIANGLES);
    render.setShader(program.shader.get());
    // every texture the shader declares gets a placeholder of its kind: a draw binds the real ones
    ShaderData& data = program.shader->shaderData;
    for (const ShaderStage& stage : data.stages){
        for (const ShaderTexture& texture : stage.textures){
            TextureRender* placeholder = &RenderSystem::emptyBlack;
            if (texture.type == TextureType::TEXTURE_3D)
                placeholder = &empty3D;
            else if (texture.samplerType == TextureSamplerType::DEPTH)
                placeholder = &RenderSystem::emptyShadowDepth;
            render.addTexture(data.getTextureIndexByName(texture.name), ShaderStageType::FRAGMENT, placeholder);
        }
    }
    // a pass of the frame: created at once, never skipped (spec S32 §6.4)
    render.setImmediatePipelines(true);
    if (!render.endLoadDynamic(PrimitiveType::TRIANGLES, IndexType::NONE)){
        Log::error("Atmosphere: could not load the %s pass", ShaderPool::getShaderTypeName(types[kind], false).c_str());
        return false;
    }
    program.loaded = true;
    return true;
}

bool AtmosphereRenderer::ensureTarget(Framebuffer& target, unsigned int width, unsigned int height, bool floatFormat, bool linear){
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    // within 8 pixels of the size it has: kept (dynamic resolution changes nothing)
    if (target.isCreated()){
        const int dw = (int)target.getWidth() - (int)width;
        const int dh = (int)target.getHeight() - (int)height;
        const bool sameFormat = (target.getRender().getColorTexture().isCreated());
        if (sameFormat && std::abs(dw) <= 8 && std::abs(dh) <= 8)
            return true;
    }
    FramebufferDesc desc;
    desc.width = (int)width;
    desc.height = (int)height;
    desc.color = floatFormat ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    desc.depth = DepthStencilFormat::NONE;
    desc.minFilter = linear ? TextureFilter::LINEAR : TextureFilter::NEAREST;
    desc.magFilter = desc.minFilter;
    desc.wrapU = TextureWrap::CLAMP_TO_EDGE;
    desc.wrapV = TextureWrap::CLAMP_TO_EDGE;
    desc.label = "atmosphere";
    target.destroy();
    return target.create(desc);
}

void AtmosphereRenderer::notePass(const char* label){
    building.passes.push_back(label);
}

void AtmosphereRenderer::renderDepth(Framebuffer& target, TextureRender* sceneDepth, int factor, const CameraComponent& camera){
    Program& program = programs[PROGRAM_DEPTH];
    atmosphere_depth_t block;
    const float sourceWidth = (float)std::max(sceneDepth->getWidth(), 1);
    const float sourceHeight = (float)std::max(sceneDepth->getHeight(), 1);
    block.source = Vector4(1.0f / sourceWidth, 1.0f / sourceHeight, sourceWidth, sourceHeight);
    block.factor_invFar = Vector4((float)factor, 1.0f / std::max(camera.farClip, 1e-3f), 0.0f, 0.0f);
    inverseProjectionRows(camera, block.inverseProjectionZ, block.inverseProjectionW);

    passRender.setClearColor(Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    passRender.setLabel(PASS_LABELS[0]);
    passRender.startRenderPass(&target.getRender());
    PipelineState state = offscreenState(false);
    if (program.render.beginDraw(state)){
        ShaderData& data = program.shader->shaderData;
        bindTexture(program.render, data, "u_depthTexture", sceneDepth);
        applyBlock(program.render, data, "u_fs_atmosphereDepth", sizeof(block), &block);
        program.render.draw(0, 3, 1);
    }
    passRender.endRenderPass();
    notePass(PASS_LABELS[0]);
}

void AtmosphereRenderer::renderDenoise(Framebuffer& source, Framebuffer& target, Framebuffer& depth, const char* label){
    Program& program = programs[PROGRAM_DENOISE];
    atmosphere_denoise_t block;
    block.target = Vector4(1.0f / (float)target.getWidth(), 1.0f / (float)target.getHeight(), useFloatTargets() ? 0.0f : 1.0f, 0.0f);
    passRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    passRender.setLabel(label);
    passRender.startRenderPass(&target.getRender());
    if (program.render.beginDraw(offscreenState(useFloatTargets()))){
        ShaderData& data = program.shader->shaderData;
        bindTexture(program.render, data, "u_atmosphereTexture", &source.getRender().getColorTexture());
        bindTexture(program.render, data, "u_depthTexture", &depth.getRender().getColorTexture());
        applyBlock(program.render, data, "u_fs_atmosphereDenoise", sizeof(block), &block);
        program.render.draw(0, 3, 1);
    }
    passRender.endRenderPass();
    notePass(label);
}

void AtmosphereRenderer::renderFog(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier){
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    Framebuffer& depth = targets.depth[tier.fogDivisor == 2 ? 0 : 1];
    Program& program = programs[PROGRAM_FOG];
    volumetric_fog_t& block = fogBlock;
    block = volumetric_fog_t();

    const bool floatFormat = useFloatTargets();
    Framebuffer& target = targets.fog[0];
    block.target = Vector4(1.0f / (float)target.getWidth(), 1.0f / (float)target.getHeight(), (float)std::min(tier.fogSteps, ATMOSPHERE_MAX_FOG_STEPS),
                           fog->volumetricDistance);
    const bool orthographic = camera.type != CameraType::CAMERA_PERSPECTIVE;
    block.camera = Vector4(cameraForward(camera), orthographic ? 1.0f : 0.0f);
    // light shafts: the main directional light's cascades of this camera pass (S10's
    // u_fs_shadow_data, includes/shadow_sampling.glsl sampleDirectionalShadow1)
    const fs_shadow_data_t& shadowData = rs->ps().fs_shadow_data;
    const bool shafts = fog->lightShafts && rs->hasShadows && shadowData.header.x >= -0.5f;
    // the depth is linearised with the near and far the projection was built with
    const Vector2 nearFar = RenderSystem::getCameraNearFar(camera);
    block.depthInfo = Vector4(nearFar.y, nearFar.x, floatFormat ? 0.0f : 1.0f, shafts ? 1.0f : 0.0f);

    // the volumes: in view, within the volumetric distance, the nearest first, capped by the tier
    std::vector<int> selected;
    const int culled = AtmosphereMath::selectFogVolumes(volumes, camera.frustumPlanes, camera.farClip, cameraTransform.worldPosition,
                                                        fog->volumetricDistance, std::min(tier.fogVolumes, ATMOSPHERE_MAX_FOG_VOLUMES), selected);
    for (int i = 0; i < (int)selected.size(); i++){
        const FogVolumeData& volume = volumes[selected[i]];
        for (int r = 0; r < 3; r++)
            block.volumeFrame[i * 3 + r] = volume.worldToVolume.row(r);
        block.volumeShape[i] = Vector4(volume.halfExtents, volume.fog.shape == FogVolumeShape::SPHERE ? 1.0f : 0.0f);
        block.volumeMedium[i] = Vector4(1.0f / std::max(volume.fog.attenuationDistance, 0.01f), std::max(volume.fog.blendDistance, 0.0f),
                                        volume.fog.falloff == FogVolumeFalloff::EXPONENTIAL ? 1.0f : 0.0f, std::clamp(volume.fog.noiseIntensity, 0.0f, 1.0f));
        block.volumeAlbedo[i] = Vector4(volume.fog.albedo, 1.0f / std::max(volume.fog.noiseScale, 0.01f));
        block.volumeScroll[i] = Vector4(volume.fog.scrollSpeed * (float)(-time), 0.0f);
    }

    // point and spot lights (High), nearest the camera first
    int lightCount = 0;
    if (tier.fogLocalLights > 0){
        struct Candidate{ int index; float distance; Vector3 position; };
        std::vector<Candidate> candidates;
        Scene* scene = rs->scene;
        auto lights = scene->getComponentArray<LightComponent>();
        for (size_t i = 0; i < lights->size(); i++){
            const LightComponent& light = lights->getComponentFromIndex(i);
            const Entity entity = lights->getEntity(i);
            if (light.type == LightType::DIRECTIONAL || light.intensity <= 0.0f || light.volumetricIntensity <= 0.0f || !rs->isLightActive(entity, light))
                continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform)
                continue;
            const float distance = std::max((transform->worldPosition - cameraTransform.worldPosition).length() - std::max(light.range, 0.0f), 0.0f);
            if (distance > fog->volumetricDistance)
                continue;
            candidates.push_back({(int)i, distance, transform->worldPosition});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b){
            return a.distance != b.distance ? a.distance < b.distance : a.index < b.index;
        });
        for (const Candidate& candidate : candidates){
            if (lightCount >= std::min(tier.fogLocalLights, ATMOSPHERE_MAX_FOG_LIGHTS))
                break;
            const LightComponent& light = lights->getComponentFromIndex(candidate.index);
            block.lightPosition[lightCount] = Vector4(candidate.position, light.type == LightType::SPOT ? 2.0f : 1.0f);
            block.lightColor[lightCount] = Vector4(light.color * (light.intensity * light.volumetricIntensity), light.range);
            block.lightSpot[lightCount] = Vector4(light.worldDirection, 0.0f);
            block.lightCone[lightCount] = Vector4(light.innerConeCos, light.outerConeCos, 0.0f, 0.0f);
            lightCount++;
        }
    }

    const float noiseIntensity = std::clamp(fog->noiseIntensity, 0.0f, 1.0f);
    bool noiseUsed = noiseIntensity > 0.0f;
    for (int i = 0; i < (int)selected.size(); i++)
        noiseUsed = noiseUsed || block.volumeMedium[i].w > 0.0f;
    // the density noise is the clouds' detail noise: baked on first use
    const bool noiseReady = noiseUsed && cloudsReady();
    block.noise = Vector4(noiseReady ? noiseIntensity : 0.0f, 1.0f / std::max(fog->noiseScale, 0.01f), (float)selected.size(), (float)lightCount);
    if (!noiseReady){
        for (int i = 0; i < (int)selected.size(); i++)
            block.volumeMedium[i].w = 0.0f;
    }
    block.noiseOffset = Vector4(fogNoiseOffset, 0.0f);

    passRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    passRender.setLabel(PASS_LABELS[1]);
    passRender.startRenderPass(&target.getRender());
    if (program.render.beginDraw(offscreenState(floatFormat))){
        ShaderData& data = program.shader->shaderData;
        TextureRender* atlas = &RenderSystem::emptyShadowDepth;
        if (shafts){
            const RenderSystem::CameraShadows& shadows = rs->ps();
            if (shadows.projectiveAtlas)
                atlas = &shadows.projectiveAtlas->getDepthTexture();
            else if (rs->hasShadowAtlas)
                atlas = &rs->shadowAtlasFramebuffer.getDepthTexture();
        }
        bindTexture(program.render, data, "u_depthTexture", &depth.getRender().getColorTexture());
        bindTexture(program.render, data, "u_fogNoise", noiseReady ? &noiseDetail : &empty3D);
        bindTexture(program.render, data, "u_shadowAtlas", atlas);
        bindTexture(program.render, data, "u_blueNoise", &blueNoise);
        applyBlock(program.render, data, "u_fs_volumetricFog", sizeof(block), &block);
        applyBlock(program.render, data, "u_fs_heightFog", sizeof(passFog), &passFog);
        applyBlock(program.render, data, "u_fs_shadow_data", sizeof(shadowData), &shadowData);
        program.render.draw(0, 3, 1);
    }
    passRender.endRenderPass();
    notePass(PASS_LABELS[1]);

    building.volumetricActive = true;
    building.fogWidth = (int)target.getWidth();
    building.fogHeight = (int)target.getHeight();
    building.fogSteps = tier.fogSteps;
    building.volumesUsed = (int)selected.size();
    building.volumesCulled = culled;
    building.localLights = lightCount;
}

float AtmosphereRenderer::cloudsFade() const{
    if (noiseReadyTime < 0.0)
        return 0.0f;
    return (float)std::clamp((time - noiseReadyTime) / CLOUDS_FADE_IN_SECONDS, 0.0, 1.0);
}

float AtmosphereRenderer::cameraAltitudeFade(const Transform& cameraTransform) const{
    // the Sky Map is seen from below: it fades out over 200 m as the camera nears the cloud base
    const float start = clouds->bottomAltitude - 100.0f;
    return 1.0f - std::clamp((cameraTransform.worldPosition.y - start) / 200.0f, 0.0f, 1.0f);
}

TextureRender* AtmosphereRenderer::weatherTexture(){
    // the component's map once loaded, else the built-in one
    if (clouds->weatherMap.empty())
        return &noiseWeather;
    Texture* map = &clouds->weatherMap;
    // It always tiles. Its wrap is authored data, saved with the component, so another wrap's
    // map draws through a copy with the repeat sampler
    if (map->getWrapU() != TextureWrap::REPEAT || map->getWrapV() != TextureWrap::REPEAT){
        if (tiledWeatherSource != *map){
            tiledWeatherSource = *map;
            tiledWeatherMap = *map;
            tiledWeatherMap.setWrapU(TextureWrap::REPEAT);
            tiledWeatherMap.setWrapV(TextureWrap::REPEAT);
        }
        map = &tiledWeatherMap;
    }
    TextureRender* render = map->getRender(nullptr);
    return render && render->isCreated() ? render : &noiseWeather;
}

void AtmosphereRenderer::fillClouds(clouds_t& block, const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier,
                                    bool skyMap, float altitudeFade){
    block = clouds_t();
    block.invViewProjection = camera.viewProjectionMatrix.inverse();
    const bool orthographic = camera.type != CameraType::CAMERA_PERSPECTIVE;
    block.cameraPosition = Vector4(cameraTransform.worldPosition, camera.farClip);
    block.cameraForward = Vector4(cameraForward(camera), orthographic ? 1.0f : 0.0f);
    const float range = std::max(clouds->altitudeRange, 1.0f);
    block.layer = Vector4(clouds->bottomAltitude, range, std::clamp(clouds->earthCurvature, 0.0f, 1.0f) / (2.0f * EARTH_RADIUS),
                          std::max(clouds->maxDistance, 0.1f) * 1000.0f);
    // extinction at full density; tilings as periods of the noise in meters
    block.shape = Vector4(std::clamp(clouds->coverage, 0.0f, 1.0f), std::max(clouds->density, 0.0f) * 0.02f,
                          std::max(clouds->shapeScale, 0.01f) / 20000.0f, std::clamp(clouds->shapeFactor, 0.0f, 1.0f));
    block.erosion = Vector4(std::clamp(clouds->erosionFactor, 0.0f, 1.0f), std::max(clouds->erosionScale, 0.01f) / 20000.0f,
                            tier.cloudErosion ? 1.0f : 0.0f, 1.0f / (std::max(clouds->weatherMapScale, 0.01f) * 1000.0f));
    block.windOffset = Vector4(cloudShapeOffset.x, cloudShapeOffset.y, cloudWeatherOffset.x, cloudWeatherOffset.y);

    const frame_t& frame = rs->passFrame;
    Vector3 sunDirection(frame.mainLightDirection.x, frame.mainLightDirection.y, frame.mainLightDirection.z);
    if (sunDirection.length() <= 0.0f)
        sunDirection = Vector3(0.0f, 1.0f, 0.0f);
    // a sun below the horizon fades out over +-3 degrees
    const float sin3 = 0.05233596f;
    const float t = std::clamp((sunDirection.y + sin3) / (2.0f * sin3), 0.0f, 1.0f);
    const float sunFade = t * t * (3.0f - 2.0f * t);
    const Vector3 sunColor = Vector3(frame.mainLightColor.x, frame.mainLightColor.y, frame.mainLightColor.z) *
                             (frame.mainLightDirection.w * std::max(clouds->sunLightDimmer, 0.0f) * sunFade);
    block.sunDir_steps = Vector4(sunDirection, (float)std::clamp(tier.cloudSteps, 1, ATMOSPHERE_MAX_CLOUD_STEPS));
    block.sunColor_lightSteps = Vector4(sunColor, (float)std::clamp(tier.cloudLightSteps, 1, ATMOSPHERE_MAX_CLOUD_LIGHT_STEPS));
    const Vector3 ambient = Vector3(frame.ambient.x, frame.ambient.y, frame.ambient.z) * std::max(clouds->ambientLightDimmer, 0.0f);
    block.ambientBottom_near = Vector4(ambient * 0.5f, RenderSystem::getCameraNearFar(camera).x);
    block.ambientTop_fade = Vector4(ambient, cloudsFade() * altitudeFade);
    block.target = Vector4(0.0f, 0.0f, skyMap ? 1.0f : 0.0f, useFloatTargets() ? 0.0f : 1.0f);
    // aerial perspective by the height fog (the editor's Fog toggle leaves passFog clear)
    block.fog = Vector4(passFog.extinction_base_H_maxOpacity.x > 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
}

void AtmosphereRenderer::renderScreenClouds(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier){
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    Program& program = programs[PROGRAM_CLOUDS];
    Framebuffer& target = targets.clouds[0];
    fillClouds(cloudsBlock, camera, cameraTransform, tier, false, 1.0f);
    cloudsBlock.target.x = 1.0f / (float)target.getWidth();
    cloudsBlock.target.y = 1.0f / (float)target.getHeight();

    passRender.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    passRender.setLabel(PASS_LABELS[3]);
    passRender.startRenderPass(&target.getRender());
    if (program.render.beginDraw(offscreenState(useFloatTargets()))){
        ShaderData& data = program.shader->shaderData;
        bindTexture(program.render, data, "u_depthTexture", &targets.depth[1].getRender().getColorTexture());
        bindTexture(program.render, data, "u_cloudShape", &noiseShape);
        bindTexture(program.render, data, "u_cloudDetail", &noiseDetail);
        bindTexture(program.render, data, "u_weatherMap", weatherTexture());
        bindTexture(program.render, data, "u_blueNoise", &blueNoise);
        applyBlock(program.render, data, "u_fs_clouds", sizeof(cloudsBlock), &cloudsBlock);
        applyBlock(program.render, data, "u_fs_heightFog", sizeof(passFog), &passFog);
        program.render.draw(0, 3, 1);
    }
    passRender.endRenderPass();
    notePass(PASS_LABELS[3]);
}

uint64_t AtmosphereRenderer::skyMapKey(const Transform& cameraTransform, const AtmosphereTier& tier) const{
    uint64_t h = 1469598103934665603ULL;
    const clouds_t& block = cloudsBlock;
    // what the map shows: the clouds, their light and drift, the fog in front of them and where
    // they are seen from (to the meter)
    const Vector4* vectors[] = {&block.layer, &block.shape, &block.erosion, &block.windOffset, &block.sunDir_steps,
                                &block.sunColor_lightSteps, &block.ambientBottom_near, &block.ambientTop_fade, &block.fog};
    for (const Vector4* v : vectors){
        for (int c = 0; c < 4; c++)
            h = hashCombine(h, (*v)[c]);
    }
    const float* fogData = reinterpret_cast<const float*>(&passFog.color_colorMode);
    for (int i = 0; i < 4 * 9; i++)
        h = hashCombine(h, fogData[i]);
    h = hashCombine(h, std::round(cameraTransform.worldPosition.x));
    h = hashCombine(h, std::round(cameraTransform.worldPosition.y));
    h = hashCombine(h, std::round(cameraTransform.worldPosition.z));
    h = hashCombine(h, (float)tier.skyMapSize);
    h = hashCombine(h, (float)(clouds->weatherMap.empty() ? 0 : 1));
    return h;
}

void AtmosphereRenderer::renderSkyMap(const CameraComponent& camera, const Transform& cameraTransform, const AtmosphereTier& tier){
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    targets.used[3] = true;
    // one refresh step per frame and view
    if (targets.skyMapFrame == rs->drawCount + 1)
        return;
    targets.skyMapFrame = rs->drawCount + 1;

    const bool floatFormat = useFloatTargets();
    const int size = std::max(tier.skyMapSize, 16);
    if (targets.skyMapSize != size || targets.compressed == floatFormat){
        for (Framebuffer& map : targets.skyMap)
            map.destroy();
        targets.skyMapSize = size;
        targets.skyMapCurrent = -1;
        targets.skyMapPrevious = -1;
        targets.skyMapBuilding = 0;
        targets.skyMapRow = 0;
    }
    targets.compressed = !floatFormat;
    for (Framebuffer& map : targets.skyMap){
        if (!ensureTarget(map, (unsigned int)size, (unsigned int)size, floatFormat, true))
            return;
    }
    targets.skyMapSlices = std::max(tier.skyMapSliceFrames, 1);

    fillClouds(cloudsBlock, camera, cameraTransform, tier, true, 1.0f);
    cloudsBlock.target.x = 1.0f / (float)size;
    cloudsBlock.target.y = 1.0f / (float)size;
    // the crossfade toward the newest map, over the frames a refresh takes
    if (targets.skyMapPrevious >= 0)
        targets.skyMapFadeFrames++;

    const uint64_t key = skyMapKey(cameraTransform, tier);
    const bool idle = targets.skyMapRow == 0;
    if (idle && targets.skyMapCurrent >= 0 && key == targets.skyMapCurrentKey)
        return; // nothing changed: no refresh
    if (idle)
        targets.skyMapBuildingKey = key;

    Program& program = programs[PROGRAM_CLOUDS];
    const int rows = (size + targets.skyMapSlices - 1) / targets.skyMapSlices;
    const int first = targets.skyMapRow;
    const int count = std::min(rows, size - first);
    Framebuffer& map = targets.skyMap[targets.skyMapBuilding];

    passRender.setColorActions(LoadAction::LOAD, StoreAction::STORE);
    passRender.setLabel(PASS_LABELS[4]);
    passRender.startRenderPass(&map.getRender());
    passRender.applyViewportTopLeft(0, first, size, count);
    passRender.applyScissorTopLeft(0, first, size, count);
    if (program.render.beginDraw(offscreenState(floatFormat))){
        ShaderData& data = program.shader->shaderData;
        bindTexture(program.render, data, "u_depthTexture", &RenderSystem::emptyWhite);
        bindTexture(program.render, data, "u_cloudShape", &noiseShape);
        bindTexture(program.render, data, "u_cloudDetail", &noiseDetail);
        bindTexture(program.render, data, "u_weatherMap", weatherTexture());
        bindTexture(program.render, data, "u_blueNoise", &blueNoise);
        applyBlock(program.render, data, "u_fs_clouds", sizeof(cloudsBlock), &cloudsBlock);
        applyBlock(program.render, data, "u_fs_heightFog", sizeof(passFog), &passFog);
        program.render.draw(0, 3, 1);
    }
    passRender.endRenderPass();
    notePass(PASS_LABELS[4]);

    targets.skyMapRow = first + count;
    if (targets.skyMapRow >= size){
        // the map is complete: it becomes the current one, the current the previous
        const int done = targets.skyMapBuilding;
        targets.skyMapPrevious = targets.skyMapCurrent;
        targets.skyMapCurrent = done;
        targets.skyMapCurrentKey = targets.skyMapBuildingKey;
        targets.skyMapFadeFrames = 0;
        for (int i = 0; i < 3; i++){
            if (i != targets.skyMapCurrent && i != targets.skyMapPrevious){
                targets.skyMapBuilding = i;
                break;
            }
        }
        targets.skyMapRow = 0;
    }
}

float AtmosphereRenderer::renderPassTargets(PassPlan& plan, const CameraComponent& camera, const Transform& cameraTransform,
                                            TextureRender* sceneDepth, bool forcedDepth, unsigned int width, unsigned int height){
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    // the medium and the sun of the camera for the marches (they make their own rays)
    fillHeightFog(passFog, camera, cameraTransform, Rect(0.0f, 0.0f, 1.0f, 1.0f), 1, false, 0.0f);
    const AtmosphereQuality quality = viewQuality();
    passSceneDepth = sceneDepth;
    building.forcedDepthPrePass = building.forcedDepthPrePass || (forcedDepth && sceneDepth);

    if (plan.needsDepth() && !sceneDepth){
        plan.volumetricFog = false;
        plan.screenClouds = false;
    }
    const bool cloudsPlanned = plan.skyMapClouds || plan.screenClouds;
    if (cloudsPlanned && !cloudsReady())
        waiting = true;
    if (cloudsPlanned && (!cloudsReady() || !loadProgram(PROGRAM_CLOUDS))){
        plan.skyMapClouds = false;
        plan.screenClouds = false;
    }
    if (plan.needsDepth() && (!loadProgram(PROGRAM_DEPTH) || !loadProgram(PROGRAM_DENOISE)
            || (plan.volumetricFog && !loadProgram(PROGRAM_FOG)) || !loadProgram(PROGRAM_VOLUMETRIC_COMPOSITE))){
        plan.volumetricFog = false;
        plan.screenClouds = false;
    }
    if (plan.skyComposite && !loadProgram(PROGRAM_SKY_COMPOSITE)){
        plan.skyComposite = false;
        plan.skyFill = false;
        plan.skyMapClouds = false;
    }

    const float resolutionX = (float)std::max(width, 1u);
    const float resolutionY = (float)std::max(height, 1u);
    const bool floatFormat = useFloatTargets();
    if (plan.needsDepth()){
        targets.used[0] = true;
        if (targets.compressed == floatFormat){
            for (Framebuffer& target : targets.fog)
                target.destroy();
            for (Framebuffer& target : targets.clouds)
                target.destroy();
            targets.compressed = !floatFormat;
        }
    }

    float marched = 0.0f;
    bool depthReady[2] = {false, false};
    if (plan.volumetricFog){
        const AtmosphereTier tier = AtmosphereTiers::get(AtmosphereTiers::apply(quality, fog->qualityOverride));
        const int level = tier.fogDivisor == 2 ? 0 : 1;
        const unsigned int fogWidth = (unsigned int)std::ceil(resolutionX / (float)tier.fogDivisor);
        const unsigned int fogHeight = (unsigned int)std::ceil(resolutionY / (float)tier.fogDivisor);
        if (ensureTarget(targets.depth[level], fogWidth, fogHeight, false, false) && ensureTarget(targets.fog[0], fogWidth, fogHeight, floatFormat, false)
                && ensureTarget(targets.fog[1], fogWidth, fogHeight, floatFormat, false)){
            targets.used[1] = true;
            renderDepth(targets.depth[level], sceneDepth, tier.fogDivisor, camera);
            depthReady[level] = true;
            renderFog(camera, cameraTransform, tier);
            renderDenoise(targets.fog[0], targets.fog[1], targets.depth[level], PASS_LABELS[2]);
            marched = std::max(fog->volumetricDistance, 0.0f);
            fogDepthLevel = level;
        }else{
            plan.volumetricFog = false;
        }
    }

    if (plan.screenClouds || plan.skyMapClouds){
        const AtmosphereTier tier = AtmosphereTiers::get(AtmosphereTiers::apply(quality, clouds->qualityOverride));
        building.cloudsActive = true;
        building.cloudsMode = plan.screenClouds ? CloudsRendering::SCREEN : CloudsRendering::SKY_MAP;
        if (plan.screenClouds){
            const unsigned int cloudsWidth = (unsigned int)std::ceil(resolutionX / (float)tier.cloudScreenDivisor);
            const unsigned int cloudsHeight = (unsigned int)std::ceil(resolutionY / (float)tier.cloudScreenDivisor);
            if (ensureTarget(targets.depth[1], cloudsWidth, cloudsHeight, false, false) && ensureTarget(targets.clouds[0], cloudsWidth, cloudsHeight, floatFormat, false)
                    && ensureTarget(targets.clouds[1], cloudsWidth, cloudsHeight, floatFormat, false)){
                targets.used[2] = true;
                if (!depthReady[1])
                    renderDepth(targets.depth[1], sceneDepth, tier.cloudScreenDivisor, camera);
                renderScreenClouds(camera, cameraTransform, tier);
                renderDenoise(targets.clouds[0], targets.clouds[1], targets.depth[1], PASS_LABELS[2]);
            }else{
                plan.screenClouds = false;
            }
        }
        // the Sky Map: refreshed by the view's main camera for the cameras that read it (the
        // Sky Map mode's, or the other cameras of a Screen mode scene)
        const bool mapUsers = plan.skyMapClouds || rs->hasMultipleCameras || rs->hasReflectionProbes;
        if (plan.mainCamera && mapUsers){
            renderSkyMap(camera, cameraTransform, tier);
            building.mapSize = targets.skyMapSize;
            building.sliceFrames = targets.skyMapSlices;
        }
        if (targets.skyMapCurrent < 0)
            plan.skyMapClouds = false;
        plan.skyComposite = plan.skyFill || plan.skyMapClouds;
    }
    return marched;
}

void AtmosphereRenderer::bindPostColor(ObjectRender& render, ShaderData& data){
    applyBlock(render, data, "u_fs_postColor", sizeof(post_color_t), &rs->passPostColors[POST_BLEND_OWN]);
    bindTexture(render, data, "u_postColorLut", rs->postTailLut.isCreated() ? &rs->postTailLut : &RenderSystem::emptyWhite);
}

void AtmosphereRenderer::drawComposites(const PassPlan& plan, PipelineType pipelineType, int msaaSamples, const Rect& viewport,
                                        unsigned int targetHeight, bool flipped, const CameraComponent& camera, const Transform& cameraTransform){
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    PipelineState state;
    state.target = targetFormatFor(pipelineType, msaaSamples);
    state.blend = BlendMode::PREMULTIPLIED;
    state.depthWrite = false;

    if (plan.skyComposite){
        Program& program = programs[PROGRAM_SKY_COMPOSITE];
        sky_composite_t block;
        TextureRender* current = &RenderSystem::emptyBlack;
        TextureRender* previous = &RenderSystem::emptyBlack;
        float crossfade = 1.0f;
        float opacity = 0.0f;
        if (plan.skyMapClouds && targets.skyMapCurrent >= 0){
            current = &targets.skyMap[targets.skyMapCurrent].getRender().getColorTexture();
            previous = current;
            if (targets.skyMapPrevious >= 0){
                crossfade = std::min(1.0f, (float)(targets.skyMapFadeFrames + 1) / (float)std::max(targets.skyMapSlices, 1));
                // a finished crossfade reads the current map alone (bit-identical frames)
                if (crossfade < 1.0f)
                    previous = &targets.skyMap[targets.skyMapPrevious].getRender().getColorTexture();
            }
            opacity = cloudsFade() * cameraAltitudeFade(cameraTransform);
        }
        block.params = Vector4(crossfade, targets.compressed ? 1.0f : 0.0f, opacity, 0.0f);
        state.depthCompare = CompareFunc::LESS_EQUAL;
        if (program.render.beginDraw(state)){
            ShaderData& data = program.shader->shaderData;
            bindTexture(program.render, data, "u_cloudsMap", current);
            bindTexture(program.render, data, "u_cloudsMapPrevious", previous);
            applyBlock(program.render, data, "u_fs_skyComposite", sizeof(block), &block);
            applyBlock(program.render, data, "u_fs_heightFog", sizeof(height_fog_t), &rs->passHeightFog);
            bindPostColor(program.render, data);
            program.render.draw(0, 3, 1);
        }
        notePass(PASS_LABELS[5]);
    }

    if ((plan.volumetricFog || plan.screenClouds) && passSceneDepth){
        Program& program = programs[PROGRAM_VOLUMETRIC_COMPOSITE];
        volumetric_composite_t block;
        const bool gl = Engine::isOpenGL();
        const float width = std::max(viewport.getWidth(), 1.0f);
        const float height = std::max(viewport.getHeight(), 1.0f);
        const float rowStart = gl ? viewport.getY() : (float)targetHeight - viewport.getY() - viewport.getHeight();
        const bool topFirst = gl ? flipped : !flipped;
        block.screenToUV = Vector4(1.0f / width, topFirst ? -1.0f / height : 1.0f / height,
                                   -viewport.getX() / width, topFirst ? 1.0f + rowStart / height : -rowStart / height);
        Framebuffer& fogTarget = targets.fog[1];
        Framebuffer& cloudsTarget = targets.clouds[1];
        auto size = [](Framebuffer& target){
            const float w = (float)std::max(target.getWidth(), 1u);
            const float h = (float)std::max(target.getHeight(), 1u);
            return Vector4(1.0f / w, 1.0f / h, w, h);
        };
        block.fogSize = plan.volumetricFog ? size(fogTarget) : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        block.cloudsSize = plan.screenClouds ? size(cloudsTarget) : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        block.params = Vector4(plan.volumetricFog ? 1.0f : 0.0f, plan.screenClouds ? 1.0f : 0.0f, targets.compressed ? 1.0f : 0.0f, 0.0f);
        block.depthInfo = Vector4(1.0f / std::max(camera.farClip, 1e-3f), 0.0f, 0.0f, 0.0f);
        inverseProjectionRows(camera, block.inverseProjectionZ, block.inverseProjectionW);
        state.depthCompare = CompareFunc::ALWAYS;
        // the subtract pass, then the blend pass (volumetric_composite.frag)
        for (int composite = 0; composite < 2; composite++){
            block.params.w = (float)composite;
            state.blend = composite == 0 ? BlendMode::SUBTRACT_PREMULTIPLIED : BlendMode::PREMULTIPLIED;
            if (!program.render.beginDraw(state))
                break;
            ShaderData& data = program.shader->shaderData;
            bindTexture(program.render, data, "u_depthTexture", passSceneDepth);
            if (plan.volumetricFog){
                bindTexture(program.render, data, "u_fogTexture", &fogTarget.getRender().getColorTexture());
                bindTexture(program.render, data, "u_fogDepth", &targets.depth[fogDepthLevel].getRender().getColorTexture());
            }else{
                bindTexture(program.render, data, "u_fogTexture", &RenderSystem::emptyBlack);
                bindTexture(program.render, data, "u_fogDepth", &RenderSystem::emptyWhite);
            }
            if (plan.screenClouds){
                bindTexture(program.render, data, "u_cloudsTexture", &cloudsTarget.getRender().getColorTexture());
                bindTexture(program.render, data, "u_cloudsDepth", &targets.depth[1].getRender().getColorTexture());
            }else{
                bindTexture(program.render, data, "u_cloudsTexture", &RenderSystem::emptyBlack);
                bindTexture(program.render, data, "u_cloudsDepth", &RenderSystem::emptyWhite);
            }
            applyBlock(program.render, data, "u_fs_volumetricComposite", sizeof(block), &block);
            applyBlock(program.render, data, "u_fs_heightFog", sizeof(height_fog_t), &rs->passHeightFog);
            bindPostColor(program.render, data);
            program.render.draw(0, 3, 1);
        }
        notePass(PASS_LABELS[6]);
    }
}

void AtmosphereRenderer::endFrame(){
    building.quality = viewQuality();
    building.platformDefault = Engine::getAtmosphereQualityDefault();
    building.fog = fogPresent;
    building.heightFog = heightFogActive;
    building.volumetric = heightFogActive && fog->volumetric;
    building.clouds = clouds != nullptr;
    building.noiseReady = noiseShape.isCreated();
    building.floatTargets = useFloatTargets();
    building.horizonSource = horizonSource;
    building.frame = rs->drawCount;
    if (!building.clouds)
        building.cloudsActive = false;
    AtmosphereViewTargets& targets = rs->rv().atmosphere;
    targets.releaseUnused();
    building.vramBytes = targets.getBytes();
    building.vramScreenBytes = targets.getScreenBytes();
    if (noiseShape.isCreated())
        building.vramBytes += (size_t)AtmosphereNoise::SHAPE_SIZE * AtmosphereNoise::SHAPE_SIZE * AtmosphereNoise::SHAPE_SIZE * 4
                            + (size_t)AtmosphereNoise::DETAIL_SIZE * AtmosphereNoise::DETAIL_SIZE * AtmosphereNoise::DETAIL_SIZE * 4
                            + (size_t)AtmosphereNoise::WEATHER_SIZE * AtmosphereNoise::WEATHER_SIZE * 4;
    targets.info = building;
    building = AtmosphereInfo();
    loading = waiting;
    waiting = false;
    passSceneDepth = nullptr;
}

void AtmosphereRenderer::destroy(){
    tiledWeatherSource = Texture();
    tiledWeatherMap = Texture();
    for (int kind = 0; kind < PROGRAM_COUNT; kind++){
        Program& program = programs[kind];
        if (program.loaded)
            program.render.destroy();
        program.loaded = false;
        if (program.shader){
            program.shader.reset();
            ShaderPool::remove(PROGRAM_TYPES[kind], 0);
        }
    }
}
