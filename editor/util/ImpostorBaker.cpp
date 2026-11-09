// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/ImpostorBaker.h"

#include "Engine.h"
#include "Log.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "object/Camera.h"
#include "render/FrameReadback.h"
#include "render/preview/ModelRender.h"
#include "terrain/TerrainOps.h"
#include "util/Fnv.h"

#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    struct BakeState {
        editor::Project* project = nullptr;
        editor::ImpostorBaker::Request request;
        editor::ImpostorBaker::Finished finished;
        editor::ImpostorBaker::Progress progress;
        bool cancelled = false;

        size_t prototypeCursor = 0;
        int view = 0;
        bool normalPass = false;
        bool loaded = false;
        bool waitingFrame = false;
        int settleFrames = 0;

        std::unique_ptr<editor::ModelRender> render;
        Vector3 center;
        float width = 1.0f, height = 1.0f, depth = 1.0f;
        std::vector<uint8_t> albedo, normal;
        int atlasWidth = 0, atlasHeight = 0;
        std::chrono::steady_clock::time_point started;

        std::vector<editor::ImpostorBakeResult> results;
        std::vector<jobs::Task<void>> writes;
    };

    std::unique_ptr<BakeState>& bakeState() {
        static std::unique_ptr<BakeState> state;
        return state;
    }

    void pngToVector(void* context, void* data, int size) {
        std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    }

    // The op's own validation and dry-run answer: rendering is the handler's, over frames (T1 R89)
    bool validateBake(const terraincore::TerrainMapStore&, const terraincore::Op& op, terraincore::OpError& error) {
        if (const terraincore::OpParam* views = op.params.find("views")) {
            const int value = static_cast<int>(views->number);
            if (value != 4 && value != 8 && value != 16) {
                error.field = "views";
                error.message = "valid: 4, 8, 16";
                return false;
            }
        }
        if (const terraincore::OpParam* resolution = op.params.find("resolution")) {
            const int value = static_cast<int>(resolution->number);
            if (value != 128 && value != 256 && value != 512) {
                error.field = "resolution";
                error.message = "valid: 128, 256, 512";
                return false;
            }
        }
        return true;
    }

    void applyBake(terraincore::OpContext& context, const terraincore::Op&, terraincore::OpEntry&) {
        if (!context.dryRun()) {
            context.fail("bake_impostors runs over frames: call it through edit_terrain.");
        }
    }

}

void editor::ImpostorBaker::atlasGrid(int views, int& columns, int& rows) {
    columns = views <= 4 ? 2 : 4;
    rows = (views + columns - 1) / columns;
}

std::string editor::ImpostorBaker::sourceHash(Project* project, const std::string& meshPath) {
    std::ifstream file(project->resolveAssetPath(meshPath), std::ios::binary);
    if (!file) return std::string();
    std::stringstream bytes;
    bytes << file.rdbuf();
    const std::string content = bytes.str();
    const uint64_t hash = fnv1a64(content.data(), content.size());
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

editor::ImpostorState editor::ImpostorBaker::stateOf(Project* project, const std::string& meshPath, int views, const std::string& storedHash) {
    if (views <= 0) return ImpostorState::None;
    return sourceHash(project, meshPath) == storedHash ? ImpostorState::Baked : ImpostorState::OutOfDate;
}

void editor::ImpostorBaker::registerOp() {
    if (terraincore::registeredOpExists("bake_impostors")) return;
    terraincore::RegisteredOpTraits traits;
    traits.spansFrames = true;
    terraincore::registerOp("bake_impostors", validateBake, applyBake, traits);
}

bool editor::ImpostorBaker::encodePng(const std::vector<uint8_t>& rgba, int width, int height, std::vector<uint8_t>& png) {
    png.clear();
    return stbi_write_png_to_func(pngToVector, &png, width, height, 4, rgba.data(), width * 4) != 0;
}

void editor::ImpostorBaker::dilate(std::vector<uint8_t>& rgba, int width, int height, int passes) {
    std::vector<uint8_t> next = rgba;
    for (int pass = 0; pass < passes; pass++) {
        bool any = false;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const size_t at = (static_cast<size_t>(y) * width + x) * 4;
                if (rgba[at + 3] != 0) continue;
                int r = 0, g = 0, b = 0, count = 0;
                for (int oy = -1; oy <= 1; oy++) {
                    for (int ox = -1; ox <= 1; ox++) {
                        const int nx = x + ox, ny = y + oy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        const size_t other = (static_cast<size_t>(ny) * width + nx) * 4;
                        if (rgba[other + 3] == 0) continue;
                        r += rgba[other];
                        g += rgba[other + 1];
                        b += rgba[other + 2];
                        count++;
                    }
                }
                if (count > 0) {
                    next[at] = static_cast<uint8_t>(r / count);
                    next[at + 1] = static_cast<uint8_t>(g / count);
                    next[at + 2] = static_cast<uint8_t>(b / count);
                    // marked, so the next pass grows from here; the alpha is put back to 0 at the end
                    next[at + 3] = 1;
                    any = true;
                }
            }
        }
        rgba = next;
        if (!any) break;
    }
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] == 1) rgba[i] = 0;
    }
}

