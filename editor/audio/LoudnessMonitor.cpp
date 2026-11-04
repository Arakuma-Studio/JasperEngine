// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LoudnessMonitor.h"

#include "audio/AudioEffects.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerIO.h"

#include <algorithm>

using namespace jasperengine;

editor::LoudnessMonitor& editor::LoudnessMonitor::get() {
    static LoudnessMonitor monitor;
    return monitor;
}

void editor::LoudnessMonitor::markVisible() {
    visible = true;
}

void editor::LoudnessMonitor::requestByAgent() {
    agent = true;
}

void editor::LoudnessMonitor::reset() {
    meter.reset();
}

void editor::LoudnessMonitor::drain() {
    if (!AudioMixer::isLoudnessTapOn()) return;
    scratch.resize(4096 * 2);
    unsigned int channels = 0;
    unsigned int rate = 0;
    for (;;) {
        const size_t frames = AudioMixer::drainLoudnessTap(scratch.data(), scratch.size() / 2, channels, rate);
        if (frames == 0) break;
        if (!meter || meter->getSampleRate() != rate) {
            AudioMeterOptions options;
            options.bands = false;
            meter = std::make_unique<AudioMeter>(rate, channels, options);
        }
        meter->process(scratch.data(), frames);
    }
}

void editor::LoudnessMonitor::update(bool playing) {
    if (playing && !wasPlaying) meter.reset();
    if (!playing && wasPlaying) agent = false;
    wasPlaying = playing;

    visibleLastFrame = visible;
    const bool wanted = playing && (visible || agent);
    visible = false;
    if (wanted != AudioMixer::isLoudnessTapOn()) AudioMixer::setLoudnessTap(wanted);
    if (wanted) drain();
}

editor::LoudnessMonitor::Reading editor::LoudnessMonitor::reading() {
    drain();
    Reading out;
    if (meter) {
        const LoudnessResult result = meter->result();
        out.momentary = meter->momentaryNow();
        out.shortTerm = meter->shortTermNow();
        out.integrated = result.integrated;
        out.truePeakDb = result.truePeakDb;
    }
    for (const AudioMixerGroupDef& group : AudioMixer::getDef().groups) {
        if (!group.parentId.empty()) continue;
        for (const AudioMixerEffectDef& effect : group.effects) {
            if (effect.type == AudioEffectType::Limiter) {
                out.gainReductionDb = std::max(out.gainReductionDb, AudioMixer::getEffectGainReductionDb(effect.id));
            }
        }
    }
    return out;
}
