// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectWindow.h"

#include "Backend.h"
#include "ParticleModuleRows.h"
#include "Project.h"
#include "Theme.h"
#include "Widgets.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "component/ParticleSystemComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleModules.h"
#include "util/ParticleEffectEdit.h"
#include "util/UIProbe.h"
#include "widget/ColorField.h"
#include "widget/CurveField.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    // a re-simulation spends at most this much of a frame (S20 R29, S91 R17)
    constexpr double kResimulateBudgetMs = 4.0;
    // a one-shot effect plays on this long after its last particle before the clock loops to 0
    constexpr double kOneShotRest = 0.5;
    // the Overdraw view: each particle adds this heat (linear), as the Scene view's Overdraw does
    const Vector4 kOverdrawHeat(0.10f, 0.035f, 0.01f, 1.0f);

    ParticleEffectWindow::ChecksPanel& registeredChecksPanel() {
        static ParticleEffectWindow::ChecksPanel panel;
        return panel;
    }

    uint64_t mixHash(uint64_t h, uint64_t value) {
        return (h ^ value) * 1099511628211ull;
    }

    uint64_t textHash(const std::string& text) {
        return (uint64_t)std::hash<std::string>()(text);
    }

    // the Overdraw view's copy: every particle a constant additive heat, untextured, unlit (the
    // simulation keeps its random streams: they are per module, and only colour modules go)
    void overdrawCopy(ParticleEffectDocument& d) {
        d.renderer.blend = ParticleBlend::Additive;
        d.renderer.lighting = LightingMode::Unlit;
        d.renderer.texture.clear();
        d.renderer.textureGuid.clear();
        d.renderer.untexturedShape = UntexturedShape::Square;
        d.renderer.emissionIntensity = 0.0f;
        d.main.startColor = MinMaxGradient();
        d.main.startColor.color = kOverdrawHeat;
        d.colorOverLifetime.enabled = false;
        d.colorBySpeed.enabled = false;
        d.textureSheetAnimation.enabled = false;
    }

    // the eye's copy: simulated as before (its children's events too), nothing drawn
    void hiddenCopy(ParticleEffectDocument& d) {
        d.renderer.renderMode = RenderMode::None;
        d.trails.enabled = false;
    }

    void drawChecker(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
        const float cell = Theme::dpi(12.0f);
        drawList->AddRectFilled(min, max, IM_COL32(150, 150, 150, 255));
        for (float y = min.y; y < max.y; y += cell) {
            for (float x = min.x + (std::fmod((y - min.y) / cell, 2.0f) < 1.0f ? 0.0f : cell); x < max.x; x += cell * 2.0f) {
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)), IM_COL32(105, 105, 105, 255));
            }
        }
    }

    const char* backgroundLabel(ParticlePreviewRender::Background background) {
        switch (background) {
            case ParticlePreviewRender::Background::Dark: return "Dark";
            case ParticlePreviewRender::Background::Light: return "Light";
            case ParticlePreviewRender::Background::Checker: return "Checker";
            case ParticlePreviewRender::Background::SceneSky: return "Scene Sky";
            case ParticlePreviewRender::Background::Black: return "Black";
        }
        return "Dark";
    }

}

void ParticleEffectWindow::setChecksPanel(ChecksPanel panel) {
    registeredChecksPanel() = std::move(panel);
}

ParticleEffectWindow::ParticleEffectWindow(Project* project): project(project) {
}

ParticleEffectWindow::~ParticleEffectWindow() = default;

void ParticleEffectWindow::setOpen(bool open) {
    windowOpen = open;
}

void ParticleEffectWindow::requestFocus() {
    windowOpen = true;
    focusRequested = true;
}

void ParticleEffectWindow::openEffect(const std::string& assetPath) {
    if (effectPath != assetPath || subtreeRoot != NULL_ENTITY) {
        close();
        effectPath = assetPath;
        // the effect's own pinned seed, else 1
        const EffectAsset& asset = ParticleEffectLibrary::get(assetPath);
        seed = asset.document && asset.document->seed != 0 ? asset.document->seed : 1u;
        stage.setSeed(seed);
        collectNodes();
    }
    requestFocus();
}

void ParticleEffectWindow::openSubtree(uint32_t sceneId, Entity root) {
    if (subtreeScene != sceneId || subtreeRoot != root || !effectPath.empty()) {
        close();
        subtreeScene = sceneId;
        subtreeRoot = root;
        seed = 1;
        if (SceneProject* sceneProject = project->getScene(sceneId)) {
            if (const ParticleSystemComponent* particles = sceneProject->scene->findComponent<ParticleSystemComponent>(root)) {
                if (particles->seed != 0) seed = particles->seed;
            }
        }
        stage.setSeed(seed);
        collectNodes();
    }
    requestFocus();
}

void ParticleEffectWindow::close() {
    pushEdit(nullptr, nullptr, true);
    stage.clear();
    effectPath.clear();
    subtreeScene = 0;
    subtreeRoot = NULL_ENTITY;
    nodes.clear();
    rows.clear();
    hidden.clear();
    exposedOverrides.clear();
    selected = 0;
    staged = false;
    stagedSignature = 0;
    clock = 0.0;
    playing = true;
    scrubbing = false;
}

void ParticleEffectWindow::resetForProjectSwitch() {
    // the histories go with the project: a merging step is not touched again
    editCommand = nullptr;
    editHistory = nullptr;
    close();
    windowOpen = false;
}

