// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleEffectTiming.h"

#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

namespace {

    // a burst list's times are capped: a burst repeating for ever lists its first loop's cycles
    constexpr size_t kMaxListedBursts = 64;

    const char* timingTrigger(SubEmitterTrigger trigger) {
        switch (trigger) {
            case SubEmitterTrigger::Birth: return "birth";
            case SubEmitterTrigger::Death: return "death";
            case SubEmitterTrigger::Collision: return "collision";
            case SubEmitterTrigger::Trigger: return "trigger";
        }
        return "death";
    }

    bool emits(const ParticleEffectDocument& d) {
        if (!d.emission.present || !d.emission.enabled) return false;
        return !d.emission.rateOverTime.isZero() || !d.emission.rateOverDistance.isZero() || !d.emission.bursts.empty();
    }

    void lifetimeOf(const ParticleEffectDocument& d, ParticleTimingRow& row) {
        float lo = 0.0f, hi = 0.0f;
        d.main.startLifetime.range(lo, hi);
        row.lifetimeMin = std::max(0.0f, std::min(lo, hi));
        row.lifetimeMax = std::max(0.0f, std::max(lo, hi));
    }

    // a system's own clock: its start delay, its emission over Duration (for ever when looping), its bursts
    void systemTiming(const ParticleEffectDocument& d, ParticleTimingRow& row) {
        float delayLo = 0.0f, delayHi = 0.0f;
        d.startDelay.range(delayLo, delayHi);
        row.start = std::max(0.0f, std::min(delayLo, delayHi));
        row.startMax = std::max(row.start, std::max(delayLo, delayHi));
        row.looping = d.looping;
        row.prewarm = d.looping && d.prewarm;
        lifetimeOf(d, row);

        const float duration = std::max(0.0f, d.duration);
        float lastBurst = 0.0f;
        if (d.emission.present && d.emission.enabled) {
            for (const Burst& burst : d.emission.bursts) {
                if (burst.time > duration) continue;
                const int cycles = burst.cycles > 0 ? burst.cycles : (int)kMaxListedBursts;
                const float interval = std::max(burst.interval, 0.0001f);
                for (int c = 0; c < cycles && row.bursts.size() < kMaxListedBursts; c++) {
                    const float t = burst.time + interval * (float)c;
                    if (t > duration) break;
                    row.bursts.push_back(row.start + t);
                    lastBurst = std::max(lastBurst, t);
                }
            }
            std::sort(row.bursts.begin(), row.bursts.end());
        }

        const bool rate = !d.emission.rateOverTime.isZero() || !d.emission.rateOverDistance.isZero();
        if (!emits(d)) {
            row.emitEnd = row.startMax;
            row.end = row.startMax;
        } else if (d.looping) {
            row.emitEnd = ParticleTimingRow::kForever;
            row.end = ParticleTimingRow::kForever;
        } else {
            row.emitEnd = row.startMax + (rate ? duration : lastBurst);
            row.end = row.emitEnd + row.lifetimeMax;
        }
        // bursts alone: the first emission is the first burst, not the end of the start delay
        if (emits(d) && !rate && !row.bursts.empty()) {
            const float first = row.bursts.front() - row.start;
            row.start += first;
            row.startMax += first;
        }
    }

    struct Walker {
        const ParticleEffectTiming::Resolver& resolve;
        std::vector<ParticleTimingRow>& rows;

