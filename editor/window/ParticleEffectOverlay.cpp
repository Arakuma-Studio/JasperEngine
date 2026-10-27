// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectOverlay.h"

#include "Project.h"
#include "Theme.h"
#include "component/ParticleSystemComponent.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/ParticleSubsystem.h"

#include <algorithm>
#include <cfloat>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // a seek spreads its ticks over frames at most this long (S20 R29)
    constexpr double kSeekBudgetMs = 4.0;

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

void ParticleEffectOverlay::update(Project* project, SceneProject* sceneProject, double dt) {
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
    for (size_t i = 0; i < now.size(); i++) {
        const ParticleSystemStats stats = particles->getStats(now[i]);
        live += stats.live;
        simulateMs += stats.simulateMicros / 1000.0;
        renderMs += stats.renderMicros / 1000.0;
        if (i == 0) time = stats.time;
        if (const particles::ParticleEffectDocument* document = ParticleSubsystem::getDocument(scene->getComponent<ParticleSystemComponent>(now[i]))) {
            // looping effects scrub over a minute, one-shots over four durations
            const double span = document->looping ? 60.0 : std::max(1.0, (double)document->duration * 4.0);
            range = std::max(range, span + document->startDelay);
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
        ImGui::TextUnformatted("Playback Time");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float scrub = (float)std::min(time, range);
        if (ImGui::SliderFloat("##time", &scrub, 0.0f, (float)range, "%.2f s")) seek(project, sceneProject, scrub);

        ImGui::Text("Particles  %u", live);
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
        if (gizmoChanged) sceneProject->needUpdateRender = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