void ParticleEffectWindow::clearSceneState(uint32_t sceneId) {
    if (subtreeRoot == NULL_ENTITY || subtreeScene != sceneId) return;
    // the scene's history goes with it: its merging step is not touched again
    if (editHistory && editHistory != project->getProjectCommandHistory()) {
        editCommand = nullptr;
        editHistory = nullptr;
    }
    close();
}

void ParticleEffectWindow::stopPreview() {
    // the stage is rebuilt (at the same time) when the window shows again
    stage.clear();
    staged = false;
}

// ---------------------------------------------------------------- the nodes

std::string ParticleEffectWindow::nodeKey(int node) const {
    if (node < 0 || node >= (int)nodes.size()) return std::string();
    const Node& n = nodes[(size_t)node];
    if (n.parent < 0) return (n.effect.empty() ? "entity:" + std::to_string(n.entity) : n.effect) + "#" + std::to_string(node);
    return nodeKey(n.parent) + "/" + std::to_string(n.subEmitter);
}

std::string ParticleEffectWindow::stageKey(int node) const {
    return std::string(STAGE_KEY_PREFIX) + std::to_string(node) + ".particles";
}

bool ParticleEffectWindow::collectNodes() {
    std::vector<ParticleTimingSystem> systems;
    std::vector<Entity> entities;
    if (!effectPath.empty()) {
        const EffectAsset& asset = ParticleEffectLibrary::get(effectPath);
        if (asset.document) {
            systems.push_back({std::filesystem::path(effectPath).stem().string(), effectPath, asset.document.get()});
            entities.push_back(NULL_ENTITY);
        }
    } else if (subtreeRoot != NULL_ENTITY) {
        SceneProject* sceneProject = project->getScene(subtreeScene);
        Scene* scene = sceneProject ? sceneProject->scene : nullptr;
        if (scene && scene->isEntityCreated(subtreeRoot) && scene->isComponentRegistered<ParticleSystemComponent>()) {
            // the systems at and below the root, in the Hierarchy's order
            for (Entity entity : sceneProject->entities) {
                const ParticleSystemComponent* particles = scene->findComponent<ParticleSystemComponent>(entity);
                if (!particles) continue;
                bool inside = false;
                for (Entity e = entity; e != NULL_ENTITY && !inside;) {
                    inside = e == subtreeRoot;
                    const Transform* transform = scene->findComponent<Transform>(e);
                    e = transform ? transform->parent : NULL_ENTITY;
                }
                if (!inside) continue;
                const ParticleEffectDocument* document = &particles->inlineEffect;
                if (!particles->effect.empty()) document = ParticleEffectLibrary::get(particles->effect.path).document.get();
                if (!document) continue;
                systems.push_back({scene->getEntityName(entity), particles->effect.empty() ? std::string() : particles->effect.path, document});
                entities.push_back(entity);
            }
        }
    }
    rows = ParticleEffectTiming::compute(systems);
    std::vector<Node> next;
    size_t system = 0;
    for (const ParticleTimingRow& row : rows) {
        Node node;
        node.name = row.name;
        node.effect = row.effect;
        node.parent = row.parent;
        node.subEmitter = row.subEmitter;
        node.depth = row.depth;
        node.missing = row.missing;
        if (row.depth == 0) {
            node.sceneId = subtreeRoot != NULL_ENTITY ? subtreeScene : 0;
            node.entity = system < entities.size() ? entities[system] : NULL_ENTITY;
            system++;
        }
        next.push_back(node);
    }
    nodes = std::move(next);
    if (selected >= (int)nodes.size()) selected = nodes.empty() ? 0 : (int)nodes.size() - 1;
    return !nodes.empty();
}

const ParticleEffectDocument* ParticleEffectWindow::sourceDocument(int node, ParticleEffectDocument& scratch) const {
    (void)scratch;
    if (node < 0 || node >= (int)nodes.size()) return nullptr;
    const Node& n = nodes[(size_t)node];
    if (n.missing) return nullptr;
    if (!n.effect.empty()) return ParticleEffectLibrary::get(n.effect).document.get();
    SceneProject* sceneProject = project->getScene(n.sceneId);
    if (!sceneProject || !sceneProject->scene) return nullptr;
    const ParticleSystemComponent* particles = sceneProject->scene->findComponent<ParticleSystemComponent>(n.entity);
    return particles ? &particles->inlineEffect : nullptr;
}

uint64_t ParticleEffectWindow::sourceSignature() const {
    uint64_t h = 14695981039346656037ull;
    h = mixHash(h, nodes.size());
    h = mixHash(h, overdraw ? 1u : 0u);
    for (size_t i = 0; i < nodes.size(); i++) {
        const Node& n = nodes[i];
        h = mixHash(h, textHash(n.effect));
        h = mixHash(h, isHidden((int)i) ? 7u : 3u);
        if (!n.effect.empty()) {
            h = mixHash(h, ParticleEffectLibrary::version(n.effect));
        } else {
            ParticleEffectDocument scratch;
            if (const ParticleEffectDocument* document = sourceDocument((int)i, scratch)) h = mixHash(h, textHash(ParticleEffectCodec::write(*document)));
        }
        // a subtree system's place under the root
        if (n.entity != NULL_ENTITY) {
            if (SceneProject* sceneProject = project->getScene(n.sceneId)) {
                if (const Transform* transform = sceneProject->scene->findComponent<Transform>(n.entity)) {
                    const float values[] = {transform->worldPosition.x, transform->worldPosition.y, transform->worldPosition.z,
                                            transform->worldRotation.w, transform->worldRotation.x, transform->worldRotation.y,
                                            transform->worldRotation.z, transform->worldScale.x, transform->worldScale.y, transform->worldScale.z};
                    for (float v : values) h = mixHash(h, std::hash<float>()(v));
                }
            }
        }
    }
    return h;
}

