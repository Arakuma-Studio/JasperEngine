// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DialogueRunner.h"

#include "Engine.h"
#include "Log.h"
#include "dialogue/DialogueVariables.h"

#include <memory>

using namespace jasperengine;

namespace {

    DialogueRunnerComponent* componentOf(Scene* scene, Entity entity) {
        return scene ? scene->findComponent<DialogueRunnerComponent>(entity) : nullptr;
    }

    void syncMirrors(Scene* scene, Entity entity, const dialogue::Runner& runner) {
        if (DialogueRunnerComponent* component = componentOf(scene, entity)) {
            component->isRunning = runner.isRunning();
            component->state = dialogue::stateName(runner.getState());
            component->currentNode = runner.getCurrentNode();
        }
    }

    struct PendingStart {
        Scene* scene = nullptr;
        std::weak_ptr<char> lifetime;
        Entity entity = NULL_ENTITY;
    };

    struct AutoStart {
        std::vector<PendingStart> pending;
        bool subscribed = false;
    };

    AutoStart& autoStart() {
        static AutoStart instance;
        return instance;
    }

    constexpr const char* AUTO_START_TAG = "jasperengine_dialogue_autostart";

    void startPending() {
        AutoStart& state = autoStart();
        if (state.pending.empty()) return;
        std::vector<PendingStart> starting = std::move(state.pending);
        state.pending.clear();
        for (const PendingStart& item : starting) {
            if (item.lifetime.expired()) continue;
            DialogueRunnerComponent* component = componentOf(item.scene, item.entity);
            if (!component || !component->autoStart) continue;
            DialogueRunner(item.scene, item.entity).start();
        }
    }

}

DialogueRunner::DialogueRunner(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<DialogueRunnerComponent>(entity)){
        scene->addComponent<DialogueRunnerComponent>(entity, {});
    }
}

DialogueRunner::~DialogueRunner(){
}

DialogueRunner::DialogueRunner(const DialogueRunner& rhs) = default;
DialogueRunner& DialogueRunner::operator=(const DialogueRunner& rhs) = default;
DialogueRunner::DialogueRunner(DialogueRunner&& rhs) noexcept = default;
DialogueRunner& DialogueRunner::operator=(DialogueRunner&& rhs) noexcept = default;

dialogue::Runner* dialogue::runnerOf(Scene* scene, Entity entity){
    DialogueRunnerComponent* component = componentOf(scene, entity);
    if (!component) return nullptr;
    if (!component->runner){
        auto runner = std::make_shared<Runner>();
        runner->setOwner(scene, entity);
        runner->setSeed(component->seed != 0 ? static_cast<uint64_t>(static_cast<uint32_t>(component->seed)) : static_cast<uint64_t>(entity));
        // the events look the component up each time: its storage moves as the scene's arrays grow
        runner->onStart = [scene, entity](Runner& self){
            syncMirrors(scene, entity, self);
            if (DialogueRunnerComponent* c = componentOf(scene, entity)) c->onStart.call(jasperengine::DialogueRunner(scene, entity));
        };
        runner->onLine = [scene, entity](Runner& self, const Line& line){
            syncMirrors(scene, entity, self);
            if (DialogueRunnerComponent* c = componentOf(scene, entity)) c->onLine.call(jasperengine::DialogueRunner(scene, entity), line);
        };
        runner->onEnd = [scene, entity](Runner& self, const std::string& result){
            syncMirrors(scene, entity, self);
            if (DialogueRunnerComponent* c = componentOf(scene, entity)) c->onEnd.call(jasperengine::DialogueRunner(scene, entity), result);
        };
        runner->onVariableChanged = [scene, entity](Runner&, const std::string& name, const Value& value){
            if (DialogueRunnerComponent* c = componentOf(scene, entity)) c->onVariableChanged.call(jasperengine::DialogueRunner(scene, entity), name, value);
        };
        component->runner = runner;
    }
    Runner* runner = component->runner.get();
    const std::string& path = component->dialogue.path;
    if (path.empty()){
        if (runner->getDialogue()) runner->setDialogue(nullptr);
    } else if (!runner->getDialogue() || runner->getDialogue()->path != path){
        std::string error;
        std::shared_ptr<const Program> program = Programs::load(path, &error);
        if (program) runner->setDialogue(program);
        else Log::error("Dialogue Runner: %s", error.c_str());
    }
    return runner;
}

dialogue::Runner* DialogueRunner::getRunner() const{
    return dialogue::runnerOf(scene, entity);
}

bool DialogueRunner::start(const std::string& entry, const LocalizationArgs& args){
    dialogue::Runner* runner = getRunner();
    if (!runner) return false;
    if (!runner->getDialogue()){
        Log::warn("Dialogue Runner on %s: no dialogue to start (set its Dialogue)", getName().c_str());
        return false;
    }
    const std::string chosen = entry.empty() ? getComponent<DialogueRunnerComponent>().startEntry : entry;
    const bool started = runner->start(chosen, args);
    syncMirrors(scene, entity, *runner);
    return started;
}

bool DialogueRunner::advance(){
    dialogue::Runner* runner = getRunner();
    const bool done = runner && runner->advance();
    if (runner) syncMirrors(scene, entity, *runner);
    return done;
}