        void children(const ParticleEffectDocument& d, int parentRow, int depth) {
            if (depth >= kMaxSubEmitterDepth || !d.subEmittersModule.present || !d.subEmittersModule.enabled) return;
            for (size_t e = 0; e < d.subEmitters.size(); e++) {
                const SubEmitterEntry& entry = d.subEmitters[e];
                const ParticleTimingRow parent = rows[(size_t)parentRow];
                ParticleTimingRow row;
                row.name = std::filesystem::path(entry.effect).stem().string();
                row.effect = entry.effect;
                row.depth = depth;
                row.parent = parentRow;
                row.subEmitter = (int)e;
                row.trigger = timingTrigger(entry.trigger);
                row.eventDriven = parent.eventDriven || entry.trigger == SubEmitterTrigger::Collision || entry.trigger == SubEmitterTrigger::Trigger;
                const ParticleEffectDocument* child = entry.effect.empty() ? nullptr : resolve(entry.effect);
                if (!child) {
                    row.missing = true;
                    row.start = row.startMax = row.emitEnd = row.end = parent.start;
                    rows.push_back(row);
                    continue;
                }
                lifetimeOf(*child, row);
                // Inherit Lifetime: the child lives its lifetime times its parent particle's (times the factor)
                if (entry.inheritLifetime) {
                    row.lifetimeMin *= parent.lifetimeMin * entry.inheritFactor;
                    row.lifetimeMax *= parent.lifetimeMax * entry.inheritFactor;
                }
                // a child emits on its parent's events, its own clock unused
                switch (entry.trigger) {
                    case SubEmitterTrigger::Birth:
                        row.start = parent.start;
                        row.emitEnd = parent.emitEnd;
                        break;
                    case SubEmitterTrigger::Death:
                        row.start = std::min(parent.start + parent.lifetimeMin, parent.end);
                        row.emitEnd = parent.end;
                        break;
                    case SubEmitterTrigger::Collision:
                    case SubEmitterTrigger::Trigger:
                        row.start = parent.start;
                        row.emitEnd = parent.end;
                        break;
                }
                row.startMax = row.start;
                row.looping = parent.looping;
                row.end = std::isinf(row.emitEnd) ? ParticleTimingRow::kForever : row.emitEnd + row.lifetimeMax;
                rows.push_back(row);
                children(*child, (int)rows.size() - 1, depth + 1);
            }
        }
    };

    nlohmann::ordered_json timingTime(float t) {
        return std::isinf(t) ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(ParticleEffectCodec::jsonFloat(t));
    }

}

std::vector<ParticleTimingRow> ParticleEffectTiming::compute(const std::vector<ParticleTimingSystem>& systems, const Resolver& resolve) {
    const Resolver library = [](const std::string& path) -> const ParticleEffectDocument* {
        return ParticleEffectLibrary::get(path).document.get();
    };
    std::vector<ParticleTimingRow> rows;
    Walker walker{resolve ? resolve : library, rows};
    for (const ParticleTimingSystem& system : systems) {
        if (!system.document) continue;
        ParticleTimingRow row;
        row.name = system.name;
        row.effect = system.effect;
        systemTiming(*system.document, row);
        rows.push_back(row);
        walker.children(*system.document, (int)rows.size() - 1, 1);
    }
    return rows;
}

float ParticleEffectTiming::span(const std::vector<ParticleTimingRow>& rows, float minimum) {
    float result = minimum;
    for (const ParticleTimingRow& row : rows) {
        for (float t : {row.startMax, row.emitEnd, row.end}) {
            if (!std::isinf(t)) result = std::max(result, t);
        }
        // a looping system shows two of its loops' worth of particles
        if (std::isinf(row.end)) result = std::max(result, row.startMax + row.lifetimeMax * 2.0f);
        if (!row.bursts.empty()) result = std::max(result, row.bursts.back() + row.lifetimeMax);
    }
    return result;
}

nlohmann::ordered_json ParticleEffectTiming::toJson(const std::vector<ParticleTimingRow>& rows) {
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const ParticleTimingRow& row : rows) {
        nlohmann::ordered_json item = {
            {"name", row.name},
            {"effect", row.effect},
            {"depth", row.depth},
            {"parent", row.parent < 0 ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(row.parent)},
        };
        if (row.depth > 0) {
            item["sub_emitter"] = row.subEmitter;
            item["trigger"] = row.trigger;
        }
        item["looping"] = row.looping;
        item["prewarm"] = row.prewarm;
        item["event_driven"] = row.eventDriven;
        if (row.missing) item["missing"] = true;
        item["start"] = timingTime(row.start);
        item["start_max"] = timingTime(row.startMax);
        item["emit_end"] = timingTime(row.emitEnd);
        item["lifetime"] = {timingTime(row.lifetimeMin), timingTime(row.lifetimeMax)};
        item["end"] = timingTime(row.end);
        nlohmann::ordered_json bursts = nlohmann::ordered_json::array();
        for (float t : row.bursts) bursts.push_back(timingTime(t));
        item["bursts"] = bursts;
        list.push_back(item);
    }
    return {{"rows", list}, {"span", timingTime(span(rows))}};
}