void ParticleEffectWindow::restage() {
    // the copies: each node's sub-emitters name its children's copies, the eye and Overdraw applied
    for (size_t i = 0; i < nodes.size(); i++) {
        ParticleEffectDocument scratch;
        const ParticleEffectDocument* source = sourceDocument((int)i, scratch);
        if (!source) continue;
        ParticleEffectDocument copy = *source;
        for (size_t c = 0; c < nodes.size(); c++) {
            const Node& child = nodes[c];
            if (child.parent != (int)i || child.subEmitter < 0 || child.subEmitter >= (int)copy.subEmitters.size()) continue;
            if (child.missing) continue;
            copy.subEmitters[(size_t)child.subEmitter].effect = stageKey((int)c);
            copy.subEmitters[(size_t)child.subEmitter].effectGuid.clear();
        }
        if (overdraw) overdrawCopy(copy);
        if (isHidden((int)i)) hiddenCopy(copy);
        ParticleEffectLibrary::set(stageKey((int)i), copy);
    }

    std::vector<ParticlePreviewRender::StageSystem> list;
    const Transform* rootTransform = nullptr;
    if (subtreeRoot != NULL_ENTITY) {
        if (SceneProject* sceneProject = project->getScene(subtreeScene)) rootTransform = sceneProject->scene->findComponent<Transform>(subtreeRoot);
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].depth != 0) continue;
        ParticlePreviewRender::StageSystem system;
        system.name = nodes[i].name;
        system.effect = stageKey((int)i);
        const Transform* transform = nullptr;
        if (nodes[i].entity != NULL_ENTITY) {
            if (SceneProject* sceneProject = project->getScene(nodes[i].sceneId)) transform = sceneProject->scene->findComponent<Transform>(nodes[i].entity);
        }
        if (transform && rootTransform) {
            // the place under the subtree's root (the root itself at the stage's centre)
            const Quaternion inverse = rootTransform->worldRotation.inverse();
            const Vector3 offset = inverse * (transform->worldPosition - rootTransform->worldPosition);
            const Vector3 rootScale = rootTransform->worldScale;
            auto divide = [](float a, float b) { return std::fabs(b) > 1e-6f ? a / b : a; };
            system.position = Vector3(divide(offset.x, rootScale.x), divide(offset.y, rootScale.y), divide(offset.z, rootScale.z));
            system.rotation = inverse * transform->worldRotation;
            system.scale = Vector3(divide(transform->worldScale.x, rootScale.x), divide(transform->worldScale.y, rootScale.y),
                                   divide(transform->worldScale.z, rootScale.z));
        }
        list.push_back(system);
    }
    const bool fresh = !stage.isStage() || !(stage.getStageSystems() == list);
    stage.buildStage(list, std::max(16u, (unsigned int)Theme::dpi(256.0f)), std::max(16u, (unsigned int)Theme::dpi(256.0f)));
    if (fresh) {
        stage.setSeed(seed);
        stage.setMovingEmitter(moving);
        SceneProject* selectedScene = project->getSelectedScene();
        stage.setBackground(overdraw ? Background::Black : background, selectedScene ? selectedScene->scene : nullptr);
        for (const ExposedRow& row : exposedOverrides) stage.setExposed(row.name, row.value);
    }
    stagedSignature = sourceSignature();
    staged = true;
    // the same time, the same seed (R17, R20): the checkpoints held the old effect's runs
    stage.invalidate();
    stage.seek(clock);
}

// ---------------------------------------------------------------- the clock

void ParticleEffectWindow::update(double dt) {
    if (!collectNodes()) return;
    if (!staged || sourceSignature() != stagedSignature) restage();
    if (stage.isSeeking()) {
        stage.updateSeek(kResimulateBudgetMs);
        return;
    }
    if (playing && !scrubbing) {
        stage.advance(std::clamp(dt, 0.0, 0.1) * speed);
        clock = stage.getTime();
        wrapLoop();
    }
}

void ParticleEffectWindow::wrapLoop() {
    double from = -1.0;
    if (loopRange) {
        if (clock >= loopEnd || clock < loopStart) from = loopStart;
    } else {
        // a one-shot effect plays again once its last particle is gone; a looping one plays on
        bool finite = !rows.empty();
        double end = 0.0;
        for (const ParticleTimingRow& row : rows) {
            if (std::isinf(row.end)) finite = false;
            else end = std::max(end, (double)row.end);
        }
        if (finite && clock >= end + kOneShotRest) from = 0.0;
    }
    if (from < 0.0) return;
    if (perLoopSeed) {
        seed = seed + 1 == 0 ? 1 : seed + 1;
        stage.setSeed(seed);
    }
    clock = from;
    stage.seek(clock);
}

void ParticleEffectWindow::play() {
    playing = true;
}

void ParticleEffectWindow::pause() {
    playing = false;
}

