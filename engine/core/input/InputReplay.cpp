// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "input/InputReplay.h"

#include "input/InputRecorder.h"
#include "input/InputScriptRunner.h"

#include <algorithm>

namespace jasperengine::input {

namespace {

    std::vector<std::shared_ptr<Replay>>& replays(){
        static std::vector<std::shared_ptr<Replay>> list;
        return list;
    }

}

Replay::Replay(std::shared_ptr<const ParsedScript> script, ReplayOptions options)
    : script_(std::move(script)), options(std::move(options)){
}

Replay::~Replay(){
    if (gate)
        InputRecorder::removeRealInputGate(gate);
}

std::shared_ptr<Replay> Replay::start(std::shared_ptr<const ParsedScript> script, ReplayOptions options){
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