void editor::ImpostorBaker::applyResults(Project* project, uint32_t sceneId, Entity entity, const std::vector<ImpostorBakeResult>& results) {
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* terrain = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!terrain) return;
    std::vector<TerrainTreePrototype> prototypes = terrain->treePrototypes;
    bool changed = false;
    for (const ImpostorBakeResult& result : results) {
        if (!result.error.empty() || result.prototype < 0 || result.prototype >= static_cast<int>(prototypes.size())) continue;
        TerrainTreePrototype& prototype = prototypes[result.prototype];
        prototype.impostorAlbedo = Texture(result.albedoPath);
        prototype.impostorNormal = Texture(result.normalPath);
        prototype.impostorViews = result.views;
        prototype.impostorBoundsCenter = Vector3(result.boundsCenter[0], result.boundsCenter[1], result.boundsCenter[2]);
        prototype.impostorSize = Vector2(result.size[0], result.size[1]);
        prototype.impostorSourceHash = result.sourceHash;
        changed = true;
    }
    if (changed) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new PropertyCmd<std::vector<TerrainTreePrototype>>(
            project, sceneId, entity, ComponentType::TerrainComponent, "treePrototypes", prototypes));
    }
}

bool editor::ImpostorBaker::isRunning() {
    return bakeState() != nullptr;
}

void editor::ImpostorBaker::cancel() {
    if (bakeState()) bakeState()->cancelled = true;
}

bool editor::ImpostorBaker::start(Project* project, const Request& request, Finished finished, Progress progress, std::string& error) {
    if (isRunning()) {
        error = "A bake is running; wait for it or cancel it.";
        return false;
    }
    SceneProject* sceneProject = project->getScene(request.sceneId);
    TerrainComponent* terrain = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(request.terrain) : nullptr;
    if (!terrain) {
        error = "The Terrain is not loaded.";
        return false;
    }
    if (request.prototypes.empty()) {
        error = "No prototype to bake.";
        return false;
    }
    for (int prototype : request.prototypes) {
        if (prototype < 0 || prototype >= static_cast<int>(terrain->treePrototypes.size())) {
            error = "No tree prototype " + std::to_string(prototype) + ": the Terrain has " + std::to_string(terrain->treePrototypes.size()) + ".";
            return false;
        }
    }
    auto state = std::make_unique<BakeState>();
    state->project = project;
    state->request = request;
    state->finished = std::move(finished);
    state->progress = std::move(progress);
    state->started = std::chrono::steady_clock::now();
    bakeState() = std::move(state);
    return true;
}

namespace {

