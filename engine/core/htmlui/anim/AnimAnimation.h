// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMANIMATION_H
#define ANIMANIMATION_H

// Animation of the anime API, ported from Anime.js 4.5.0 (animation/animation.js,
// animation/composition.js, animation/additive.js): tweens of target properties with
// keyframes, from/to values, relative values, function values, units, colours and complex
// strings, and the replace/none/blend compositions between animations of a property.

#include "Export.h"
#include "AnimTargets.h"
#include "AnimTimer.h"
#include "AnimValue.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class AnimAnimation;

    enum class AnimComposition { REPLACE = 0, NONE = 1, BLEND = 2 };

    // Anime's Tween
    struct JASPERENGINE_API AnimTween {
        AnimAnimation* parent = nullptr;
        uint64_t id = 0;
        std::string property;
        AnimTargetPtr target;
        AnimRawValue value;                              // _value: the last composed value / the end value
        std::function<AnimRawValue()> toFunc, fromFunc;  // function values, for refresh()
        EaseFn ease;
        std::vector<double> fromNumbers, toNumbers;
        std::vector<std::string> strings;
        double fromNumber = 0, toNumber = 0;
        std::vector<double> numbers;                     // _numbers: additive/colour scratch
        double number = 0;                               // _number
        std::string unit;
        EaseFn modifier;                                 // empty: identity
        double currentTime = 0;
        double startTime = 0;
        double delay = 0;
        double updateDuration = 0;
        double changeDuration = 0;
        double absoluteStartTime = 0;
        double absoluteUpdateStartTime = 0;
        double absoluteEndTime = 0;
        int hasFromValue = 0;
        AnimTweenType tweenType = AnimTweenType::OBJECT;
        AnimValueType valueType = AnimValueType::NUMBER;
        AnimComposition composition = AnimComposition::REPLACE;
        int isOverlapped = 0;
        int isOverridden = 0;
        AnimRawValue inlineValue;
        AnimTween* prevRep = nullptr;
        AnimTween* nextRep = nullptr;
        AnimTween* prevAdd = nullptr;
        AnimTween* nextAdd = nullptr;
        AnimTween* prev = nullptr;
        AnimTween* next = nullptr;
        bool inReplaceList = false;
        bool inAdditiveList = false;
        bool inAnimationList = false;

        double modify(double v) const { return modifier ? modifier(v) : v; }
    };

    class JASPERENGINE_API AnimAnimation : public AnimTimer {
    public:
        // anime.animate(targets, params): a started animation (unless autoplay = false)
        static std::shared_ptr<AnimAnimation> create(const AnimTargetList& targets, const AnimParam& params);
        // utils.set(targets, props): a zero-duration animation rendered now (composition none by default)
        static std::shared_ptr<AnimAnimation> set(const AnimTargetList& targets, AnimParam params);

        ~AnimAnimation() override;

        const AnimTargetList& getTargets() const { return targets; }

        AnimTimer& stretch(double newDuration) override;
        AnimTimer& refresh() override;
        AnimTimer& revert() override;

        // the tweens' list (Anime's _head/_tail)
        AnimTween* head = nullptr;
        AnimTween* tail = nullptr;

        bool hasOnlyInvalidTargets() const override;
        // Anime's constructor-time onRender (autoplay off, a from value), called by the creator
        // once the object is owned by a shared_ptr
        bool pendingCreateRender = false;

        // Anime's constructor; parent/parentPosition for timeline children, fastSet for set()
        AnimAnimation(const AnimTargetList& targets, const AnimParam& params, AnimTimeline* parent, double parentPosition,
                      bool fastSet = false, int index = 0, const AnimTargetList* allTargets = nullptr);

    private:
        AnimTargetList targets;
        std::vector<std::unique_ptr<AnimTween>> ownedTweens;
        bool hasTargets = false;
    };

    // composition helpers (animation/composition.js)
    namespace AnimComposer {
        AnimTweenSiblings& getSiblings(AnimTarget& target, const std::string& property, bool additive);
        void overrideTween(AnimTween& tween);
        void composeTween(AnimTween& tween, AnimTweenSiblings& siblings);
        void removeTweenSiblings(AnimTween& tween);
        // removeTargetsFromRenderable; renderable null: every running top-level timer
        void removeTargets(const AnimTargetList& targets, AnimTimer* renderable, const std::string& property);
        // the additive pass of the engine tick: sums the blended tweens and writes the result
        void updateAdditive();
        // drops the additive state (AnimEngine::reset)
        void resetAdditive();
        // renders one tween value at progress and writes it (unless internalRender); true when written
        bool renderTween(AnimTween& tween, double progress, int internalRender);
        // revertValues
        void revertValues(AnimTimer& renderable, bool inlineStylesOnly = false);
    }

}

#endif //ANIMANIMATION_H
