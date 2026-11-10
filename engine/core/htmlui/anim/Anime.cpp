// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Anime.h"
#include "AnimText.h"

using namespace jasperengine;

std::shared_ptr<AnimAnimation> Anime::animate(const AnimTargetList& targets, const AnimParam& params){
    return AnimAnimation::create(targets, params.isObject() ? params : AnimParam::object());
}

std::shared_ptr<AnimTimeline> Anime::createTimeline(const AnimParam& params){
    return AnimTimeline::create(params.isObject() ? params : AnimParam::object());
}

std::shared_ptr<AnimTimer> Anime::createTimer(const AnimParam& params){
    return AnimTimer::create(params.isObject() ? params : AnimParam::object());
}

std::shared_ptr<AnimSpring> Anime::createSpring(const AnimSpringParams& params){
    return std::make_shared<AnimSpring>(params);
}

AnimParam Anime::stagger(const AnimParam& value, const AnimParam& params){
    return AnimUtils::stagger(value, params.isObject() ? params : AnimParam::object());
}

std::shared_ptr<AnimText> Anime::splitText(const AnimTargetList& targets, const AnimParam& params){
    return AnimText::create(targets.empty() ? UIElement() : AnimTargets::getElement(targets.front()),
                            params.isObject() ? params : AnimParam::object());
}