    void prepareBakeMaterials(Scene* scene, bool normalPass, const Vector3& right, const Vector3& up, const Vector3& forward) {
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (int m = 0; m < meshes->size(); m++) {
            MeshComponent& mesh = meshes->getComponentFromIndex(m);
            for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
                Material& material = mesh.submeshes[s].material;
                const bool first = material.shader.empty() || material.shader != std::string("library:impostor_bake");
                if (first) {
                    material.shader = "library:impostor_bake";
                    material.shaderTextures.clear();
                    material.shaderTextures.emplace_back("u_baseTexture", material.baseColorTexture);
                    mesh.needReload = true;
                }
                const bool wantsNormal = std::find(material.shaderFeatures.begin(), material.shaderFeatures.end(), "IMPOSTOR_NORMAL") != material.shaderFeatures.end();
                if (wantsNormal != normalPass) {
                    material.shaderFeatures.clear();
                    if (normalPass) material.shaderFeatures.push_back("IMPOSTOR_NORMAL");
                    mesh.needReload = true;
                }
                ShaderUniforms::set(material.shaderParams, "baseColor", material.baseColorFactor);
                ShaderUniforms::set(material.shaderParams, "viewRight", Vector4(right.x, right.y, right.z, 0.0f));
                ShaderUniforms::set(material.shaderParams, "viewUp", Vector4(up.x, up.y, up.z, 0.0f));
                ShaderUniforms::set(material.shaderParams, "viewForward", Vector4(forward.x, forward.y, forward.z, 0.0f));
                ShaderUniforms::set(material.shaderParams, "alphaCutoff", Vector4(0.5f, 0.0f, 0.0f, 0.0f));
            }
        }
    }

    void finishPrototype(BakeState& state) {
        const int prototype = state.request.prototypes[state.prototypeCursor];
        editor::SceneProject* sceneProject = state.project->getScene(state.request.sceneId);
        TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(state.request.terrain);
        const std::string meshPath = terrain.treePrototypes[prototype].meshPath.str();

        // the normal atlas's alpha is ambient occlusion, full here; the cutout is the albedo's
        editor::ImpostorBaker::dilate(state.albedo, state.atlasWidth, state.atlasHeight, 8);
        for (size_t i = 0; i + 3 < state.normal.size(); i += 4) {
            if (state.albedo[i + 3] == 0) {
                state.normal[i] = 128;
                state.normal[i + 1] = 128;
                state.normal[i + 2] = 255;
            }
            state.normal[i + 3] = 255;
        }

        editor::ImpostorBakeResult result;
        result.prototype = prototype;
        const std::string stem = "terrain_edit_impostor_" + std::to_string(state.request.sceneId) + "_" + std::to_string(state.request.terrain) + "_" + std::to_string(terrain.treePrototypes[prototype].id);
        const fs::path directory = state.project->getTerrainMapsDir();
        std::error_code relativeError;
        result.albedoPath = fs::relative(directory / (stem + "_albedo.png"), state.project->getAssetsPath(), relativeError).generic_string();
        result.normalPath = fs::relative(directory / (stem + "_normal.png"), state.project->getAssetsPath(), relativeError).generic_string();
        result.boundsCenter[0] = state.center.x;
        result.boundsCenter[1] = state.center.y;
        result.boundsCenter[2] = state.center.z;
        result.size[0] = state.width;
        result.size[1] = state.height;
        result.views = state.request.views;
        result.sourceHash = editor::ImpostorBaker::sourceHash(state.project, meshPath);
        result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state.started).count();
        state.results.push_back(result);

        // R68: the encode and the write on a job, through writeFileAtomic
        auto write = [albedo = std::move(state.albedo), normal = std::move(state.normal), width = state.atlasWidth, height = state.atlasHeight,
                      albedoFile = state.project->resolveAssetPath(result.albedoPath), normalFile = state.project->resolveAssetPath(result.normalPath)]() {
            std::vector<uint8_t> png;
            if (editor::ImpostorBaker::encodePng(albedo, width, height, png)) {
                io::writeFileAtomic(albedoFile, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
            }
            if (editor::ImpostorBaker::encodePng(normal, width, height, png)) {
                io::writeFileAtomic(normalFile, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
            }
        };
        if (jobs::hasSystem()) {
            state.writes.push_back(jobs::submit(std::move(write), {jobs::Lane::Background, jobs::GateClass::Default, "impostor-write"}));
        } else {
            write();
        }
        state.albedo.clear();
        state.normal.clear();
    }

}