void ParticleEffectWindow::setSpeed(float value) {
    speed = std::clamp(value, 0.0f, 10.0f);
}

void ParticleEffectWindow::stepFrame() {
    playing = false;
    if (stage.isSeeking()) return;
    stage.step();
    clock = stage.getTime();
}

void ParticleEffectWindow::restart() {
    clock = loopRange ? loopStart : 0.0;
    stage.seek(clock);
}

void ParticleEffectWindow::seek(double time) {
    playing = false;
    clock = std::max(0.0, time);
    stage.seek(clock);
}

double ParticleEffectWindow::getTime() const {
    return clock;
}

void ParticleEffectWindow::setSeed(uint32_t value) {
    seed = value == 0 ? 1 : value;
    stage.setSeed(seed);
    stage.seek(clock);
}

void ParticleEffectWindow::reroll() {
    // a fixed sequence (LCG): the next seed is never the same one
    uint32_t next = seed * 1664525u + 1013904223u;
    if (next == 0 || next == seed) next = seed + 1;
    setSeed(next);
}

void ParticleEffectWindow::setLoopRange(bool enabled, float start, float end) {
    loopRange = enabled;
    loopStart = std::max(0.0f, std::min(start, end));
    loopEnd = std::max(loopStart + 0.01f, std::max(start, end));
    if (loopRange && (clock < loopStart || clock > loopEnd)) {
        clock = loopStart;
        stage.seek(clock);
    }
}

float ParticleEffectWindow::timelineSpan() const {
    float span = ParticleEffectTiming::span(rows, 1.0f);
    if (loopRange) span = std::max(span, loopEnd);
    return std::max(span, (float)clock);
}

// ---------------------------------------------------------------- the stage

void ParticleEffectWindow::setBackground(Background value) {
    background = value;
    SceneProject* selectedScene = project->getSelectedScene();
    if (!overdraw) stage.setBackground(value, selectedScene ? selectedScene->scene : nullptr);
}

void ParticleEffectWindow::setMovingEmitter(bool on) {
    moving = on;
    stage.setMovingEmitter(on);
    // the motion is part of the run: the same time again, with it
    stage.seek(clock);
}

void ParticleEffectWindow::setOverdraw(bool on) {
    if (overdraw == on) return;
    overdraw = on;
    SceneProject* selectedScene = project->getSelectedScene();
    stage.setBackground(on ? Background::Black : background, selectedScene ? selectedScene->scene : nullptr);
    // the copies follow on the next update (the signature has the switch)
}

void ParticleEffectWindow::setHidden(int node, bool hide) {
    const std::string key = nodeKey(node);
    if (key.empty()) return;
    if (hide) hidden.insert(key);
    else hidden.erase(key);
}

void ParticleEffectWindow::select(int node) {
    if (node < 0 || node >= (int)nodes.size()) return;
    if (node != selected) pushEdit(nullptr, nullptr, true);
    selected = node;
}

// ---------------------------------------------------------------- edits

bool ParticleEffectWindow::readSelected(ParticleEffectDocument& document, std::vector<Diagnostic>* diagnostics) const {
    if (selected < 0 || selected >= (int)nodes.size()) return false;
    const Node& n = nodes[(size_t)selected];
    if (!n.effect.empty()) return ParticleEffectEdit::readAsset(n.effect, document, diagnostics);
    ParticleEffectDocument scratch;
    const ParticleEffectDocument* source = sourceDocument(selected, scratch);
    if (!source) return false;
    document = *source;
    return true;
}

void ParticleEffectWindow::pushEdit(CommandHistory* history, Command* command, bool commit) {
    // a step of another history (another node) ends the merging one
    if (command && editHistory && history != editHistory) pushEdit(nullptr, nullptr, true);
    if (command && history) {
        history->addCommand(command);
        const UndoEntry* top = history->top();
        editCommand = (top && top->cmd.get() == command) ? command : nullptr;
        editHistory = editCommand ? history : nullptr;
    }
    if (commit) {
        if (editCommand && editHistory) {
            const UndoEntry* top = editHistory->top();
            if (top && top->cmd.get() == editCommand) top->cmd->setNoMerge();
        }
        editCommand = nullptr;
        editHistory = nullptr;
    }
}

bool ParticleEffectWindow::editSelected(const std::function<bool(ParticleEffectDocument&)>& edit, bool commit) {
    ParticleEffectDocument document;
    if (!readSelected(document)) return false;
    ParticleEffectDocument edited = document;
    if (!edit(edited) || ParticleEffectCodec::write(edited) == ParticleEffectCodec::write(document)) {
        if (commit) pushEdit(nullptr, nullptr, true);
        return false;
    }
    const Node& n = nodes[(size_t)selected];
    const auto assign = [&edited](ParticleEffectDocument& target) {
        target = edited;
        return true;
    };
    Command* command = nullptr;
    CommandHistory* history = nullptr;
    if (!n.effect.empty()) {
        command = ParticleEffectEdit::buildAsset(project, n.effect, assign);
        history = project->getProjectCommandHistory();
    } else if (n.entity != NULL_ENTITY) {
        command = ParticleEffectEdit::build(project, n.sceneId, {n.entity}, assign);
        history = CommandHandle::get(n.sceneId);
    }
    if (!command) return false;
    pushEdit(history, command, commit);
    return true;
}

// ---------------------------------------------------------------- exposed parameters

