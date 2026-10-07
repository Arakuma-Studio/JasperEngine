// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMTIMELINE_H
#define ANIMTIMELINE_H

// Timeline of the anime API, ported from Anime.js 4.5.0 (timeline/timeline.js,
// timeline/position.js): animations, timers, sets, calls and synced animations placed at
// positions (numbers, "+=100", "-=100", "*=2", "<", "<<", "label", "label+=50", stagger
// functions), labels, defaults inherited by the children.

#include "Export.h"
#include "AnimTimer.h"
#include "AnimTargets.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class JASPERENGINE_API AnimTimeline : public AnimTimer {
    public:
        // anime.createTimeline(params): a started timeline (unless autoplay = false)
        static std::shared_ptr<AnimTimeline> create(const AnimParam& params = AnimParam::object());

        ~AnimTimeline() override;

        // add(targets, params[, position]): an animation; a FUNCTION position (stagger) adds
        // one animation per target at the position it returns
        AnimTimeline& add(const AnimTargetList& targets, const AnimParam& params, const AnimParam& position = AnimParam());
        // add(timerParams[, position]): a timer
        AnimTimeline& add(const AnimParam& timerParams, const AnimParam& position = AnimParam());
        // set(targets, props[, position]): values set at the position
        AnimTimeline& set(const AnimTargetList& targets, AnimParam params, const AnimParam& position = AnimParam());
        // call(fn[, position]): fn(timeline) when the playhead crosses the position
        AnimTimeline& call(std::function<void(AnimTimer&)> fn, const AnimParam& position = AnimParam());
        // label(name[, position])
        AnimTimeline& label(const std::string& name, const AnimParam& position = AnimParam());
        // sync(timer[, position]): drives timer's currentTime from this timeline
        AnimTimeline& sync(const std::shared_ptr<AnimTimer>& timer, const AnimParam& position = AnimParam());
        // remove(targets[, property])
        AnimTimeline& remove(const AnimTargetList& targets, const std::string& property = "");

        AnimTimer& stretch(double newDuration) override;
        AnimTimer& refresh() override;
        AnimTimer& revert() override;

        // parseTimelinePosition
        double parsePosition(const AnimParam& position);

        const std::map<std::string, double>& getLabels() const { return labels; }
        bool hasOnlyInvalidTargets() const override;

        // the children list (Anime's _head/_tail) and its owners
        AnimTimer* childHead = nullptr;
        AnimTimer* childTail = nullptr;
        AnimParam defaults;              // timeline defaults merged over the global ones
        bool composition = true;
        std::map<std::string, double> labels;

        explicit AnimTimeline(const AnimParam& params);

    private:
        void addChild(const AnimParam& childParams, double timePosition, const AnimTargetList* targets,
                      int index = 0, const AnimTargetList* allTargets = nullptr);

        std::vector<std::shared_ptr<AnimTimer>> ownedChildren;
    };

}

#endif //ANIMTIMELINE_H