void editor::ImpostorBaker::update() {
    std::unique_ptr<BakeState>& slot = bakeState();
    if (!slot) return;
    BakeState& state = *slot;

    const auto finish = [&](bool cancelled) {
        for (jobs::Task<void>& write : state.writes) write.wait();
        if (state.render && state.render->getScene()) Engine::removeScene(state.render->getScene());
        Finished finished = state.finished;
        std::vector<ImpostorBakeResult> results = std::move(state.results);
        slot.reset();
        if (finished) finished(results, cancelled);
    };

    if (state.cancelled) {
        finish(true);
        return;
    }

    if (state.prototypeCursor >= state.request.prototypes.size()) {
        finish(false);
        return;
    }

    SceneProject* sceneProject = state.project->getScene(state.request.sceneId);
    TerrainComponent* terrain = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(state.request.terrain) : nullptr;
    if (!terrain) {
        finish(true);
        return;
    }

    const int prototype = state.request.prototypes[state.prototypeCursor];
    int columns = 4, rows = 2;
    atlasGrid(state.request.views, columns, rows);
    const int resolution = state.request.resolution;

    if (!state.loaded) {
        state.render = std::make_unique<ModelRender>();
        const std::string meshFile = state.project->resolveAssetPath(terrain->treePrototypes[prototype].meshPath.str()).string();
        if (!state.render->loadModel(meshFile)) {
            ImpostorBakeResult failed;
            failed.prototype = prototype;
            failed.error = "Could not load " + meshFile;
            state.results.push_back(failed);
            state.prototypeCursor++;
            return;
        }
        // the whole model's bounds, in the model's own units
        AABB bounds;
        bounds.setNull();
        auto meshes = state.render->getScene()->getComponentArray<MeshComponent>();
        for (int m = 0; m < meshes->size(); m++) bounds.merge(meshes->getComponentFromIndex(m).verticesAABB);
        if (bounds.isNull() || bounds.isInfinite()) bounds = AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(0.5f, 1.0f, 0.5f));
        state.center = bounds.getCenter();
        const Vector3 half = bounds.getHalfSize();
        // turning about Y: what any view can see across
        state.width = 2.0f * std::sqrt(half.x * half.x + half.z * half.z);
        state.height = 2.0f * half.y;
        state.depth = state.width + 2.0f;
        state.atlasWidth = columns * resolution;
        state.atlasHeight = rows * resolution;
        state.albedo.assign(static_cast<size_t>(state.atlasWidth) * state.atlasHeight * 4, 0);
        state.normal.assign(state.albedo.size(), 0);
        state.view = 0;
        state.normalPass = false;
        state.loaded = true;
        state.waitingFrame = false;
        state.settleFrames = 0;
    }

    Scene* scene = state.render->getScene();
    if (Engine::isSceneRunning(scene)) return;

    if (state.waitingFrame) {
        state.waitingFrame = false;
        std::vector<uint8_t> pixels;
        std::string readError;
        Framebuffer* framebuffer = state.render->getFramebuffer();
        if (framebuffer && FrameReadback::readFramebuffer(*framebuffer, pixels, &readError)) {
            const int column = state.view % columns;
            const int row = state.view / columns;
            std::vector<uint8_t>& target = state.normalPass ? state.normal : state.albedo;
            const int width = framebuffer->getWidth();
            for (int y = 0; y < resolution && y < framebuffer->getHeight(); y++) {
                const uint8_t* source = pixels.data() + static_cast<size_t>(y) * width * 4;
                uint8_t* out = target.data() + (static_cast<size_t>(row * resolution + y) * state.atlasWidth + static_cast<size_t>(column * resolution)) * 4;
                std::copy(source, source + static_cast<size_t>(std::min(width, resolution)) * 4, out);
            }
        } else {
            Log::warn("Impostor bake could not read a view: %s", readError.c_str());
        }
        if (state.normalPass) {
            state.view++;
            state.normalPass = false;
        } else {
            state.normalPass = true;
        }
        if (state.progress) {
            const double done = static_cast<double>(state.prototypeCursor) + static_cast<double>(state.view) / static_cast<double>(state.request.views);
            state.progress(done / static_cast<double>(state.request.prototypes.size()));
        }
        if (state.view >= state.request.views) {
            finishPrototype(state);
            Engine::removeScene(scene);
            state.render.reset();
            state.loaded = false;
            state.prototypeCursor++;
            return;
        }
    }

    // this view: an orthographic camera fitted to the bounds, the same frame the billboard draws in
    const float angle = (static_cast<float>(state.view) + 0.0f) / static_cast<float>(state.request.views) * 6.28318530718f;
    const Vector3 forward(std::sin(angle), 0.0f, std::cos(angle));
    const Vector3 right(forward.z, 0.0f, -forward.x);
    const Vector3 up(0.0f, 1.0f, 0.0f);
    prepareBakeMaterials(scene, state.normalPass, right, up, forward);

    Camera camera(scene, scene->getCamera());
    camera.setType(CameraType::CAMERA_ORTHO);
    camera.setRenderToTexture(true);
    camera.setFramebufferSize(resolution, resolution);
    camera.setPosition(state.center + forward * state.depth);
    camera.setTarget(state.center);
    camera.setLeftClip(-state.width * 0.5f);
    camera.setRightClip(state.width * 0.5f);
    camera.setBottomClip(-state.height * 0.5f);
    camera.setTopClip(state.height * 0.5f);
    camera.setNearClip(0.1f);
    camera.setFarClip(state.depth * 2.0f + state.width);
    scene->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);

    // a changed material takes a frame to reload its shader
    if (state.settleFrames < 1) {
        state.settleFrames++;
    }
    Engine::executeSceneOnce(scene);
    state.waitingFrame = true;
}
