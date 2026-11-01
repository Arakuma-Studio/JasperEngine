// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's Particle Effect overlay (SceneWindow::ParticleOverlay, particles spec 6.8).

#include "SceneWindow.h"

#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "component/ActionComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "particles/ParticleSimulation.h"
#include "subsystem/ActionSystem.h"

#include <algorithm>

using namespace jasperengine;

std::vector<Entity> editor::SceneWindow::particleOverlaySystems(SceneProject* sceneProject, Entity& root) const {
    root = NULL_ENTITY;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (!scene) return {};
    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.empty()) return {};
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    // the selection's systems and its children's; else the nearest parent that is one
    Entity entity = selected.front();
    std::vector<Entity> systems = actions->particleSystemsOf(entity, true);
    while (systems.empty()) {
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || transform->parent == NULL_ENTITY) return {};
        entity = transform->parent;
        systems = actions->particleSystemsOf(entity, false);
    }
    root = entity;
    if (particleOverlay.all) {
        // every version 2 system of the scene, in hierarchy order
        std::vector<Entity> everything;
        for (Entity e : scene->getEntityList()) {
            const Transform* transform = scene->findComponent<Transform>(e);
            if (transform && transform->parent != NULL_ENTITY) continue;
            for (Entity system : actions->particleSystemsOf(e, true)) everything.push_back(system);
        }
        if (!everything.empty()) systems = everything;
    }
    return systems;
}

void editor::SceneWindow::startParticleOverlay(SceneProject* sceneProject) {
    Scene* scene = sceneProject->scene;
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    Entity root = NULL_ENTITY;
    particleOverlay.systems = particleOverlaySystems(sceneProject, root);
    particleOverlay.root = root;
    particleOverlay.sceneId = sceneProject->id;
    particleOverlay.actionStates.clear();
    for (Entity system : particleOverlay.systems) {
        if (const ActionComponent* action = scene->findComponent<ActionComponent>(system)) {
            particleOverlay.actionStates.push_back({system, (int)action->state});
        }
        ParticleSystemComponent& ps = scene->getComponent<ParticleSystemComponent>(system);
        actions->particleSystemReset(system);
        // a fixed seed: each restart shows the same particles
        ps.state.fixedSeed = true;
        ps.state.fixedSeedValue = ps.main.autoRandomSeed ? 0x5EEDu : ps.main.randomSeed;
        actions->particleSystemPlay(system, false);
    }
    particleOverlay.active = !particleOverlay.systems.empty();
    particleOverlay.playing = particleOverlay.active;
    particleOverlay.time = 0.0f;
    sceneProject->needUpdateRender = true;
}

void editor::SceneWindow::stopParticleOverlay(SceneProject* sceneProject) {
    if (!particleOverlay.active) return;
    Scene* scene = sceneProject && sceneProject->id == particleOverlay.sceneId ? sceneProject->scene : nullptr;
    if (!scene) {
        SceneProject* owner = project->getScene(particleOverlay.sceneId);
        scene = owner ? owner->scene : nullptr;
        sceneProject = owner;
    }
    if (scene) {
        ActionSystem* actions = scene->getSystem<ActionSystem>().get();
        for (Entity system : particleOverlay.systems) {
            if (!scene->isEntityCreated(system) || !scene->findComponent<ParticleSystemComponent>(system)) continue;
            actions->particleSystemReset(system);
            scene->getComponent<ParticleSystemComponent>(system).state.fixedSeed = false;
        }
        for (const auto& [system, state] : particleOverlay.actionStates) {
            if (!scene->isEntityCreated(system)) continue;
            if (ActionComponent* action = scene->findComponent<ActionComponent>(system)) {
                action->state = (ActionState)state;
                action->startTrigger = false;
                action->stopTrigger = false;
                action->pauseTrigger = false;
                action->timecount = 0;
            }
        }
        if (sceneProject) sceneProject->needUpdateRender = true;
    }
    particleOverlay.active = false;
    particleOverlay.playing = false;
    particleOverlay.systems.clear();
    particleOverlay.actionStates.clear();
    particleOverlay.time = 0.0f;
}

void editor::SceneWindow::drawParticleOverlay(SceneProject* sceneProject, const ImVec2& canvasMin, const ImVec2& canvasSize) {
    Scene* scene = sceneProject->scene;
    const bool editMode = sceneProject->playState == ScenePlayState::STOPPED;
    Entity root = NULL_ENTITY;
    const std::vector<Entity> systems = editMode ? particleOverlaySystems(sceneProject, root) : std::vector<Entity>();

    // the preview ends when its systems are no longer selected, or the scene plays
    if (particleOverlay.active && particleOverlay.sceneId == sceneProject->id && (!editMode || root != particleOverlay.root)) {
        stopParticleOverlay(sceneProject);
    }
    if (systems.empty()) return;

    const bool previewing = particleOverlay.active && particleOverlay.sceneId == sceneProject->id;
    if (previewing && particleOverlay.playing) {
        const float dt = ImGui::GetIO().DeltaTime * particleOverlay.speed;
        std::vector<Entity> alive;
        for (Entity system : particleOverlay.systems) {
            if (scene->isEntityCreated(system) && scene->findComponent<ParticleSystemComponent>(system)) alive.push_back(system);
        }
        scene->getSystem<ActionSystem>()->updateParticlePreview(dt, alive);
        particleOverlay.time += dt;
        sceneProject->needUpdateRender = true;
    }

    size_t count = 0;
    for (Entity system : systems) {
        if (const ParticleSystemComponent* ps = scene->findComponent<ParticleSystemComponent>(system)) count += particles::particleCount(*ps);
    }

    // bottom right of the view, Unity's "Particle Effect" panel
    const float width = Theme::dpi(250.0f);
    const float height = ImGui::GetFrameHeightWithSpacing() * 4.0f + ImGui::GetTextLineHeightWithSpacing() + Theme::dpi(8.0f);
    const float margin = Theme::dpi(8.0f);
    if (canvasSize.x < width + 2 * margin || canvasSize.y < height + 2 * margin) return;
    ImGui::SetCursorScreenPos(ImVec2(canvasMin.x + canvasSize.x - width - margin, canvasMin.y + canvasSize.y - height - margin));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 0.88f));
    const std::string id = "##ParticleEffect" + std::to_string(sceneProject->id);
    if (ImGui::BeginChild(id.c_str(), ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextUnformatted("Particle Effect");
        const bool playing = previewing && particleOverlay.playing;
        if (ImGui::Button(playing ? ICON_FA_PAUSE " Pause" : ICON_FA_PLAY " Play")) {
            if (!previewing) {
                startParticleOverlay(sceneProject);
            } else {
                particleOverlay.playing = !particleOverlay.playing;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Restart")) {
            stopParticleOverlay(sceneProject);
            startParticleOverlay(sceneProject);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!previewing);
        if (ImGui::Button(ICON_FA_STOP " Stop")) {
            stopParticleOverlay(sceneProject);
        }
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::SliderFloat("Playback Speed", &particleOverlay.speed, 0.0f, 4.0f, "%.2f");
        ImGui::Text("Playback Time  %.2f", previewing ? particleOverlay.time : 0.0f);
        ImGui::Text("Particles      %zu", count);
        bool all = particleOverlay.all;
        if (ImGui::Checkbox("Simulate all systems", &all)) {
            stopParticleOverlay(sceneProject);
            particleOverlay.all = all;
        }
        ImGui::SetItemTooltip("Simulate Layers: the selected systems and their children, or every Particle System of the scene");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