std::vector<ParticleEffectWindow::ExposedRow> ParticleEffectWindow::exposedRows() const {
    std::vector<ExposedRow> list;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].depth != 0) continue;
        ParticleEffectDocument scratch;
        const ParticleEffectDocument* document = sourceDocument((int)i, scratch);
        if (!document) continue;
        for (const ExposedParameter& parameter : document->exposed) {
            if (std::any_of(list.begin(), list.end(), [&](const ExposedRow& r) { return r.name == parameter.name; })) continue;
            ExposedRow row;
            row.name = parameter.name;
            row.type = parameter.type;
            ParticleModules::getExposed(*document, parameter.name, row.value);
            for (const ExposedRow& set : exposedOverrides) {
                if (set.name == parameter.name) row.value = set.value;
            }
            list.push_back(row);
        }
    }
    return list;
}

void ParticleEffectWindow::setExposedValue(const std::string& name, const Vector4& value) {
    auto it = std::find_if(exposedOverrides.begin(), exposedOverrides.end(), [&](const ExposedRow& r) { return r.name == name; });
    if (it == exposedOverrides.end()) {
        ExposedRow row;
        row.name = name;
        exposedOverrides.push_back(row);
        it = exposedOverrides.end() - 1;
    }
    it->value = value;
    stage.setExposed(name, value);
}

std::vector<std::string> ParticleEffectWindow::triggerNames() const {
    std::vector<std::string> names;
    for (size_t i = 0; i < nodes.size(); i++) {
        ParticleEffectDocument scratch;
        const ParticleEffectDocument* document = sourceDocument((int)i, scratch);
        if (!document) continue;
        for (const SubEmitterEntry& entry : document->subEmitters) {
            if (entry.trigger != SubEmitterTrigger::Trigger || entry.triggerName.empty()) continue;
            if (std::find(names.begin(), names.end(), entry.triggerName) == names.end()) names.push_back(entry.triggerName);
        }
    }
    return names;
}

void ParticleEffectWindow::fireTrigger(const std::string& name) {
    stage.sendTrigger(name);
}

ParticleEffectWindow::UiState ParticleEffectWindow::uiState() {
    UiState state;
    state.open = windowOpen;
    state.focused = focused;
    state.effect = effectPath;
    if (subtreeRoot != NULL_ENTITY) {
        if (SceneProject* sceneProject = project->getScene(subtreeScene)) state.subtreeRoot = sceneProject->scene->getEntityName(subtreeRoot);
    }
    state.time = clock;
    state.seed = seed;
    state.playing = playing;
    state.speed = speed;
    state.resimulating = stage.isSeeking();
    for (size_t i = 0; i < nodes.size(); i++) {
        state.nodes.push_back(nodes[i].name);
        if (isHidden((int)i)) state.hidden.push_back(nodes[i].name);
    }
    state.selected = nodes.empty() ? -1 : selected;
    if (stage.isStage()) {
        const ParticlePreviewRender::StageStats stats = stage.stats();
        state.live = stats.live;
        state.stateHash = stats.stateHash;
    }
    return state;
}

// ---------------------------------------------------------------- drawing

