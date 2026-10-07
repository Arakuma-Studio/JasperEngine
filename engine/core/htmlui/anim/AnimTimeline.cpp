// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 (MIT) timeline/timeline.js and timeline/position.js, plus
// core/globals.js defaults.

#include "AnimTimeline.h"

#include "AnimAnimation.h"
#include "AnimCommon.h"
#include "AnimEngine.h"
#include "AnimSpring.h"

#include <cmath>

using namespace jasperengine;

namespace {

    uint64_t timelineId = 0;

    // mergeObjects(o1, o2): o1's members, then o2's missing ones
    AnimParam mergeObjects(const AnimParam& o1, const AnimParam& o2){
        AnimParam merged = o1;
        for (const AnimParam::Member& member : o2.getMembers()){
            if (!o1.has(member.first))
                merged.set(member.first, member.second);
        }
        return merged;
    }

    double getTimelineTotalDuration(const AnimTimeline& tl){
        double d = AnimMath::clampInfinity(((tl.iterationDuration + tl.loopDelay) * tl.iterationCount) - tl.loopDelay);
        return d && !std::isnan(d) ? d : AnimConst::minValue;
    }

    template<typename F>
    void forEachChild(AnimTimeline& timeline, F&& fn, bool reverse = false){
        AnimTimer* child = reverse ? timeline.childTail : timeline.childHead;
        while (child){
            AnimTimer* following = reverse ? child->prev : child->next;
            fn(*child);
            child = following;
        }
    }

}

const AnimParam& AnimDefaults::global(){
    static const AnimParam defaults = AnimParam::object({
        {"playbackRate", 1},
        {"frameRate", AnimConst::maxFps},
        {"loop", 0},
        {"reversed", false},
        {"alternate", false},
        {"autoplay", true},
        {"persist", false},
        {"duration", AnimConst::K},
        {"delay", 0},
        {"loopDelay", 0},
        {"ease", "out(2)"},
        {"composition", "replace"},
    });
    return defaults;
}

const AnimParam& AnimDefaults::of(const AnimTimeline* parent){
    return parent ? parent->defaults : global();
}

std::shared_ptr<AnimTimeline> AnimTimeline::create(const AnimParam& params){
    std::shared_ptr<AnimTimeline> timeline = std::make_shared<AnimTimeline>(params);
    timeline->init();
    return timeline;
}

AnimTimeline::AnimTimeline(const AnimParam& params): AnimTimer(Kind::TIMELINE, params, nullptr, 0){
    ++timelineId;
    const AnimParam* idParam = params.find("id");
    id = idParam ? *idParam : AnimParam((double)timelineId);
    // grows with the children
    duration = 0;
    const AnimParam& defaultsParams = params.get("defaults");
    defaults = defaultsParams.isObject() ? mergeObjects(defaultsParams, AnimDefaults::global()) : AnimDefaults::global();
    composition = params.has("composition") ? params.get("composition").isTruthy() : true;
    const AnimParam& onRenderParam = params.get("onRender");
    onRender = onRenderParam.isCallback() ? onRenderParam.asCallback() : nullptr;
    const AnimParam& playbackEaseParam = params.get("playbackEase");
    if (playbackEaseParam.isEase()){
        ease = playbackEaseParam.asEase();
    } else if (playbackEaseParam.isString()){
        ease = AnimEasing::parse(playbackEaseParam.asString());
    } else if (playbackEaseParam.isSpring() && playbackEaseParam.asSpring()){
        ease = playbackEaseParam.asSpring()->getEaseFunction();
    }
    iterationDuration = 0;
}

AnimTimeline::~AnimTimeline(){
    // children outliving the timeline (a script kept one from a callback) become top-level
    for (const std::shared_ptr<AnimTimer>& child : ownedChildren){
        if (child->parent == this)
            child->parent = nullptr;
    }
}

