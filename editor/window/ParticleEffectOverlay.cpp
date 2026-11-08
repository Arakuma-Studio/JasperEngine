// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectOverlay.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "component/CameraComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "object/Camera.h"
#include "pixelart/particles/ParticleCapture.h"
#include "render/SceneRender.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a seek spreads its ticks over frames at most this long (S20 R29)
    constexpr double kSeekBudgetMs = 4.0;

    // the Renderer's Bake to Sprite Sheet... asks the overlay of the scene
    struct BakeRequest {
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool pending = false;
    };
    BakeRequest& bakeRequest() {
        static BakeRequest request;
        return request;
    }

    bool editable(const SceneProject* sceneProject) {
        return sceneProject && sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED &&
               sceneProject->scene->isComponentRegistered<ParticleSystemComponent>();
    }

}

ParticleEffectOverlay::Gizmos& ParticleEffectOverlay::gizmos() {
    static Gizmos settings;
    return settings;
}

std::vector<Entity> ParticleEffectOverlay::targets(Project* project, SceneProject* sceneProject) {
    std::vector<Entity> result;
    if (!editable(sceneProject)) return result;
    Scene* scene = sceneProject->scene;
    const State& state = states[sceneProject->id];
    if (state.simulateAll) {
        auto systems = scene->getComponentArray<ParticleSystemComponent>();
        for (size_t i = 0; i < systems->size(); i++) result.push_back(systems->getEntity(i));
        return result;
    }
    std::vector<Entity> candidates = project->getSelectedEntities(sceneProject->id);
    candidates.insert(candidates.end(), state.extra.begin(), state.extra.end());
    for (Entity entity : candidates) {
        if (scene->isEntityCreated(entity) && scene->findComponent<ParticleSystemComponent>(entity) &&
            std::find(result.begin(), result.end(), entity) == result.end()) {
            result.push_back(entity);
        }
    }
    return result;
}

void ParticleEffectOverlay::requestBake(uint32_t sceneId, Entity entity) {
    bakeRequest() = BakeRequest{sceneId, entity, true};
}

void ParticleEffectOverlay::updateBake(Project* project) {
    if (!bake.running || !bake.capture) return;
    auto* capture = static_cast<pixelart::ParticleCapture*>(bake.capture.get());
    if (!capture->step()) return;
    bake.running = false;
    if (capture->failed()) {
        Out::error("Bake to Sprite Sheet: %s", capture->error().c_str());
        bake.capture.reset();
        return;
    }
    std::filesystem::path png = project->getProjectPath() / FileUtils::pathFromUtf8(bake.path);
    std::filesystem::path json = png;
    json.replace_extension(".json");
    const pixelart::PixelImage strip = pixelart::particleSpriteStrip(capture->frames(), capture->width(), capture->height(), bake.scale, bake.padding);
    std::vector<WriteFilesCmd::Item> items;
    items.push_back({png, strip.encodePng()});
    items.push_back({json, pixelart::particleSpriteStripJson(FileUtils::pathToUtf8(png.filename()), capture->frames(), capture->width(),
                                                              capture->height(), bake.scale, bake.padding)});
    std::string error;
    if (!ProjectFileWrites::writeUndoable(project, items, error, nullptr, nullptr, {}, true, "Bake to Sprite Sheet"))
        Out::error("Bake to Sprite Sheet: %s", error.c_str());
    else
        Out::info("Baked %s and its .json (%d frames): Slice... > From JSON imports it", bake.path, (int)capture->frames().size());
    bake.capture.reset();
}

