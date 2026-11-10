// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIME_H
#define ANIME_H

// The anime API (modelled on Anime.js v4) for C++; Lua scripts get the same functions in the
// global table `anime` (engine/core/script/binding/HtmlUIAnimeLua.cpp).
//
//   auto box = AnimTargets::fromSelector("#box");
//   Anime::animate(box, AnimParam::object({{"translateX", 250}, {"rotate", "1turn"},
//                                          {"duration", 800}, {"ease", "outElastic(1, .6)"}}));
//   auto tl = Anime::createTimeline(AnimParam::object({{"loop", true}, {"alternate", true}}));
//   tl->add(box, AnimParam::object({{"opacity", 0.5}})).label("half").add(box, AnimParam::object({{"scale", 2}}), "half+=100");
//
// Everything runs on the UI clock (HtmlUIClock); `clock = "game"` runs a timer, animation or
// timeline on the scaled game time instead.

#include "Export.h"
#include "AnimAnimation.h"
#include "AnimEasing.h"
#include "AnimEngine.h"
#include "AnimParams.h"
#include "AnimSpring.h"
#include "AnimTargets.h"
#include "AnimTimeline.h"
#include "AnimTimer.h"
#include "AnimUtils.h"

#include <memory>

namespace jasperengine {

    class AnimText;

    class JASPERENGINE_API Anime {
    public:
        static std::shared_ptr<AnimAnimation> animate(const AnimTargetList& targets, const AnimParam& params);
        static std::shared_ptr<AnimTimeline> createTimeline(const AnimParam& params = AnimParam::object());
        static std::shared_ptr<AnimTimer> createTimer(const AnimParam& params = AnimParam::object());
        static std::shared_ptr<AnimSpring> createSpring(const AnimSpringParams& params = AnimSpringParams());
        static AnimParam stagger(const AnimParam& value, const AnimParam& params = AnimParam::object());
        static std::shared_ptr<AnimText> splitText(const AnimTargetList& targets, const AnimParam& params = AnimParam::object());
    };

}

#endif //ANIME_H