void AnimTimeline::addChild(const AnimParam& childParams, double timePosition, const AnimTargetList* childTargets,
                            int index, const AnimTargetList* allTargets){
    const AnimParam& durationParam = childParams.get("duration");
    const bool isSetter = durationParam.isNumber() && durationParam.asNumber() <= AnimConst::minValue;
    // 0-duration children and set() end their value at the position
    const double adjustedPosition = isSetter ? timePosition - AnimConst::minValue : timePosition;
    if (composition)
        AnimRender::tick(*this, adjustedPosition, 1, 1, AnimTickMode::AUTO);
    std::shared_ptr<AnimTimer> child;
    if (childTargets){
        std::shared_ptr<AnimAnimation> animation = std::make_shared<AnimAnimation>(*childTargets, childParams, this, adjustedPosition, false, index, allTargets);
        if (animation->pendingCreateRender && animation->onRender)
            animation->onRender(*animation);
        animation->pendingCreateRender = false;
        child = animation;
    } else {
        child = std::shared_ptr<AnimTimer>(new AnimTimer(Kind::TIMER, childParams, this, adjustedPosition));
    }
    if (composition)
        child->init(true);
    ownedChildren.push_back(child);
    AnimTimer* prev = childTail;
    child->prev = prev;
    child->next = nullptr;
    if (prev) prev->next = child.get(); else childHead = child.get();
    childTail = child.get();
    forEachChild(*this, [&](AnimTimer& c){
        const double childTLOffset = c.offset + c.delay;
        const double childDur = childTLOffset + c.duration;
        if (childDur > iterationDuration)
            iterationDuration = childDur;
    });
    duration = getTimelineTotalDuration(*this);
}

AnimTimeline& AnimTimeline::add(const AnimTargetList& targets, const AnimParam& params, const AnimParam& position){
    hasChildren = true;
    const AnimParam childParams = params.isObject() ? params : AnimParam::object();
    if (position.isFunction()){
        // one child per target, at the position the stagger function gives it
        const double tlDuration = duration;
        const double tlIterationDuration = iterationDuration;
        const AnimParam& idParam = childParams.get("id");
        AnimTargetList parsedTargets;
        for (const AnimTargetPtr& target : targets)
            AnimTargets::appendUnique(parsedTargets, target);
        for (size_t i = 0; i < parsedTargets.size(); i++){
            AnimParam staggeredChildParams = childParams;
            // each stagger starts from the timeline's duration before the group
            duration = tlDuration;
            iterationDuration = tlIterationDuration;
            if (!idParam.isNil())
                staggeredChildParams.set("id", idParam.toRaw().toString() + "-" + std::to_string(i));
            AnimFunctionArgs args;
            args.target = parsedTargets[i];
            args.index = (int)i;
            args.total = (int)parsedTargets.size();
            args.targets = &parsedTargets;
            args.timeline = this;
            const double staggeredTimePosition = parsePosition(position.asFunction()(args));
            AnimTargetList single = {parsedTargets[i]};
            addChild(staggeredChildParams, staggeredTimePosition, &single, (int)i, &parsedTargets);
        }
    } else {
        addChild(childParams, parsePosition(position), &targets);
    }
    if (composition)
        init(true);
    return *this;
}

AnimTimeline& AnimTimeline::add(const AnimParam& timerParams, const AnimParam& position){
    hasChildren = true;
    addChild(timerParams.isObject() ? timerParams : AnimParam::object(), parsePosition(position), nullptr);
    if (composition)
        init(true);
    return *this;
}

AnimTimeline& AnimTimeline::set(const AnimTargetList& targets, AnimParam params, const AnimParam& position){
    if (!params.isObject())
        return *this;
    params.set("duration", AnimConst::minValue);
    params.set("composition", "replace");
    return add(targets, params, position);
}

AnimTimeline& AnimTimeline::call(std::function<void(AnimTimer&)> fn, const AnimParam& position){
    if (!fn)
        return *this;
    AnimTimeline* self = this;
    AnimParam params = AnimParam::object({{"duration", 0}, {"delay", 0}});
    params.set("onComplete", AnimParam::callback([fn, self](AnimTimer&){ fn(*self); }));
    return add(params, position);
}