void ParticleEffectOverlay::drawBakeDialog(Project* project, SceneProject* sceneProject) {
    BakeRequest& request = bakeRequest();
    if (request.pending && request.sceneId == sceneProject->id) {
        request.pending = false;
        bake.sceneId = request.sceneId;
        bake.entity = request.entity;
        bake.open = true;
        // one loop, or the whole one-shot
        if (const particles::ParticleEffectDocument* d = ParticleSubsystem::getDocument(sceneProject->scene->getComponent<ParticleSystemComponent>(bake.entity))) {
            float lo = 0.0f, hi = 0.0f;
            d->main.startLifetime.range(lo, hi);
            bake.start = 0.0f;
            bake.end = d->looping ? d->duration : d->duration + hi;
        }
        std::snprintf(bake.path, sizeof(bake.path), "%s", (sceneProject->scene->getEntityName(bake.entity) + " sheet.png").c_str());
    }
    if (!bake.open || bake.sceneId != sceneProject->id) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(320.0f), 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("Bake to Sprite Sheet", &bake.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SliderInt("Frames", &bake.frames, 1, 64);
        ImGui::DragFloat("From (s)", &bake.start, 0.01f, 0.0f, 1.0e4f, "%.2f");
        ImGui::DragFloat("To (s)", &bake.end, 0.01f, 0.0f, 1.0e4f, "%.2f");
        ImGui::SliderInt("Scale", &bake.scale, 1, 8);
        ImGui::SliderInt("Padding", &bake.padding, 0, 16);
        ImGui::InputText("File (.png)", bake.path, sizeof(bake.path));
        ImGui::TextDisabled("Transparent background; writes the PNG strip and an Aseprite JSON beside it");
        ImGui::BeginDisabled(bake.running || bake.end < bake.start || !sceneProject->scene->isEntityCreated(bake.entity));
        if (ImGui::Button(bake.running ? "Baking..." : "Bake")) {
            const particles::ParticleEffectDocument* d = ParticleSubsystem::getDocument(sceneProject->scene->getComponent<ParticleSystemComponent>(bake.entity));
            if (d) {
                pixelart::ParticleCaptureSettings settings;
                settings.document = *d;
                const uint32_t seed = d->seed ? d->seed : sceneProject->scene->getComponent<ParticleSystemComponent>(bake.entity).seed;
                settings.seed = seed ? seed : 1u;
                settings.pixelsPerUnit = std::max(project->getPixelsPerUnit(), 1);
                for (int i = 0; i < bake.frames; i++) {
                    const double span = d->looping ? (bake.end - bake.start) / bake.frames : (bake.frames > 1 ? (bake.end - bake.start) / (bake.frames - 1) : 0.0);
                    settings.times.push_back(bake.start + span * i);
                }
                auto capture = std::make_shared<pixelart::ParticleCapture>();
                std::string error;
                if (capture->begin(settings, error)) {
                    bake.capture = capture;
                    bake.running = true;
                } else {
                    Out::error("Bake to Sprite Sheet: %s", error.c_str());
                }
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

// R34: the base-pixel grid (1 / PPU) around each previewed emitter, in an orthographic Scene view
// zoomed in 4x or more
void ParticleEffectOverlay::drawPixelGrid(Project* project, SceneProject* sceneProject, const std::vector<Entity>& systems, const ImVec2& canvasMin,
                                          const ImVec2& canvasSize) {
    if (!gizmos().pixelGrid || !sceneProject->sceneRender) return;
    Camera* camera = sceneProject->sceneRender->getCamera();
    if (!camera) return;
    const CameraComponent& view = camera->getComponent<CameraComponent>();
    if (view.type == CameraType::CAMERA_PERSPECTIVE) return;
    const float ppu = (float)std::max(project->getPixelsPerUnit(), 1);
    const float unitsHigh = std::fabs(view.topClip - view.bottomClip);
    if (unitsHigh <= 0.0f) return;
    const float zoom = canvasSize.y / unitsHigh / ppu;   // screen pixels per base pixel
    if (zoom < 4.0f) return;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto toScreen = [&](const Vector3& world) {
        const Vector4 clip = view.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const float w = std::fabs(clip.w) > 1e-6f ? clip.w : 1.0f;
        return ImVec2(canvasMin.x + (clip.x / w * 0.5f + 0.5f) * canvasSize.x, canvasMin.y + (0.5f - clip.y / w * 0.5f) * canvasSize.y);
    };
    const ImU32 colour = IM_COL32(255, 255, 255, 40);
    for (Entity entity : systems) {
        const Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
        if (!transform) continue;
        const float cx = std::floor(transform->worldPosition.x * ppu), cy = std::floor(transform->worldPosition.y * ppu);
        const float z = transform->worldPosition.z;
        const int reach = 24;
        for (int k = -reach; k <= reach; k++) {
            drawList->AddLine(toScreen(Vector3((cx + k) / ppu, (cy - reach) / ppu, z)), toScreen(Vector3((cx + k) / ppu, (cy + reach) / ppu, z)), colour);
            drawList->AddLine(toScreen(Vector3((cx - reach) / ppu, (cy + k) / ppu, z)), toScreen(Vector3((cx + reach) / ppu, (cy + k) / ppu, z)), colour);
        }
    }
}

void ParticleEffectOverlay::update(Project* project, SceneProject* sceneProject, double dt) {
    updateBake(project);
    if (!sceneProject || !sceneProject->scene) return;
    State& state = states[sceneProject->id];
    Scene* scene = sceneProject->scene;
    if (!editable(sceneProject)) {
        state.previewed.clear();
        state.seeking = false;
        return;
    }
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    const std::vector<Entity> now = targets(project, sceneProject);

    // a system that left the preview stops and clears (Unity's deselected system)
    for (Entity entity : state.previewed) {
        if (std::find(now.begin(), now.end(), entity) == now.end() && scene->isEntityCreated(entity) &&
            scene->findComponent<ParticleSystemComponent>(entity)) {
            particles->stop(entity, true);
            sceneProject->needUpdateRender = true;
        }
    }
    state.previewed = now;
    if (now.empty()) {
        state.seeking = false;
        return;
    }

    if (state.seeking) {
        bool done = true;
        const double budget = kSeekBudgetMs / (double)now.size();
        for (Entity entity : now) done = particles->previewUpdate(entity, budget) && done;
        state.seeking = !done;
        sceneProject->needUpdateRender = true;
        return;
    }
    if (!state.playing) return;
    const double step = std::clamp(dt, 0.0, 0.1) * state.speed;
    for (Entity entity : now) particles->previewAdvance(entity, step);
    sceneProject->needUpdateRender = true;
}

void ParticleEffectOverlay::clear(uint32_t sceneId) {
    states.erase(sceneId);
}

void ParticleEffectOverlay::play(uint32_t sceneId) {
    State& state = states[sceneId];
    state.playing = true;
    state.seeking = false;
}

void ParticleEffectOverlay::pause(uint32_t sceneId) {
    states[sceneId].playing = false;
}

void ParticleEffectOverlay::addTarget(uint32_t sceneId, Entity entity) {
    std::vector<Entity>& extra = states[sceneId].extra;
    if (std::find(extra.begin(), extra.end(), entity) == extra.end()) extra.push_back(entity);
}

void ParticleEffectOverlay::setSpeed(uint32_t sceneId, float speed) {
    states[sceneId].speed = std::max(0.0f, speed);
}

bool ParticleEffectOverlay::isPlaying(uint32_t sceneId) const {
    auto it = states.find(sceneId);
    return it == states.end() || it->second.playing;
}

bool ParticleEffectOverlay::isSeeking(uint32_t sceneId) const {
    auto it = states.find(sceneId);
    return it != states.end() && it->second.seeking;
}

void ParticleEffectOverlay::restart(Project* project, SceneProject* sceneProject) {
    if (!editable(sceneProject)) return;
    std::shared_ptr<ParticleSubsystem> particles = sceneProject->scene->getSystem<ParticleSubsystem>();
    for (Entity entity : targets(project, sceneProject)) particles->restart(entity);
    play(sceneProject->id);
    sceneProject->needUpdateRender = true;
}

void ParticleEffectOverlay::stop(Project* project, SceneProject* sceneProject) {
    if (!editable(sceneProject)) return;
    std::shared_ptr<ParticleSubsystem> particles = sceneProject->scene->getSystem<ParticleSubsystem>();
    for (Entity entity : targets(project, sceneProject)) particles->stop(entity, true);
    State& state = states[sceneProject->id];
    state.playing = false;
    state.seeking = false;
    sceneProject->needUpdateRender = true;
}

void ParticleEffectOverlay::seek(Project* project, SceneProject* sceneProject, double time) {
    if (!editable(sceneProject)) return;
    std::shared_ptr<ParticleSubsystem> particles = sceneProject->scene->getSystem<ParticleSubsystem>();
    for (Entity entity : targets(project, sceneProject)) particles->previewSeek(entity, time);
    State& state = states[sceneProject->id];
    // a scrub pauses the preview where it lands (Unity)
    state.playing = false;
    state.seeking = true;
    sceneProject->needUpdateRender = true;
}

void ParticleEffectOverlay::draw(Project* project, SceneProject* sceneProject, const ImVec2& canvasMin, const ImVec2& canvasSize) {
    if (!editable(sceneProject)) return;
    const std::vector<Entity> now = targets(project, sceneProject);
    if (now.empty() && !states[sceneProject->id].simulateAll) return;
    State& state = states[sceneProject->id];
    Scene* scene = sceneProject->scene;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();

    uint32_t live = 0;
    double simulateMs = 0.0, renderMs = 0.0, time = 0.0;
    double range = 1.0;
    bool anyPixel = false, anyAnalytic = false, allAnalytic = !now.empty();
    for (size_t i = 0; i < now.size(); i++) {
        const ParticleSystemStats stats = particles->getStats(now[i]);
        live += stats.live;
        anyPixel = anyPixel || stats.pixel;
        anyAnalytic = anyAnalytic || stats.analytic;
        allAnalytic = allAnalytic && stats.analytic;
        simulateMs += stats.simulateMicros / 1000.0;
        renderMs += stats.renderMicros / 1000.0;
        if (i == 0) time = stats.time;
        if (const particles::ParticleEffectDocument* document = ParticleSubsystem::getDocument(scene->getComponent<ParticleSystemComponent>(now[i]))) {
            // looping effects scrub over a minute, one-shots over four durations
            const double span = document->looping ? 60.0 : std::max(1.0, (double)document->duration * 4.0);
            float delayLo = 0.0f, delayHi = 0.0f;
            document->startDelay.range(delayLo, delayHi);
            range = std::max(range, span + (double)std::max(delayHi, 0.0f));
        }
    }

    const float width = Theme::dpi(250.0f);
    const float margin = Theme::dpi(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + canvasSize.x - width - margin, canvasMin.y + margin + ImGui::GetFrameHeightWithSpacing()));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    const std::string id = "##particle_effect_overlay" + std::to_string(sceneProject->id);
    if (ImGui::BeginChild(id.c_str(), ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
        ImGui::TextUnformatted(ICON_FA_FIRE " Particle Effect");
        ImGui::Separator();

        if (ImGui::Button(state.playing ? ICON_FA_PAUSE " Pause" : ICON_FA_PLAY " Play")) {
            if (state.playing) pause(sceneProject->id);
            else play(sceneProject->id);
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_RIGHT " Restart")) restart(project, sceneProject);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP " Stop")) stop(project, sceneProject);

        const float labelWidth = Theme::dpi(100.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Playback Speed");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float speed = state.speed;
        if (ImGui::DragFloat("##speed", &speed, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) setSpeed(sceneProject->id, speed);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(allAnalytic ? "Analytic: exact" : "Playback Time");
        if (allAnalytic && ImGui::IsItemHovered()) ImGui::SetTooltip("An Analytic effect is computed at the time: the scrub is exact and instant");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float scrub = (float)std::min(time, range);
        if (ImGui::SliderFloat("##time", &scrub, 0.0f, (float)range, "%.2f s")) seek(project, sceneProject, scrub);

        ImGui::Text("Particles  %u%s%s", live, anyPixel ? "  [pixel]" : "", anyAnalytic ? "  [analytic]" : "");
        ImGui::Text("Simulate  %.2f ms   Render  %.2f ms", simulateMs, renderMs);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Simulate Layers");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* layers[] = {"Selection Only", "All"};
        int layer = state.simulateAll ? 1 : 0;
        if (ImGui::Combo("##layers", &layer, layers, 2)) state.simulateAll = layer == 1;

        Gizmos& g = gizmos();
        bool gizmoChanged = ImGui::Checkbox("Show Bounds", &g.bounds);
        gizmoChanged = ImGui::Checkbox("Show Colliders", &g.colliders) || gizmoChanged;
        gizmoChanged = ImGui::Checkbox("Show Force Fields", &g.forceFields) || gizmoChanged;
        gizmoChanged = ImGui::Checkbox("Pixel Grid", &g.pixelGrid) || gizmoChanged;
        ImGui::SetItemTooltip("The base-pixel grid (1 / Pixels Per Unit) around the emitters, when the Scene view is zoomed in 4x or more");
        if (gizmoChanged) sceneProject->needUpdateRender = true;
        if (ImGui::Button(ICON_FA_ELLIPSIS "##particle_overlay_more")) ImGui::OpenPopup("##particle_overlay_menu");
        if (ImGui::BeginPopup("##particle_overlay_menu")) {
            if (ImGui::MenuItem("Bake to Sprite Sheet...", nullptr, false, !now.empty())) requestBake(sceneProject->id, now.front());
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    drawPixelGrid(project, sceneProject, now, canvasMin, canvasSize);
    drawBakeDialog(project, sceneProject);
}