void ParticleEffectWindow::show() {
    if (!windowOpen) {
        focused = false;
        return;
    }
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(1100.0f), Theme::dpi(640.0f)), ImGuiCond_FirstUseEver);
    // a hidden tab draws nothing and spends nothing (D4): the stage waits where it is
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        focused = false;
        ImGui::End();
        return;
    }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (effectPath.empty() && subtreeRoot == NULL_ENTITY) {
        ImGui::TextDisabled("Double-click a .particles file in the Project window, or right-click a GameObject with Particle Systems");
        ImGui::TextDisabled("and choose Open in Particle Effect.");
        ImGui::End();
        return;
    }
    update(ImGui::GetIO().DeltaTime);
    if (nodes.empty()) {
        ImGui::TextDisabled("%s", effectPath.empty() ? "No Particle System at or below this GameObject." : "The effect cannot be read:");
        if (!effectPath.empty()) {
            const EffectAsset& asset = ParticleEffectLibrary::get(effectPath);
            particlerows::drawDiagnostics(asset.diagnostics);
        }
        ImGui::End();
        return;
    }

    // the Curve window opened from these rows marks the clock: the selected system's emitter time,
    // a fraction of its Duration (the axis of curves over the emitter's time)
    ui::CurveWindow& curveWindow = ui::CurveWindow::get();
    if (curveWindow.openedFrom(WINDOW_NAME) && selected >= 0 && selected < (int)rows.size()) {
        ParticleEffectDocument scratch;
        const ParticleEffectDocument* document = sourceDocument(selected, scratch);
        const double duration = document ? std::max(0.001, (double)document->duration) : 1.0;
        const double local = std::max(0.0, clock - (double)rows[(size_t)selected].start);
        const double fraction = rows[(size_t)selected].looping ? std::fmod(local, duration) / duration : std::min(local / duration, 1.0);
        curveWindow.setPlayhead((float)fraction);
    }

    drawToolbar();
    ImGui::Separator();
    const float inspectorWidth = std::max(Theme::dpi(320.0f), ImGui::GetContentRegionAvail().x * 0.36f);
    const float leftWidth = std::max(Theme::dpi(200.0f), ImGui::GetContentRegionAvail().x - inspectorWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginChild("##particle_effect_left", ImVec2(leftWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
        const float stageHeight = std::max(Theme::dpi(160.0f), ImGui::GetContentRegionAvail().y * 0.58f);
        drawStage(ImGui::GetContentRegionAvail().x, stageHeight);
        drawOutline();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##particle_effect_right", ImVec2(0.0f, 0.0f))) {
        drawInspector();
        drawExposed();
        // the slot of the Filmstrip and Checks panel (R24): another branch registers it
        if (const ChecksPanel& panel = registeredChecksPanel()) {
            if (ImGui::CollapsingHeader("Filmstrip and Checks", ImGuiTreeNodeFlags_DefaultOpen)) panel(*this);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void ParticleEffectWindow::drawToolbar() {
    const bool wasPlaying = playing;
    if (ImGui::Button(wasPlaying ? ICON_FA_PAUSE "##pe_play" : ICON_FA_PLAY "##pe_play")) togglePlay();
    UIProbe::item("particle_effect.play");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wasPlaying ? "Pause" : "Play");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##pe_step")) stepFrame();
    UIProbe::item("particle_effect.step");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step: one tick, paused");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##pe_restart")) restart();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restart: back to the start, the same seed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(70.0f));
    float speedValue = speed;
    if (ImGui::DragFloat("##pe_speed", &speedValue, 0.01f, 0.0f, 10.0f, "x%.2f", ImGuiSliderFlags_AlwaysClamp)) setSpeed(speedValue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback Speed");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2f s", clock);
    UIProbe::item("particle_effect.time");

    ImGui::SameLine();
    ImGui::TextUnformatted("Seed");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(90.0f));
    uint32_t seedValue = seed;
    const uint32_t one = 1, most = 0xffffffffu;
    if (ImGui::DragScalar("##pe_seed", ImGuiDataType_U32, &seedValue, 0.5f, &one, &most, "%u") && seedValue != seed) setSeed(seedValue);
    UIProbe::item("particle_effect.seed");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_DICE "##pe_reroll")) reroll();
    UIProbe::item("particle_effect.reroll");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reroll: a new seed, the same time");
    ImGui::SameLine();
    ImGui::Checkbox("Per-Loop Seed", &perLoopSeed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new seed each time the clock loops");

    bool range = loopRange;
    if (ImGui::Checkbox("Loop Range", &range)) setLoopRange(range, loopStart, loopRange ? loopEnd : timelineSpan());
    if (loopRange) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(160.0f));
        float start = loopStart, end = loopEnd;
        if (ImGui::DragFloatRange2("##pe_loop", &start, &end, 0.01f, 0.0f, 600.0f, "%.2f s", "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
            setLoopRange(true, start, end);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    if (ImGui::BeginCombo("##pe_background", backgroundLabel(background))) {
        for (Background option : {Background::Dark, Background::Light, Background::Checker, Background::SceneSky}) {
            if (ImGui::Selectable(backgroundLabel(option), option == background)) setBackground(option);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Background (Scene Sky: the open scene's)");
    ImGui::SameLine();
    bool movingValue = moving;
    if (ImGui::Checkbox("Moving Emitter", &movingValue)) setMovingEmitter(movingValue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The emitter circles the stage's centre (trails, rate over distance, inherited velocity)");
    ImGui::SameLine();
    bool overdrawValue = overdraw;
    if (ImGui::Checkbox("Overdraw", &overdrawValue)) setOverdraw(overdrawValue);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every particle adds the same heat: bright is where many layers cover a pixel");
}

void ParticleEffectWindow::drawStage(float width, float height) {
    const unsigned int w = (unsigned int)std::max(16.0f, width);
    const unsigned int h = (unsigned int)std::max(16.0f, height);
    if (stage.isStage()) stage.buildStage(stage.getStageSystems(), w, h);
    stage.render();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (background == Background::Checker && !overdraw) drawChecker(drawList, min, max);
    Texture texture = stage.getTexture();
    Widgets::image(Backend::getImGuiTexture(texture.getRender()), ImVec2(width, height), IM_COL32(30, 30, 33, 255));
    UIProbe::item("particle_effect.stage");
    // a drag orbits, the wheel zooms, a double-click frames again
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##pe_stage_input", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        stage.orbit(-delta.x * 0.4f, delta.y * 0.4f);
    }
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) stage.zoom(wheel > 0.0f ? 0.9f : 1.1f);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) stage.resetView();
    }

    // the stats over the image's top-left corner
    const ParticlePreviewRender::StageStats stats = stage.stats();
    char line[192];
    std::snprintf(line, sizeof(line), "Particles %u / %u   Simulate %.2f ms   Render %.2f ms", stats.live, stats.capacity, stats.simulateMs, stats.renderMs);
    const ImVec2 pad(Theme::dpi(6.0f), Theme::dpi(4.0f));
    drawList->AddText(ImVec2(min.x + pad.x + 1.0f, min.y + pad.y + 1.0f), IM_COL32(0, 0, 0, 200), line);
    drawList->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(235, 235, 235, 255), line);
    if (stage.isSeeking()) {
        // the re-simulation's progress (R17): the stage is not at its time yet
        const float progress = stage.seekProgress();
        const float barHeight = Theme::dpi(4.0f);
        drawList->AddRectFilled(ImVec2(min.x, max.y - barHeight), ImVec2(max.x, max.y), IM_COL32(0, 0, 0, 160));
        drawList->AddRectFilled(ImVec2(min.x, max.y - barHeight), ImVec2(min.x + (max.x - min.x) * progress, max.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
        std::snprintf(line, sizeof(line), "Re-simulating to %.2f s (%d%%)", stage.seekTargetTime(), (int)(progress * 100.0f));
        drawList->AddText(ImVec2(min.x + pad.x, max.y - barHeight - ImGui::GetTextLineHeight() - pad.y), IM_COL32(235, 235, 235, 255), line);
    }
    ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + ImGui::GetStyle().ItemSpacing.y));
}