AnimTimeline& AnimTimeline::label(const std::string& name, const AnimParam& position){
    if (name.empty())
        return *this;
    labels[name] = parsePosition(position);
    return *this;
}

AnimTimeline& AnimTimeline::sync(const std::shared_ptr<AnimTimer>& synced, const AnimParam& position){
    if (!synced || synced.get() == this)
        return *this;
    synced->pause();
    const double syncedDuration = synced->getDuration();
    AnimParam params = AnimParam::object({
        {"currentTime", AnimParam::array({0, syncedDuration})},
        {"duration", syncedDuration},
        {"delay", 0},
        {"ease", "linear"},
        {"playbackEase", "linear"},
    });
    return add(AnimTargetList{AnimTargets::fromTimer(synced)}, params, position);
}

AnimTimeline& AnimTimeline::remove(const AnimTargetList& targets, const std::string& property){
    AnimComposer::removeTargets(targets, this, property);
    return *this;
}

AnimTimer& AnimTimeline::stretch(double newDuration){
    const double currentDuration = duration;
    if (currentDuration == AnimMath::normalizeTime(newDuration))
        return *this;
    const double timeScale = newDuration / currentDuration;
    forEachChild(*this, [&](AnimTimer& child){ child.stretch(child.duration * timeScale); });
    for (auto& entry : labels)
        entry.second *= timeScale;
    return AnimTimer::stretch(newDuration);
}

AnimTimer& AnimTimeline::refresh(){
    forEachChild(*this, [](AnimTimer& child){ child.refresh(); });
    return *this;
}

AnimTimer& AnimTimeline::revert(){
    AnimTimer::revert();
    AnimComposer::revertValues(*this);
    return *this;
}

bool AnimTimeline::hasOnlyInvalidTargets() const{
    if (!childHead)
        return false;
    for (AnimTimer* child = childHead; child; child = child->next){
        if (!child->hasOnlyInvalidTargets())
            return false;
    }
    return true;
}

double AnimTimeline::parsePosition(const AnimParam& position){
    double tlDuration = iterationDuration;
    if (tlDuration == AnimConst::minValue)
        tlDuration = 0;
    if (position.isNil())
        return tlDuration;
    const double numeric = position.toNumber();
    if (!std::isnan(numeric))
        return numeric;
    if (!position.isString())
        return tlDuration;
    const std::string& str = position.asString();

    // "<" (end of the previous child) and "<<" (its start)
    bool hasSibling = false;
    double prevOffset = 0;
    if (!str.empty() && str[0] == '<'){
        hasSibling = true;
        const bool goToPrevAnimationOffset = str.size() > 1 && str[1] == '<';
        AnimTimer* prevAnimation = childTail;
        prevOffset = prevAnimation ? prevAnimation->offset + prevAnimation->delay : 0;
        if (!goToPrevAnimationOffset)
            prevOffset += prevAnimation ? prevAnimation->duration : 0;
    }

    // the first relative operator (*=, +=, -=)
    size_t opPos = std::string::npos;
    for (size_t i = 0; i + 1 < str.size(); i++){
        if (str[i + 1] == '=' && (str[i] == '*' || str[i] == '+' || str[i] == '-')){
            opPos = i;
            break;
        }
    }
    if (opPos != std::string::npos){
        const std::string fullOperator = str.substr(opPos, 2);
        const std::string before = str.substr(0, opPos);
        std::string after = str.substr(opPos + 2);
        size_t again = after.find(fullOperator);
        if (again != std::string::npos)
            after = after.substr(0, again);
        double labelOffset = tlDuration;
        if (!before.empty()){
            auto it = labels.find(before);
            labelOffset = it != labels.end() ? it->second : std::nan("");
        }
        const double parsedOffset = hasSibling ? prevOffset : labelOffset;
        return AnimValue::relative(parsedOffset, AnimMath::toNumber(after), fullOperator[0]);
    }
    if (hasSibling)
        return prevOffset;
    auto it = labels.find(str);
    return it != labels.end() ? it->second : tlDuration;
}