bool DialogueRunner::choose(int index){
    dialogue::Runner* runner = getRunner();
    const bool done = runner && runner->choose(index);
    if (runner) syncMirrors(scene, entity, *runner);
    return done;
}

bool DialogueRunner::resume(){
    dialogue::Runner* runner = getRunner();
    const bool done = runner && runner->resume();
    if (runner) syncMirrors(scene, entity, *runner);
    return done;
}

void DialogueRunner::stop(){
    if (dialogue::Runner* runner = getRunner()){
        runner->stop();
        syncMirrors(scene, entity, *runner);
    }
}

void DialogueRunner::setArg(const std::string& name, const LocalizationValue& value){
    if (dialogue::Runner* runner = getRunner()) runner->setArg(name, value);
}

void DialogueRunner::setDialogue(const std::string& path){
    getComponent<DialogueRunnerComponent>().dialogue = path;
    if (dialogue::Runner* runner = getRunner()) syncMirrors(scene, entity, *runner);
}

std::string DialogueRunner::getDialogue() const{
    return getComponent<DialogueRunnerComponent>().dialogue.path;
}

void DialogueRunner::setStartEntry(const std::string& entry){
    getComponent<DialogueRunnerComponent>().startEntry = entry;
}

std::string DialogueRunner::getStartEntry() const{
    return getComponent<DialogueRunnerComponent>().startEntry;
}

void DialogueRunner::setAutoStart(bool value){
    getComponent<DialogueRunnerComponent>().autoStart = value;
}

bool DialogueRunner::isAutoStart() const{
    return getComponent<DialogueRunnerComponent>().autoStart;
}

void DialogueRunner::setSeed(int seed){
    DialogueRunnerComponent& component = getComponent<DialogueRunnerComponent>();
    component.seed = seed;
    if (component.runner) component.runner->setSeed(seed != 0 ? static_cast<uint64_t>(static_cast<uint32_t>(seed)) : static_cast<uint64_t>(entity));
}

int DialogueRunner::getSeed() const{
    return getComponent<DialogueRunnerComponent>().seed;
}

std::string DialogueRunner::getState() const{
    const DialogueRunnerComponent& component = getComponent<DialogueRunnerComponent>();
    return component.runner ? dialogue::stateName(component.runner->getState()) : "idle";
}

bool DialogueRunner::isRunning() const{
    const DialogueRunnerComponent& component = getComponent<DialogueRunnerComponent>();
    return component.runner && component.runner->isRunning();
}

std::string DialogueRunner::getCurrentNode() const{
    const DialogueRunnerComponent& component = getComponent<DialogueRunnerComponent>();
    return component.runner ? component.runner->getCurrentNode() : std::string();
}

const dialogue::Line* DialogueRunner::getCurrentLine() const{
    const DialogueRunnerComponent& component = getComponent<DialogueRunnerComponent>();
    return component.runner ? component.runner->getCurrentLine() : nullptr;
}

FunctionSubscribe<void(DialogueRunner)>& DialogueRunner::getOnStart(){
    return getComponent<DialogueRunnerComponent>().onStart;
}

FunctionSubscribe<void(DialogueRunner, const dialogue::Line&)>& DialogueRunner::getOnLine(){
    return getComponent<DialogueRunnerComponent>().onLine;
}

FunctionSubscribe<void(DialogueRunner, const std::string&)>& DialogueRunner::getOnEnd(){
    return getComponent<DialogueRunnerComponent>().onEnd;
}

FunctionSubscribe<void(DialogueRunner, const std::string&, const dialogue::Value&)>& DialogueRunner::getOnVariableChanged(){
    return getComponent<DialogueRunnerComponent>().onVariableChanged;
}

void dialogue::scheduleAutoStart(Scene* scene, const std::vector<Entity>* entities){
    if (!scene) return;
    auto components = scene->getComponentArray<DialogueRunnerComponent>();
    if (!components || components->size() == 0) return;
    AutoStart& state = autoStart();
    const size_t before = state.pending.size();
    auto consider = [&](Entity entity, const DialogueRunnerComponent& component){
        if (!component.autoStart || component.dialogue.path.empty()) return;
        // a prefab instantiated before its scene started is seen at the instantiate and at the start
        for (const PendingStart& item : state.pending){
            if (item.scene == scene && item.entity == entity) return;
        }
        state.pending.push_back({scene, scene->getLifetime(), entity});
    };
    if (entities){
        for (Entity entity : *entities){
            if (const DialogueRunnerComponent* component = componentOf(scene, entity)) consider(entity, *component);
        }
    } else {
        for (size_t i = 0; i < components->size(); i++) consider(components->getEntity(i), components->getComponentFromIndex(i));
    }
    if (state.pending.size() == before || state.subscribed) return;
    // once per Play: Engine::clearAllSubscriptions takes it at Stop, with clearRuntime
    state.subscribed = true;
    Engine::onPostUpdate.add(AUTO_START_TAG, std::function<void()>(startPending));
}

void dialogue::clearRuntime(){
    Handlers::clear();
    AutoStart& state = autoStart();
    state.pending.clear();
    state.subscribed = false;
    Programs::clear();
}