void ParticleEffectWindow::drawRuler(float span) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetFrameHeight();
    ImGui::InvisibleButton("##pe_ruler", ImVec2(width, height));
    UIProbe::item("particle_effect.ruler");
    // the playhead follows the drag (a scrub: re-simulated there, paused)
    scrubbing = ImGui::IsItemActive();
    if (scrubbing) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - min.x) / width, 0.0f, 1.0f) * span;
        if (std::fabs(t - (float)clock) > 1e-4f) seek(t);
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, ImVec2(min.x + width, min.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
    // a tick a step that leaves room for its label
    float stepSeconds = 0.1f;
    for (float candidate : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f}) {
        stepSeconds = candidate;
        if (width / span * candidate >= Theme::dpi(48.0f)) break;
    }
    for (float t = 0.0f; t <= span + 1e-4f; t += stepSeconds) {
        const float x = min.x + t / span * width;
        drawList->AddLine(ImVec2(x, min.y + height * 0.6f), ImVec2(x, min.y + height), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        char label[16];
        std::snprintf(label, sizeof(label), stepSeconds < 1.0f ? "%.2f" : "%.0f", t);
        drawList->AddText(ImVec2(x + Theme::dpi(2.0f), min.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    if (loopRange) {
        const float a = min.x + loopStart / span * width, b = min.x + loopEnd / span * width;
        drawList->AddRectFilled(ImVec2(a, min.y + height - Theme::dpi(3.0f)), ImVec2(b, min.y + height), IM_COL32(90, 160, 240, 200));
    }
    const float x = min.x + std::min((float)clock / span, 1.0f) * width;
    drawList->AddTriangleFilled(ImVec2(x - Theme::dpi(5.0f), min.y), ImVec2(x + Theme::dpi(5.0f), min.y), ImVec2(x, min.y + Theme::dpi(7.0f)),
                                IM_COL32(240, 80, 70, 255));
}

void ParticleEffectWindow::drawTimingBar(int node, float x0, float x1, float y0, float y1, float span) {
    const ParticleTimingRow& row = rows[(size_t)node];
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto at = [&](float t) { return x0 + std::clamp(std::isinf(t) ? span : t / span, 0.0f, 1.0f) * (x1 - x0); };
    const float mid = (y0 + y1) * 0.5f;
    const float half = (y1 - y0) * 0.3f;
    if (row.missing) {
        drawList->AddText(ImVec2(x0 + Theme::dpi(4.0f), y0), IM_COL32(240, 110, 100, 255), "file missing");
        return;
    }
    const bool shown = !isHidden(node);
    const ImU32 emit = shown ? (row.eventDriven ? IM_COL32(210, 150, 60, 255) : IM_COL32(80, 150, 230, 255)) : IM_COL32(110, 110, 110, 255);
    const ImU32 life = shown ? IM_COL32(80, 150, 230, 90) : IM_COL32(110, 110, 110, 70);
    // the start delay's range, the emission, then the particles living on
    if (row.startMax > row.start) drawList->AddRectFilled(ImVec2(at(row.start), mid - half * 0.4f), ImVec2(at(row.startMax), mid + half * 0.4f), life);
    drawList->AddRectFilled(ImVec2(at(row.emitEnd), mid - half * 0.6f), ImVec2(at(row.end), mid + half * 0.6f), life);
    drawList->AddRectFilled(ImVec2(at(row.startMax), mid - half), ImVec2(std::max(at(row.emitEnd), at(row.startMax) + 2.0f), mid + half), emit);
    if (std::isinf(row.emitEnd)) {
        // looping: on for ever
        drawList->AddTriangleFilled(ImVec2(x1 - Theme::dpi(6.0f), mid - half), ImVec2(x1, mid), ImVec2(x1 - Theme::dpi(6.0f), mid + half), emit);
    }
    for (float t : row.bursts) {
        const float x = at(t);
        drawList->AddQuadFilled(ImVec2(x, mid - half - 2.0f), ImVec2(x + 3.0f, mid), ImVec2(x, mid + half + 2.0f), ImVec2(x - 3.0f, mid), IM_COL32(250, 250, 250, 230));
    }
    const float x = at((float)clock);
    drawList->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(240, 80, 70, 255), 1.5f);
}

void ParticleEffectWindow::drawOutline() {
    const float span = timelineSpan();
    if (!ImGui::BeginTable("##pe_outline", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##eye", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    ImGui::TableSetupColumn("Outline", ImGuiTableColumnFlags_WidthFixed, Theme::dpi(170.0f));
    ImGui::TableSetupColumn("Timing", ImGuiTableColumnFlags_WidthStretch);
    // the header row: the ruler with the playhead over the timing column
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Outline");
    ImGui::TableNextColumn();
    drawRuler(span);

    for (size_t i = 0; i < nodes.size(); i++) {
        const Node& n = nodes[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool hiddenNode = isHidden((int)i);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (hiddenNode) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        if (ImGui::SmallButton(hiddenNode ? ICON_FA_EYE_SLASH "##eye" : ICON_FA_EYE "##eye")) setHidden((int)i, !hiddenNode);
        if (hiddenNode) ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        UIProbe::item("particle_effect.eye." + std::to_string(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s in the preview only (the effect and its export are unchanged)", hiddenNode ? "Show" : "Hide");

        ImGui::TableNextColumn();
        ImGui::Indent(Theme::dpi(12.0f) * (float)n.depth);
        std::string label = n.depth == 0 ? std::string(ICON_FA_FIRE " ") + n.name : std::string(ICON_FA_ARROW_TURN_UP " ") + n.name;
        if (n.depth > 0) label += " (" + rows[i].trigger + ")";
        if (ImGui::Selectable((label + "##node").c_str(), selected == (int)i)) select((int)i);
        UIProbe::item("particle_effect.node." + std::to_string(i));
        if (ImGui::IsItemHovered()) {
            const ParticleTimingRow& row = rows[i];
            ImGui::SetTooltip("%s\nstart %.2f s, emits to %s, lifetime %.2f-%.2f s%s", n.effect.empty() ? "inline effect" : n.effect.c_str(), row.start,
                              std::isinf(row.emitEnd) ? "the end (looping)" : (std::to_string(row.emitEnd).substr(0, 5) + " s").c_str(),
                              row.lifetimeMin, row.lifetimeMax, row.eventDriven ? "\nemits on events: the times are a bound" : "");
        }
        ImGui::Unindent(Theme::dpi(12.0f) * (float)n.depth);

        ImGui::TableNextColumn();
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        const float cellWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float cellHeight = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##timing", ImVec2(cellWidth, cellHeight));
        // a click or a drag on a bar moves the playhead too
        if (ImGui::IsItemActive()) {
            const float t = std::clamp((ImGui::GetIO().MousePos.x - cell.x) / cellWidth, 0.0f, 1.0f) * span;
            if (std::fabs(t - (float)clock) > 1e-4f) seek(t);
        }
        drawTimingBar((int)i, cell.x, cell.x + cellWidth, cell.y, cell.y + cellHeight, span);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ParticleEffectWindow::drawInspector() {
    if (selected < 0 || selected >= (int)nodes.size()) return;
    const Node& n = nodes[(size_t)selected];
    ImGui::AlignTextToFramePadding();
    ImGui::Text(ICON_FA_FIRE " %s", n.name.c_str());
    UIProbe::item("particle_effect.inspector.header");
    ImGui::TextDisabled("%s", n.effect.empty() ? "inline effect (this GameObject only)" : n.effect.c_str());
    if (n.missing) {
        ImGui::TextDisabled("The file does not read.");
        return;
    }
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    const bool readable = readSelected(document, &diagnostics);
    particlerows::drawDiagnostics(diagnostics);
    if (!readable) return;

    CommandHistory* history = n.effect.empty() ? CommandHandle::get(n.sceneId) : project->getProjectCommandHistory();
    ParticleEffectDocument edited = document;
    particlerows::EffectRows ui;
    ui.project = project;
    ui.history = history;
    ui.expanded = &expanded;
    ui.prefix = "particle_effect";
    particlerows::drawEffect(ui, edited);
    if (ui.changed) {
        editSelected([&edited](ParticleEffectDocument& target) {
            target = edited;
            return true;
        }, ui.commit);
    } else if (ui.commit) {
        pushEdit(nullptr, nullptr, true);
    }
}

void ParticleEffectWindow::drawExposed() {
    const std::vector<ExposedRow> exposed = exposedRows();
    const std::vector<std::string> triggers = triggerNames();
    if (!ImGui::CollapsingHeader("Exposed Parameters", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextDisabled("Drive the preview only; the effect keeps its values.");
    if (exposed.empty() && triggers.empty()) {
        ImGui::TextDisabled("The effect exposes no parameter and answers no trigger.");
        return;
    }
    for (const ExposedRow& row : exposed) {
        ImGui::PushID(row.name.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.name.c_str());
        ImGui::SameLine(Theme::dpi(130.0f));
        ImGui::SetNextItemWidth(-FLT_MIN);
        Vector4 value = row.value;
        bool edited = false;
        switch (row.type) {
            case ExposedType::Float: edited = ImGui::DragFloat("##value", &value.x, 0.01f); break;
            case ExposedType::Vector3: edited = ImGui::DragFloat3("##value", &value.x, 0.01f); break;
            case ExposedType::Color: {
                ui::ColorFieldOptions options;
                options.linearStorage = false;
                options.label = row.name.c_str();
                const ui::EditPhase phase = ui::colorField("##value", value, options);
                edited = phase == ui::EditPhase::Changing || phase == ui::EditPhase::Applied || phase == ui::EditPhase::Cancelled;
                break;
            }
        }
        UIProbe::item("particle_effect.exposed." + row.name);
        if (edited) setExposedValue(row.name, value);
        ImGui::PopID();
    }
    if (!triggers.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Triggers");
        for (const std::string& name : triggers) {
            ImGui::SameLine();
            if (ImGui::Button((ICON_FA_BOLT " " + name).c_str())) fireTrigger(name);
            UIProbe::item("particle_effect.trigger." + name);
        }
    }
}
