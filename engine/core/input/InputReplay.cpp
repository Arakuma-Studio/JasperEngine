// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only with the input scripts module (input/InputScriptSwitch.h); InputScriptsOff.cpp answers the
// engine without it
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "input/InputReplay.h"

#include "input/InputRecorder.h"
#include "input/InputScriptRunner.h"

#include "Engine.h"
#include "Scene.h"
#if JASPERENGINE_HTMLUI
#include "component/UIDocumentComponent.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElement.h"
#endif

#include <algorithm>

namespace jasperengine::input {

namespace {

    std::vector<std::shared_ptr<Replay>>& replays(){
        static std::vector<std::shared_ptr<Replay>> list;
        return list;
    }

#if JASPERENGINE_HTMLUI
    // a selector in the running UI Documents drawn on the screen, the topmost document first (a
    // higher sortOrder draws on top): the centre of the first match with a size (R8)
    bool resolveDocumentTarget(const std::string& selector, const ScriptJson&, Vector2& point, std::string& error, ScriptJson* found){
        struct Candidate { Scene* scene; Entity entity; int sortOrder; };
        std::vector<Candidate> candidates;
        for (Scene* scene : Engine::getSceneLayers()) {
            if (!scene || !scene->isComponentRegistered<UIDocumentComponent>())
                continue;
            auto* documents = scene->getComponentArray<UIDocumentComponent>();
            for (size_t i = 0; documents && i < documents->size(); i++) {
                const Entity entity = documents->getEntity(i);
                const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
                if (component && component->renderMode == UIDocumentRenderMode::SCREEN)
                    candidates.push_back({scene, entity, component->sortOrder});
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.sortOrder > b.sortOrder; });
        for (const Candidate& c : candidates) {
            UIDocument document(c.scene, c.entity);
            for (const UIElement& element : document.querySelectorAll(selector)) {
                const Rect box = element.getBoundingBox();
                if (box.getWidth() <= 0 || box.getHeight() <= 0)
                    continue;
                point = Vector2(box.getX() + box.getWidth() / 2.0f, box.getY() + box.getHeight() / 2.0f);
                if (found)
                    *found = {{"entity", c.entity}, {"name", c.scene->getEntityName(c.entity)}};
                return true;
            }
        }
        error = "matched no element of the running UI Documents" + std::string(candidates.empty() ? " (none is drawn on the screen)" : "");
        return false;
    }
#endif

}

PlayerHost runtimePlayerHost(){
    PlayerHost host;
#if JASPERENGINE_HTMLUI
    host.resolveTarget = &resolveDocumentTarget;
#endif
    return host;
}

Replay::Replay(std::shared_ptr<const ParsedScript> script, ReplayOptions options)
    : script_(std::move(script)), options(std::move(options)){
}

Replay::~Replay(){
    if (gate)
        InputRecorder::removeRealInputGate(gate);
}

std::shared_ptr<Replay> Replay::start(std::shared_ptr<const ParsedScript> script, ReplayOptions options){
#if JASPERENGINE_HTMLUI
    if (!options.host.resolveTarget)
        options.host.resolveTarget = &resolveDocumentTarget;
#endif
    std::shared_ptr<Replay> replay(new Replay(std::move(script), std::move(options)));
    replays().push_back(replay);
    const ReplayOptions& o = replay->options;
    if (o.blockRealInput || o.stopOnRealInput) {
        std::weak_ptr<Replay> weak = replay;
        std::function<void()> onPress;
        if (o.stopOnRealInput) {
            onPress = [weak]() {
                if (std::shared_ptr<Replay> self = weak.lock())
                    self->stop();
            };
        }
        replay->gate = InputRecorder::addRealInputGate(o.blockRealInput, std::move(onPress));
    }
    replay->runOnce();
    return replay;
}

std::vector<std::shared_ptr<Replay>> Replay::running(){
    return replays();
}

void Replay::stopAll(){
    std::vector<std::shared_ptr<Replay>> list;
    list.swap(replays());
    // Play stops: no callback runs (the game's Lua state goes)
    for (const std::shared_ptr<Replay>& replay : list) {
        replay->stopped = true;
        replay->done = true;
        ScriptRunner::stop(replay->player.get());
        if (replay->gate)
            InputRecorder::removeRealInputGate(replay->gate);
        replay->gate = 0;
    }
}

void Replay::runOnce(){
    PlayOptions play = options.play;
    play.stopOnDivergence = options.stopOnDivergence;
    player = std::make_shared<ScriptPlayer>(script_, options.host, play);
    std::weak_ptr<Replay> weak = weak_from_this();
    ScriptRunner::start(player, [weak](ScriptPlayer& ended) {
        std::shared_ptr<Replay> self = weak.lock();
        if (!self || self->done)
            return;
        if (self->options.loop && ended.report().done && !ended.report().failed) {
            self->runOnce();
            return;
        }
        self->finish();
    });
}

void Replay::finish(){
    if (done)
        return;
    done = true;
    if (gate)
        InputRecorder::removeRealInputGate(gate);
    gate = 0;
    std::shared_ptr<Replay> keep = shared_from_this();
    auto& list = replays();
    list.erase(std::remove(list.begin(), list.end(), keep), list.end());
    if (options.onFinished)
        options.onFinished(*this);
}

void Replay::stop(){
    if (done)
        return;
    stopped = true;
    ScriptRunner::stop(player.get());
    finish();
}

int64_t Replay::frame() const{
    return done ? -1 : ScriptRunner::frameOf(player.get());
}

const ReplayReport& Replay::report() const{
    return player->report();
}

std::string Replay::errorText() const{
    return player->errorText();
}

}

#endif // JASPERENGINE_INPUT_SCRIPTS_ENABLED
