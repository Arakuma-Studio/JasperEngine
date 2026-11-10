// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 (MIT) animation/animation.js (JSAnimation), animation/composition.js,
// animation/additive.js and core/styles.js revertValues.

#include "AnimAnimation.h"

#include "AnimCommon.h"
#include "AnimEngine.h"
#include "AnimProperty.h"
#include "AnimSpring.h"
#include "AnimTimeline.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <unordered_map>

using namespace jasperengine;

namespace {

    uint64_t tweenId = 0;
    uint64_t animationId = 0;

    template<typename F>
    void forEachTween(AnimAnimation& animation, F&& fn, bool reverse = false){
        AnimTween* tween = reverse ? animation.tail : animation.head;
        while (tween){
            AnimTween* following = reverse ? tween->prev : tween->next;
            fn(*tween);
            tween = following;
        }
    }

    // addChild / removeChild on the animation's tween list
    void appendTween(AnimAnimation& animation, AnimTween* tween){
        AnimTween* prev = animation.tail;
        tween->prev = prev;
        tween->next = nullptr;
        if (prev) prev->next = tween; else animation.head = tween;
        animation.tail = tween;
        tween->inAnimationList = true;
    }

    void unlinkTween(AnimAnimation& animation, AnimTween* tween){
        if (!tween->inAnimationList)
            return;
        AnimTween* prev = tween->prev;
        AnimTween* next = tween->next;
        if (prev) prev->next = next; else animation.head = next;
        if (next) next->prev = prev; else animation.tail = prev;
        tween->prev = nullptr;
        tween->next = nullptr;
        tween->inAnimationList = false;
    }

    // Anime's isKey(): animated property names, not parameters
    bool isKey(const std::string& key){
        return !isAnimParameterKey(key);
    }

    AnimComposition compositionOf(const AnimParam& value){
        if (value.isNumber()){
            const int n = (int)value.asNumber();
            return n == 1 ? AnimComposition::NONE : n == 2 ? AnimComposition::BLEND : AnimComposition::REPLACE;
        }
        if (value.isString()){
            if (value.asString() == "none") return AnimComposition::NONE;
            if (value.asString() == "blend") return AnimComposition::BLEND;
            if (value.asString() != "replace")
                Log::warn("[HTML UI] anime: unknown composition '%s' (replace, none, blend), using 'replace'", value.asString().c_str());
        }
        return AnimComposition::REPLACE;
    }

    const std::regex& cssVariableMatchRgx(){
        static const std::regex r("var\\(\\s*(--[\\w-]+)(?:\\s*,\\s*([^)]+))?\\s*\\)", std::regex::ECMAScript);
        return r;
    }

    std::string trimmed(const std::string& text){
        size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return "";
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    AnimParam resolveCssVar(const std::string& value, const AnimTargetPtr& target){
        std::smatch m;
        if (!std::regex_search(value, m, cssVariableMatchRgx()))
            return AnimParam(0.0);
        std::string computed = target ? target->resolveCssVariable(m[1].str()).toString() : std::string();
        if (trimmed(computed).empty() && m[2].matched)
            computed = trimmed(m[2].str());
        return computed.empty() ? AnimParam(0.0) : AnimParam(computed);
    }

    // Anime's `!isNaN(+computed) ? +computed : computed || 0`
    AnimParam coerceFunctionResult(const AnimParam& computed){
        const double n = computed.toNumber();
        if (!std::isnan(n))
            return AnimParam(n);
        return computed.isTruthy() ? computed : AnimParam(0.0);
    }

    // getFunctionValue; store (when given) receives a re-evaluation for refresh()
    AnimParam functionValue(const AnimParam& value, const AnimTargetPtr& target, int index, const AnimTargetList& targets,
                            std::function<AnimRawValue()>* store, AnimTimeline* timeline = nullptr, const AnimRawValue& previous = AnimRawValue()){
        if (value.isFunction()){
            AnimFunctionArgs args;
            args.target = target;
            args.index = index;
            args.total = (int)targets.size();
            args.targets = &targets;
            args.timeline = timeline;
            args.previousValue = previous;
            AnimParam::Function fn = value.asFunction();
            if (store){
                AnimTargetList targetsCopy = targets;
                *store = [fn, target, index, targetsCopy](){
                    AnimFunctionArgs a;
                    a.target = target;
                    a.index = index;
                    a.total = (int)targetsCopy.size();
                    a.targets = &targetsCopy;
                    return coerceFunctionResult(fn(a)).toRaw();
                };
            }
            return coerceFunctionResult(fn(args));
        }
        if (value.isString() && value.asString().rfind("var(", 0) == 0){
            if (store){
                std::string text = value.asString();
                AnimTargetPtr t = target;
                *store = [text, t](){ return resolveCssVar(text, t).toRaw(); };
            }
            return resolveCssVar(value.asString(), target);
        }
        return value;
    }

    // setValue(value, default) with nil = undefined
    const AnimParam& orDefault(const AnimParam& value, const AnimParam& fallback){
        return value.isNil() ? fallback : value;
    }

    EaseFn easeOf(const AnimParam& ease){
        switch (ease.getType()){
            case AnimParam::Type::EASE: return ease.asEase() ? ease.asEase() : AnimEasing::none();
            case AnimParam::Type::STRING: return AnimEasing::parse(ease.asString());
            case AnimParam::Type::SPRING: return ease.asSpring() ? ease.asSpring()->getEaseFunction() : AnimEasing::none();
            default: return AnimEasing::none();
        }
    }

    // generateKeyframes: keyframes (duration or percentage) into per-property keyframe arrays
    AnimParam generateKeyframes(const AnimParam& keyframes, const AnimParam& parameters){
        AnimParam properties = AnimParam::object();
        if (keyframes.isArray()){
            std::vector<std::string> propertyNames;
            for (const AnimParam& key : keyframes.getItems()){
                for (const AnimParam::Member& member : key.getMembers()){
                    if (isKey(member.first))
                        propertyNames.push_back(member.first);
                }
            }
            for (const std::string& propName : propertyNames){
                AnimParam propArray = AnimParam::array();
                for (const AnimParam& key : keyframes.getItems()){
                    AnimParam newKey = AnimParam::object();
                    for (const AnimParam::Member& member : key.getMembers()){
                        if (isKey(member.first)){
                            if (member.first == propName)
                                newKey.set("to", member.second);
                        } else {
                            newKey.set(member.first, member.second);
                        }
                    }
                    propArray.push(newKey);
                }
                properties.set(propName, propArray);
            }
        } else if (keyframes.isObject()){
            const double totalDuration = orDefault(parameters.get("duration"), AnimDefaults::global().get("duration")).toNumber();
            struct Key { double o; const AnimParam* p; };
            std::vector<Key> keys;
            for (const AnimParam::Member& member : keyframes.getMembers())
                keys.push_back(Key{AnimMath::parseFloat(member.first) / 100, &member.second});
            std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b){ return a.o < b.o; });
            for (const Key& key : keys){
                const double offset = key.o;
                const AnimParam& prop = *key.p;
                for (const AnimParam::Member& member : prop.getMembers()){
                    const std::string& name = member.first;
                    if (!isKey(name))
                        continue;
                    if (!properties.has(name))
                        properties.set(name, AnimParam::array());
                    AnimParam propArray = properties.get(name);
                    const double duration = offset * totalDuration;
                    const size_t length = propArray.size();
                    AnimParam keyObj = AnimParam::object({{"to", member.second}});
                    double durProgress = 0;
                    for (size_t i = 0; i < length; i++)
                        durProgress += propArray.at(i).get("duration").toNumber();
                    if (length == 1)
                        keyObj.set("from", propArray.at(0).get("to"));
                    if (prop.has("ease"))
                        keyObj.set("ease", prop.get("ease"));
                    keyObj.set("duration", duration - (length ? durProgress : 0));
                    propArray.push(keyObj);
                    properties.set(name, propArray);
                }
            }
            // the easing of a keyframe applies to the next segment, as in WAAPI
            AnimParam result = AnimParam::object();
            for (const AnimParam::Member& member : properties.getMembers()){
                std::vector<AnimParam> propArray = member.second.getItems();
                AnimParam prevEase;
                for (AnimParam& key : propArray){
                    AnimParam currentEase = key.get("ease");
                    if (!prevEase.isNil()) key.set("ease", prevEase); else key.erase("ease");
                    prevEase = currentEase;
                }
                if (!propArray.empty() && !propArray[0].get("duration").isTruthy())
                    propArray.erase(propArray.begin());
                result.set(member.first, AnimParam::array(propArray));
            }
            properties = result;
        }
        return properties;
    }

    // mergeObjects(o1, o2): o1's members, then o2's missing ones
    AnimParam mergeObjects(const AnimParam& o1, const AnimParam& o2){
        AnimParam merged = o1;
        for (const AnimParam::Member& member : o2.getMembers()){
            if (!o1.has(member.first))
                merged.set(member.first, member.second);
        }
        return merged;
    }

    // composeComplexValue
    std::string composeComplexValue(AnimTween& tween, double progress, int precision){
        const std::vector<double>& fn = tween.fromNumbers;
        const std::vector<double>& tn = tween.toNumbers;
        const std::vector<std::string>& ts = tween.strings;
        std::string v = ts.empty() ? std::string() : ts[0];
        if (tween.numbers.size() < tn.size())
            tween.numbers.resize(tn.size(), 0.0);
        for (size_t j = 0; j < tn.size(); j++){
            const double from = j < fn.size() ? fn[j] : 0.0;
            const double n = tween.modify(AnimMath::round(AnimMath::lerp(from, tn[j], progress), precision));
            const std::string s = j + 1 < ts.size() ? ts[j + 1] : std::string();
            v += AnimMath::toString(n) + s;
            tween.numbers[j] = n;
        }
        return v;
    }

    AnimEngine& engine(){
        return AnimEngine::instance();
    }

}

// ---------------------------------------------------------------------------------------------
// AnimAnimation

std::shared_ptr<AnimAnimation> AnimAnimation::create(const AnimTargetList& targets, const AnimParam& params){
    std::shared_ptr<AnimAnimation> animation = std::make_shared<AnimAnimation>(targets, params, nullptr, 0);
    if (animation->pendingCreateRender && animation->onRender)
        animation->onRender(*animation);
    animation->pendingCreateRender = false;
    animation->init();
    return animation;
}

std::shared_ptr<AnimAnimation> AnimAnimation::set(const AnimTargetList& targets, AnimParam params){
    if (!params.isObject())
        params = AnimParam::object();
    params.set("duration", AnimConst::minValue);
    // active tweens are not overridden by default
    if (!params.has("composition"))
        params.set("composition", "none");
    std::shared_ptr<AnimAnimation> animation = std::make_shared<AnimAnimation>(targets, params, nullptr, 0, true);
    animation->resume();
    return animation;
}

AnimAnimation::AnimAnimation(const AnimTargetList& parsedTargets, const AnimParam& parameters, AnimTimeline* parent,
                             double parentPosition, bool fastSet, int index, const AnimTargetList* allTargets):
        AnimTimer(Kind::ANIMATION, parameters, parent, parentPosition){
    ++animationId;
    AnimEngine::registerAnimation(this, true);

    for (const AnimTargetPtr& target : parsedTargets){
        if (target)
            AnimTargets::appendUnique(targets, target);
    }
    hasTargets = !targets.empty();
    const int targetsLength = (int)targets.size();

    const AnimParam& kfParams = parameters.get("keyframes");
    const AnimParam params = !kfParams.isNil() ? mergeObjects(generateKeyframes(kfParams, parameters), parameters) : parameters;

    const AnimParam& idParam = params.get("id");
    const AnimParam& delayParam = params.get("delay");
    const AnimParam& durationParam = params.get("duration");
    const AnimParam& easeParam = params.get("ease");
    const AnimParam& playbackEaseParam = params.get("playbackEase");
    const AnimParam& modifierParam = params.get("modifier");
    const AnimParam& compositionParam = params.get("composition");

    const AnimParam& animDefaults = AnimDefaults::of(parent);
    const AnimParam& animEase = orDefault(easeParam, animDefaults.get("ease"));
    const AnimParam& animPlaybackEase = orDefault(playbackEaseParam, animDefaults.get("playbackEase"));
    EaseFn parsedAnimPlaybackEase = !animPlaybackEase.isNil() ? easeOf(animPlaybackEase) : nullptr;
    const bool hasSpring = animEase.isSpring() && animEase.asSpring();
    const AnimParam tEasing = hasSpring ? AnimParam::ease(animEase.asSpring()->getEaseFunction())
                                        : (easeParam.isNil() ? (parsedAnimPlaybackEase ? AnimParam("linear") : animDefaults.get("ease")) : easeParam);
    const AnimParam tDuration = hasSpring ? AnimParam(animEase.asSpring()->getSettlingDuration()) : orDefault(durationParam, animDefaults.get("duration"));
    const AnimParam& tDelay = orDefault(delayParam, animDefaults.get("delay"));
    const AnimParam& tModifier = !modifierParam.isNil() ? modifierParam : animDefaults.get("modifier");
    // many targets (>= 1000) and no composition given: none, for faster tween creation
    const AnimParam tComposition = compositionParam.isNil() && targetsLength >= AnimConst::K ? AnimParam("none")
                                 : !compositionParam.isNil() ? compositionParam : animDefaults.get("composition");
    const double absoluteOffsetTime = offset + (parent ? parent->offset : 0);
    // the spring's onComplete receives this animation
    if (hasSpring)
        animEase.asSpring()->setParent(this);

    double iterationDurationValue = std::nan("");
    double iterationDelay = std::nan("");
    bool shouldTriggerRender = false;
    std::unordered_map<std::string, AnimRawValue> inlineStylesStore;

    for (int targetIndex = 0; targetIndex < targetsLength; targetIndex++){
        const AnimTargetPtr& target = targets[targetIndex];
        const int ti = index ? index : targetIndex;
        const AnimTargetList& tl = allTargets ? *allTargets : targets;

        for (const AnimParam::Member& member : params.getMembers()){
            const std::string& p = member.first;
            if (!isKey(p))
                continue;

            const AnimTweenType tweenType = target->getTweenType(p);
            const std::string propName = target->sanitizeProperty(p, tweenType);

            AnimParam propValue = member.second;
            const bool isPropValueArray = propValue.isArray();

            if (fastSet && !isPropValueArray)
                propValue = AnimParam::array({propValue, propValue});

            // normalized to keyframes: [x, y] -> [{to: [x, y]}], [x, y, z] -> [[x, y], z], x -> [x]
            std::vector<AnimParam> keyframes;
            if (isPropValueArray){
                const size_t arrayLength = propValue.size();
                const bool isNotObjectValue = !propValue.at(0).isObject();
                if (arrayLength == 2 && isNotObjectValue){
                    keyframes.push_back(AnimParam::object({{"to", propValue}}));
                } else if (arrayLength > 2 && isNotObjectValue){
                    for (size_t i = 0; i < arrayLength; i++){
                        if (i == 1){
                            keyframes.push_back(AnimParam::array({propValue.at(0), propValue.at(1)}));
                        } else if (i > 1){
                            keyframes.push_back(propValue.at(i));
                        }
                    }
                } else {
                    keyframes = propValue.getItems();
                }
            } else {
                keyframes.push_back(propValue);
            }

            AnimTweenSiblings* siblings = nullptr;
            AnimTween* prevTween = nullptr;
            double firstTweenChangeStartTime = std::nan("");
            double lastTweenChangeEndTime = 0;
            int tweenIndex = 0;
            const int l = (int)keyframes.size();

            for (; tweenIndex < l; tweenIndex++){
                const AnimParam& keyframe = keyframes[tweenIndex];
                AnimParam key = keyframe.isObject() ? keyframe : AnimParam::object({{"to", keyframe}});

                std::function<AnimRawValue()> toFunc, fromFunc;

                const AnimParam computedComposition = functionValue(orDefault(key.get("composition"), tComposition), target, ti, tl, nullptr);
                const AnimComposition tweenComposition = compositionOf(computedComposition);
                if (!siblings && tweenComposition != AnimComposition::NONE)
                    siblings = &AnimComposer::getSiblings(*target, propName, false);
                const AnimParam computedToValue = functionValue(key.get("to"), target, ti, tl, &toFunc, nullptr, prevTween ? prevTween->value : AnimRawValue());

                AnimParam tweenToValue;
                // function values may return the object syntax ({to: v})
                if (computedToValue.isObject() && computedToValue.has("to")){
                    key = computedToValue;
                    tweenToValue = computedToValue.get("to");
                } else {
                    tweenToValue = computedToValue;
                }
                const AnimParam tweenFromValue = functionValue(key.get("from"), target, ti, tl, &fromFunc);
                const AnimParam easeToParse = key.get("ease").isTruthy() ? key.get("ease") : tEasing;

                AnimParam keyEasing = easeToParse;
                if (easeToParse.isFunction()){
                    AnimParam result = functionValue(easeToParse, target, ti, tl, nullptr);
                    if (result.isString() || result.isEase() || result.isSpring())
                        keyEasing = result;
                }

                const bool keyHasSpring = keyEasing.isSpring() && keyEasing.asSpring();
                const EaseFn tweenEasing = keyHasSpring ? keyEasing.asSpring()->getEaseFunction() : easeOf(keyEasing);
                double tweenDuration;
                if (keyHasSpring){
                    tweenDuration = keyEasing.asSpring()->getSettlingDuration();
                } else {
                    AnimParam durationDefault = l > 1 ? AnimParam(functionValue(tDuration, target, ti, tl, nullptr).toNumber() / l) : tDuration;
                    tweenDuration = functionValue(orDefault(key.get("duration"), durationDefault), target, ti, tl, nullptr).toNumber();
                }
                // the default delay only applies to the first keyframe
                const double tweenDelay = functionValue(orDefault(key.get("delay"), !tweenIndex ? tDelay : AnimParam(0.0)), target, ti, tl, nullptr).toNumber();
                // modifiers are not function values
                const AnimParam& keyModifier = key.get("modifier");
                const EaseFn tweenModifier = keyModifier.isEase() ? keyModifier.asEase() : (tModifier.isEase() ? tModifier.asEase() : nullptr);
                const bool hasFromvalue = !tweenFromValue.isNil();
                const bool hasToValue = !tweenToValue.isNil();
                const bool isFromToArray = tweenToValue.isArray();
                const bool isFromToValue = isFromToArray || (hasFromvalue && hasToValue);
                // the update start in local time: the previous keyframe's end, 0 for the first one
                const double tweenUpdateStartLocal = prevTween ? lastTweenChangeEndTime : 0;
                const double tweenStartTime = prevTween ? lastTweenChangeEndTime + tweenDelay : tweenDelay;
                // rounded to keep seconds-mode float errors out of the comparisons
                const double absoluteStartTime = AnimMath::round(absoluteOffsetTime + tweenStartTime, 12);
                const double absoluteUpdateStartTime = AnimMath::round(absoluteOffsetTime + tweenUpdateStartLocal, 12);

                // an animation with a from value renders once even without autoplay
                if (!shouldTriggerRender && (hasFromvalue || isFromToArray))
                    shouldTriggerRender = true;

                AnimTween* prevSibling = prevTween;

                if (tweenComposition != AnimComposition::NONE){
                    AnimTween* nextSibling = siblings->head;
                    // the latest live sibling before the new tween; later ones are overridden
                    while (nextSibling && nextSibling->absoluteStartTime <= absoluteStartTime){
                        if (!nextSibling->isOverridden)
                            prevSibling = nextSibling;
                        nextSibling = nextSibling->nextRep;
                        if (nextSibling && nextSibling->absoluteStartTime >= absoluteStartTime){
                            while (nextSibling){
                                AnimComposer::overrideTween(*nextSibling);
                                nextSibling = nextSibling->nextRep;
                            }
                        }
                    }
                }

                // decompose the values
                AnimDecomposed fromTarget, toTarget;
                auto originalValue = [&](){
                    AnimRawValue inlineValue;
                    AnimRawValue value = target->getValue(propName, tweenType, &inlineValue);
                    if (!inlineValue.isNil())
                        inlineStylesStore[propName] = inlineValue;
                    return value;
                };
                auto tweenValue = [](const AnimTween& tween){
                    AnimDecomposed d;
                    d.t = tween.valueType;
                    d.n = tween.toNumber;
                    d.u = tween.unit;
                    d.o = 0;
                    if (tween.valueType == AnimValueType::COLOR || tween.valueType == AnimValueType::COMPLEX){
                        d.d = tween.toNumbers;
                        d.s = tween.strings;
                    }
                    return d;
                };
                const bool chainsSibling = parent && prevSibling && prevSibling->parent && prevSibling->parent->parent == parent;
                if (isFromToValue){
                    fromTarget = AnimValue::decompose(isFromToArray ? functionValue(tweenToValue.at(0), target, ti, tl, &fromFunc).toRaw() : tweenFromValue.toRaw());
                    toTarget = AnimValue::decompose(isFromToArray ? functionValue(tweenToValue.at(1), target, ti, tl, &toFunc).toRaw() : tweenToValue.toRaw());
                    // registers the inline style
                    const AnimRawValue original = originalValue();
                    if (fromTarget.t == AnimValueType::NUMBER){
                        if (prevSibling){
                            if (prevSibling->valueType == AnimValueType::UNIT){
                                fromTarget.t = AnimValueType::UNIT;
                                fromTarget.u = prevSibling->unit;
                            }
                        } else {
                            AnimDecomposed decomposedOriginal = AnimValue::decompose(original);
                            if (decomposedOriginal.t == AnimValueType::UNIT){
                                fromTarget.t = AnimValueType::UNIT;
                                fromTarget.u = decomposedOriginal.u;
                            }
                        }
                    }
                } else {
                    if (hasToValue){
                        toTarget = AnimValue::decompose(tweenToValue.toRaw());
                    } else if (prevTween){
                        toTarget = tweenValue(*prevTween);
                    } else {
                        toTarget = AnimValue::decompose(chainsSibling ? prevSibling->value : originalValue());
                    }
                    if (hasFromvalue){
                        fromTarget = AnimValue::decompose(tweenFromValue.toRaw());
                    } else if (prevTween){
                        fromTarget = tweenValue(*prevTween);
                    } else {
                        fromTarget = AnimValue::decompose(chainsSibling ? prevSibling->value : originalValue());
                    }
                }

                // relative operators
                if (fromTarget.o){
                    fromTarget.n = AnimValue::relative(!prevSibling ? AnimValue::decompose(originalValue()).n : prevSibling->toNumber,
                                                       fromTarget.n, fromTarget.o);
                }
                if (toTarget.o){
                    toTarget.n = AnimValue::relative(fromTarget.n, toTarget.n, toTarget.o);
                }

                // homogenize the value types of from and to
                if (fromTarget.t != toTarget.t){
                    if (fromTarget.t == AnimValueType::COMPLEX || toTarget.t == AnimValueType::COMPLEX){
                        AnimDecomposed& complexValue = fromTarget.t == AnimValueType::COMPLEX ? fromTarget : toTarget;
                        AnimDecomposed& notComplexValue = fromTarget.t == AnimValueType::COMPLEX ? toTarget : fromTarget;
                        notComplexValue.t = AnimValueType::COMPLEX;
                        notComplexValue.s = complexValue.s;
                        notComplexValue.d = std::vector<double>(complexValue.d ? complexValue.d->size() : 0, notComplexValue.n);
                    } else if (fromTarget.t == AnimValueType::UNIT || toTarget.t == AnimValueType::UNIT){
                        AnimDecomposed& unitValue = fromTarget.t == AnimValueType::UNIT ? fromTarget : toTarget;
                        AnimDecomposed& notUnitValue = fromTarget.t == AnimValueType::UNIT ? toTarget : fromTarget;
                        notUnitValue.t = AnimValueType::UNIT;
                        notUnitValue.u = unitValue.u;
                    } else if (fromTarget.t == AnimValueType::COLOR || toTarget.t == AnimValueType::COLOR){
                        AnimDecomposed& colorValue = fromTarget.t == AnimValueType::COLOR ? fromTarget : toTarget;
                        AnimDecomposed& notColorValue = fromTarget.t == AnimValueType::COLOR ? toTarget : fromTarget;
                        notColorValue.t = AnimValueType::COLOR;
                        notColorValue.d = std::vector<double>(colorValue.d ? colorValue.d->size() : 0, 0.0);
                    }
                }

                // unit conversion
                if (fromTarget.u != toTarget.u){
                    AnimDecomposed& valueToConvert = !toTarget.u.empty() ? fromTarget : toTarget;
                    target->convertUnit(valueToConvert, !toTarget.u.empty() ? toTarget.u : fromTarget.u, false);
                }

                // complex values of different lengths
                if (toTarget.d && fromTarget.d && toTarget.d->size() != fromTarget.d->size()){
                    AnimDecomposed& longestValue = fromTarget.d->size() > toTarget.d->size() ? fromTarget : toTarget;
                    AnimDecomposed& shortestValue = &longestValue == &fromTarget ? toTarget : fromTarget;
                    std::vector<double> filled(longestValue.d->size(), 0.0);
                    for (size_t i = 0; i < filled.size(); i++)
                        filled[i] = i < shortestValue.d->size() ? (*shortestValue.d)[i] : 0.0;
                    shortestValue.d = filled;
                    shortestValue.s = longestValue.s;
                }

                // the tween
                double durationNumber = tweenDuration;
                const double tweenUpdateDuration = AnimMath::round(durationNumber && !std::isnan(durationNumber) ? durationNumber : AnimConst::minValue, 12);

                AnimRawValue inlineValue;
                auto inlineIt = inlineStylesStore.find(propName);
                if (inlineIt != inlineStylesStore.end()){
                    inlineValue = inlineIt->second;
                    inlineStylesStore.erase(inlineIt);
                }

                lastTweenChangeEndTime = AnimMath::round(tweenStartTime + tweenUpdateDuration, 12);

                std::unique_ptr<AnimTween> owned(new AnimTween());
                AnimTween& tween = *owned;
                tween.parent = this;
                tween.id = tweenId++;
                tween.property = propName;
                tween.target = target;
                tween.toFunc = toFunc;
                tween.fromFunc = fromFunc;
                tween.ease = tweenEasing;
                tween.fromNumbers = fromTarget.d ? *fromTarget.d : std::vector<double>();
                tween.toNumbers = toTarget.d ? *toTarget.d : std::vector<double>();
                tween.strings = toTarget.s ? *toTarget.s : std::vector<std::string>();
                tween.fromNumber = fromTarget.n;
                tween.toNumber = toTarget.n;
                tween.numbers = fromTarget.d ? *fromTarget.d : std::vector<double>();
                tween.number = fromTarget.n;
                tween.unit = toTarget.u;
                tween.modifier = tweenModifier;
                tween.currentTime = 0;
                tween.startTime = tweenStartTime;
                tween.delay = tweenDelay;
                tween.updateDuration = tweenUpdateDuration;
                tween.changeDuration = tweenUpdateDuration;
                tween.absoluteStartTime = absoluteStartTime;
                tween.absoluteUpdateStartTime = absoluteUpdateStartTime;
                tween.absoluteEndTime = AnimMath::round(absoluteOffsetTime + lastTweenChangeEndTime, 12);
                tween.hasFromValue = hasFromvalue || isFromToArray ? 1 : 0;
                tween.tweenType = tweenType;
                tween.valueType = toTarget.t;
                tween.composition = tweenComposition;
                tween.inlineValue = inlineValue;

                if (tweenComposition != AnimComposition::NONE)
                    AnimComposer::composeTween(tween, *siblings);

                // the end value, for function values chaining keyframes and timeline children
                if (tween.valueType == AnimValueType::COMPLEX){
                    tween.value = AnimRawValue(composeComplexValue(tween, 1, -1));
                } else if (tween.valueType == AnimValueType::UNIT){
                    tween.value = AnimRawValue(AnimMath::toString(tween.modify(tween.toNumber)) + tween.unit);
                } else if (tween.valueType == AnimValueType::COLOR){
                    const std::vector<double>& d = *toTarget.d;
                    tween.value = AnimRawValue(AnimValue::colorToRgbaString({d[0], d[1], d[2], d[3]}));
                } else {
                    tween.value = AnimRawValue(tween.modify(tween.toNumber));
                }

                if (std::isnan(firstTweenChangeStartTime))
                    firstTweenChangeStartTime = tween.startTime;

                prevTween = &tween;

                appendTween(*this, &tween);
                ownedTweens.push_back(std::move(owned));
            }

            // the animation timings from the added tweens
            if (std::isnan(iterationDelay) || firstTweenChangeStartTime < iterationDelay)
                iterationDelay = firstTweenChangeStartTime;
            if (std::isnan(iterationDurationValue) || lastTweenChangeEndTime > iterationDurationValue)
                iterationDurationValue = lastTweenChangeEndTime;
        }
    }

    if (!targetsLength){
        Log::warn("[HTML UI] anime: no target found. Make sure the element you animate exists (its document is loaded) when the animation is created");
    }

    if (iterationDelay && !std::isnan(iterationDelay)){
        forEachTween(*this, [&](AnimTween& tween){
            // a tween at the very start trims its delay too
            if (!(tween.startTime - tween.delay))
                tween.delay -= iterationDelay;
            tween.startTime -= iterationDelay;
        });
        iterationDurationValue -= iterationDelay;
    } else {
        iterationDelay = 0;
    }

    // no valid property: a setter
    if (!iterationDurationValue || std::isnan(iterationDurationValue)){
        iterationDurationValue = AnimConst::minValue;
        iterationCount = 0;
    }
    id = !idParam.isNil() ? idParam : AnimParam((double)animationId);
    duration = iterationDurationValue == AnimConst::minValue ? AnimConst::minValue
             : AnimMath::clampInfinity(((iterationDurationValue + loopDelay) * iterationCount) - loopDelay);
    if (!duration || std::isnan(duration))
        duration = AnimConst::minValue;
    const AnimParam& onRenderParam = params.get("onRender");
    onRender = onRenderParam.isCallback() ? onRenderParam.asCallback()
             : (animDefaults.get("onRender").isCallback() ? animDefaults.get("onRender").asCallback() : nullptr);
    ease = parsedAnimPlaybackEase;
    delay = iterationDelay;
    iterationDuration = iterationDurationValue;

    // Anime calls onRender here when autoplay is off and a value has a from: the creator
    // (create(), AnimTimeline) does it once the object is owned
    pendingCreateRender = !autoplay && shouldTriggerRender;
}

AnimAnimation::~AnimAnimation(){
    AnimEngine::registerAnimation(this, false);
    for (const std::unique_ptr<AnimTween>& tween : ownedTweens)
        AnimComposer::removeTweenSiblings(*tween);
}

bool AnimAnimation::hasOnlyInvalidTargets() const{
    if (!hasTargets)
        return false;
    for (const AnimTargetPtr& target : targets){
        if (target->isValid())
            return false;
    }
    return true;
}

AnimTimer& AnimAnimation::stretch(double newDuration){
    const double currentDuration = duration;
    if (currentDuration == AnimMath::normalizeTime(newDuration))
        return *this;
    const double timeScale = newDuration / currentDuration;
    forEachTween(*this, [&](AnimTween& tween){
        tween.updateDuration = AnimMath::normalizeTime(tween.updateDuration * timeScale);
        tween.changeDuration = AnimMath::normalizeTime(tween.changeDuration * timeScale);
        tween.currentTime *= timeScale;
        tween.delay *= timeScale;
        tween.startTime *= timeScale;
        tween.absoluteStartTime *= timeScale;
        tween.absoluteUpdateStartTime *= timeScale;
        tween.absoluteEndTime *= timeScale;
    });
    return AnimTimer::stretch(newDuration);
}

AnimTimer& AnimAnimation::refresh(){
    forEachTween(*this, [&](AnimTween& tween){
        if (!tween.toFunc && !tween.fromFunc)
            return;
        if (tween.fromFunc){
            AnimDecomposed from = AnimValue::decompose(tween.fromFunc());
            if (from.u != tween.unit && tween.target->getKind() == AnimTarget::Kind::ELEMENT)
                tween.target->convertUnit(from, tween.unit, true);
            tween.fromNumbers = from.d ? *from.d : std::vector<double>();
            tween.fromNumber = from.n;
        } else if (tween.toFunc){
            AnimDecomposed original = AnimValue::decompose(tween.target->getValue(tween.property, tween.tweenType, nullptr));
            tween.fromNumbers = original.d ? *original.d : std::vector<double>();
            tween.fromNumber = original.n;
        }
        if (tween.toFunc){
            AnimDecomposed to = AnimValue::decompose(tween.toFunc());
            tween.toNumbers = to.d ? *to.d : std::vector<double>();
            tween.strings = to.s ? *to.s : std::vector<std::string>();
            // relative operators apply again (anime issue #1025)
            tween.toNumber = to.o ? AnimValue::relative(tween.fromNumber, to.n, to.o) : to.n;
        }
    });
    // setters render once
    if (duration == AnimConst::minValue)
        restart();
    return *this;
}

AnimTimer& AnimAnimation::revert(){
    AnimTimer::revert();
    AnimComposer::revertValues(*this);
    return *this;
}

// ---------------------------------------------------------------------------------------------
// composition

AnimTweenSiblings& AnimComposer::getSiblings(AnimTarget& target, const std::string& property, bool additive){
    return additive ? target.additiveSiblings[property] : target.replaceSiblings[property];
}

void AnimComposer::overrideTween(AnimTween& tween){
    tween.isOverlapped = 1;
    tween.isOverridden = 1;
    tween.changeDuration = AnimConst::minValue;
    tween.currentTime = AnimConst::minValue;
}

namespace {

    // addChild(siblings, tween, addTweenSortMethod, '_prevRep', '_nextRep')
    void addReplaceSibling(AnimTweenSiblings& siblings, AnimTween& tween){
        AnimTween* prev = siblings.tail;
        while (prev && (prev->isOverridden || prev->absoluteStartTime > tween.absoluteStartTime))
            prev = prev->prevRep;
        AnimTween* next = prev ? prev->nextRep : siblings.head;
        if (prev) prev->nextRep = &tween; else siblings.head = &tween;
        if (next) next->prevRep = &tween; else siblings.tail = &tween;
        tween.prevRep = prev;
        tween.nextRep = next;
        tween.inReplaceList = true;
    }

    void removeReplaceSibling(AnimTweenSiblings& siblings, AnimTween& tween){
        AnimTween* prev = tween.prevRep;
        AnimTween* next = tween.nextRep;
        if (prev) prev->nextRep = next; else siblings.head = next;
        if (next) next->prevRep = prev; else siblings.tail = prev;
        tween.prevRep = nullptr;
        tween.nextRep = nullptr;
        tween.inReplaceList = false;
    }

    void addAdditiveSibling(AnimTweenSiblings& siblings, AnimTween& tween){
        AnimTween* prev = siblings.tail;
        if (prev) prev->nextAdd = &tween; else siblings.head = &tween;
        siblings.tail = &tween;
        tween.prevAdd = prev;
        tween.nextAdd = nullptr;
        tween.inAdditiveList = true;
    }

    void removeAdditiveSibling(AnimTweenSiblings& siblings, AnimTween& tween){
        AnimTween* prev = tween.prevAdd;
        AnimTween* next = tween.nextAdd;
        if (prev) prev->nextAdd = next; else siblings.head = next;
        if (next) next->prevAdd = prev; else siblings.tail = prev;
        tween.prevAdd = nullptr;
        tween.nextAdd = nullptr;
        tween.inAdditiveList = false;
    }

    void forEachTimelineChild(AnimTimeline& timeline, const std::function<void(AnimTimer&)>& fn){
        AnimTimer* child = timeline.childHead;
        while (child){
            AnimTimer* next = child->next;
            fn(*child);
            child = next;
        }
    }

}

void AnimComposer::composeTween(AnimTween& tween, AnimTweenSiblings& siblings){
    const AnimComposition tweenCompositionType = tween.composition;

    if (tweenCompositionType == AnimComposition::REPLACE){
        const double tweenAbsStartTime = tween.absoluteStartTime;

        addReplaceSibling(siblings, tween);

        AnimTween* prevSibling = tween.prevRep;

        if (prevSibling){
            AnimAnimation* prevParent = prevSibling->parent;
            const double prevAbsEndTime = prevSibling->absoluteEndTime;

            // looped animations
            if (prevParent && tween.parent && !(tween.parent->getId().getType() == prevParent->getId().getType() &&
                                                 tween.parent->getId().toRaw() == prevParent->getId().toRaw()) &&
                prevParent->iterationCount > 1 &&
                prevAbsEndTime + (prevParent->duration - prevParent->iterationDuration) > tweenAbsStartTime){
                overrideTween(*prevSibling);
                AnimTween* prevPrevSibling = prevSibling->prevRep;
                // the keyframes of the same animation too
                while (prevPrevSibling && prevPrevSibling->parent == prevParent){
                    overrideTween(*prevPrevSibling);
                    prevPrevSibling = prevPrevSibling->prevRep;
                }
            }

            // the precision-matched update start: no phantom overlap between touching keyframes
            const double absoluteUpdateStartTime = tween.absoluteUpdateStartTime;

            if (prevAbsEndTime > absoluteUpdateStartTime){
                const double prevChangeStartTime = prevSibling->startTime;
                const double prevTLOffset = prevAbsEndTime - (prevChangeStartTime + prevSibling->updateDuration);
                const double updatedPrevChangeDuration = AnimMath::round(absoluteUpdateStartTime - prevTLOffset - prevChangeStartTime, 12);

                prevSibling->changeDuration = updatedPrevChangeDuration;
                prevSibling->currentTime = updatedPrevChangeDuration;
                prevSibling->isOverlapped = 1;

                if (updatedPrevChangeDuration < AnimConst::minValue)
                    overrideTween(*prevSibling);
            }

            // a timeline cannot replace itself
            AnimTimeline* tweenParentTL = tween.parent ? tween.parent->parent : nullptr;
            if (prevParent && (!tweenParentTL || tweenParentTL != prevParent->parent)){
                bool pausePrevParentAnimation = true;
                for (AnimTween* t = prevParent->head; t; t = t->next){
                    if (!t->isOverlapped) pausePrevParentAnimation = false;
                }
                if (pausePrevParentAnimation){
                    AnimTimeline* prevParentTL = prevParent->parent;
                    if (prevParentTL){
                        bool pausePrevParentTL = true;
                        forEachTimelineChild(*prevParentTL, [&](AnimTimer& a){
                            if (&a != prevParent && a.kind == AnimTimer::Kind::ANIMATION){
                                for (AnimTween* t = static_cast<AnimAnimation&>(a).head; t; t = t->next){
                                    if (!t->isOverlapped) pausePrevParentTL = false;
                                }
                            }
                        });
                        if (pausePrevParentTL)
                            prevParentTL->cancel();
                    } else {
                        prevParent->cancel();
                    }
                }
            }
        }

    } else if (tweenCompositionType == AnimComposition::BLEND){
        AnimTweenSiblings& additiveTweenSiblings = getSiblings(*tween.target, tween.property, true);
        AnimEngine& e = engine();

        AnimTween* lookupTween = additiveTweenSiblings.head;

        if (!lookupTween){
            std::unique_ptr<AnimTween> lookup(new AnimTween(tween));
            lookup->parent = nullptr;
            lookup->composition = AnimComposition::REPLACE;
            lookup->updateDuration = AnimConst::minValue;
            lookup->startTime = 0;
            lookup->numbers = tween.fromNumbers;
            lookup->number = 0;
            lookup->prev = lookup->next = nullptr;
            lookup->prevRep = lookup->nextRep = nullptr;
            lookup->prevAdd = lookup->nextAdd = nullptr;
            lookup->inReplaceList = false;
            lookup->inAnimationList = false;
            lookupTween = lookup.get();
            additiveTweenSiblings.head = lookupTween;
            additiveTweenSiblings.tail = lookupTween;
            lookupTween->inAdditiveList = true;
            // the additive animation's list
            lookupTween->prev = e.additiveTail;
            if (e.additiveTail) e.additiveTail->next = lookupTween; else e.additiveHead = lookupTween;
            e.additiveTail = lookupTween;
            e.additiveTweens.push_back(std::move(lookup));
            if (std::find(e.additiveTargets.begin(), e.additiveTargets.end(), tween.target.get()) == e.additiveTargets.end())
                e.additiveTargets.push_back(tween.target.get());
        }

        // TO becomes FROM, TO becomes 0
        const double toNumber = tween.toNumber;
        tween.fromNumber = lookupTween->fromNumber - toNumber;
        tween.toNumber = 0;
        tween.numbers = tween.fromNumbers;
        tween.number = 0;
        lookupTween->fromNumber = toNumber;

        if (!tween.toNumbers.empty()){
            std::vector<double> toNumbers = tween.toNumbers;
            if (tween.fromNumbers.size() < toNumbers.size())
                tween.fromNumbers.resize(toNumbers.size(), 0.0);
            for (size_t i = 0; i < toNumbers.size(); i++){
                const double lookupFrom = i < lookupTween->fromNumbers.size() ? lookupTween->fromNumbers[i] : 0.0;
                tween.fromNumbers[i] = lookupFrom - toNumbers[i];
                tween.toNumbers[i] = 0;
            }
            lookupTween->fromNumbers = toNumbers;
        }

        addAdditiveSibling(additiveTweenSiblings, tween);
    }
}

void AnimComposer::removeTweenSiblings(AnimTween& tween){
    const AnimComposition tweenComposition = tween.composition;
    if (tweenComposition == AnimComposition::NONE || !tween.target)
        return;
    AnimTarget& target = *tween.target;
    if (tween.inReplaceList){
        auto it = target.replaceSiblings.find(tween.property);
        if (it != target.replaceSiblings.end())
            removeReplaceSibling(it->second, tween);
        tween.inReplaceList = false;
    }
    if (tweenComposition == AnimComposition::BLEND && tween.inAdditiveList){
        auto it = target.additiveSiblings.find(tween.property);
        if (it == target.additiveSiblings.end()){
            tween.inAdditiveList = false;
            return;
        }
        AnimTweenSiblings& additiveTweenSiblings = it->second;
        removeAdditiveSibling(additiveTweenSiblings, tween);
        // only the lookup left: drop it
        AnimTween* lookupTween = additiveTweenSiblings.head;
        if (lookupTween && lookupTween == additiveTweenSiblings.tail){
            removeAdditiveSibling(additiveTweenSiblings, *lookupTween);
            AnimEngine& e = engine();
            AnimTween* prev = lookupTween->prev;
            AnimTween* next = lookupTween->next;
            if (prev) prev->next = next; else e.additiveHead = next;
            if (next) next->prev = prev; else e.additiveTail = prev;
            target.additiveSiblings.erase(it);
            if (target.additiveSiblings.empty()){
                auto t = std::find(e.additiveTargets.begin(), e.additiveTargets.end(), &target);
                if (t != e.additiveTargets.end())
                    e.additiveTargets.erase(t);
            }
            // last: the lookup may hold the last reference to the target
            for (auto owned = e.additiveTweens.begin(); owned != e.additiveTweens.end(); ++owned){
                if (owned->get() == lookupTween){
                    std::unique_ptr<AnimTween> dying = std::move(*owned);
                    e.additiveTweens.erase(owned);
                    break;
                }
            }
        }
    }
}

namespace {

    bool containsTarget(const AnimTargetList& targets, const AnimTarget* target){
        for (const AnimTargetPtr& t : targets){
            if (t.get() == target)
                return true;
        }
        return false;
    }

    // removeTargetsFromJSAnimation
    bool removeTargetsFromAnimation(const AnimTargetList& targets, AnimAnimation& animation, const std::string& propertyName){
        bool matches = false;
        forEachTween(animation, [&](AnimTween& tween){
            if (!containsTarget(targets, tween.target.get()))
                return;
            const std::string normalized = propertyName.empty() ? std::string() : tween.target->sanitizeProperty(propertyName, tween.tweenType);
            if (normalized.empty() || normalized == tween.property){
                unlinkTween(animation, &tween);
                AnimComposer::removeTweenSiblings(tween);
                matches = true;
            }
        }, true);
        return matches;
    }

    bool hasHead(AnimTimer& timer){
        if (timer.kind == AnimTimer::Kind::ANIMATION)
            return static_cast<AnimAnimation&>(timer).head != nullptr;
        if (timer.kind == AnimTimer::Kind::TIMELINE)
            return static_cast<AnimTimeline&>(timer).childHead != nullptr;
        return false;
    }

    void unlinkChild(AnimTimer* parent, AnimTimer& child){
        AnimTimer*& head = parent ? static_cast<AnimTimeline*>(parent)->childHead : engine().head;
        AnimTimer*& tail = parent ? static_cast<AnimTimeline*>(parent)->childTail : engine().tail;
        AnimTimer* prev = child.prev;
        AnimTimer* next = child.next;
        if (prev) prev->next = next; else head = next;
        if (next) next->prev = prev; else tail = prev;
        child.prev = nullptr;
        child.next = nullptr;
    }

}

void AnimComposer::removeTargets(const AnimTargetList& targets, AnimTimer* renderable, const std::string& propertyName){
    AnimEngine& e = engine();
    bool removeMatches = false;
    const bool parentHasChildren = renderable ? renderable->hasChildren : e.head != nullptr;
    if (parentHasChildren){
        double iterationDuration = 0;
        // reverse order, the next saved first
        AnimTimer* child = renderable ? static_cast<AnimTimeline*>(renderable)->childTail : e.tail;
        while (child){
            AnimTimer* prevChild = child->prev;
            std::shared_ptr<AnimTimer> keep = child->shared_from_this();
            if (!child->hasChildren){
                removeMatches = child->kind == AnimTimer::Kind::ANIMATION &&
                                removeTargetsFromAnimation(targets, static_cast<AnimAnimation&>(*child), propertyName);
                if (removeMatches && !hasHead(*child)){
                    child->cancel();
                    unlinkChild(renderable, *child);
                    if (!renderable){
                        e.hasChildren = e.tail != nullptr;
                        child->running = false;
                        child->engineRef.reset();
                    }
                } else {
                    const double childTLOffset = child->offset + child->delay;
                    const double childDur = childTLOffset + child->duration;
                    if (childDur > iterationDuration)
                        iterationDuration = childDur;
                }
            }
            if (hasHead(*child)){
                if (child->kind == AnimTimer::Kind::TIMELINE)
                    removeTargets(targets, child, propertyName);
            } else {
                child->hasChildren = false;
            }
            child = prevChild;
        }
        if (renderable)
            renderable->iterationDuration = iterationDuration;
    } else if (renderable && renderable->kind == AnimTimer::Kind::ANIMATION){
        removeMatches = removeTargetsFromAnimation(targets, static_cast<AnimAnimation&>(*renderable), propertyName);
    }
    if (removeMatches && !(renderable ? hasHead(*renderable) : e.head != nullptr)){
        if (renderable){
            renderable->hasChildren = false;
            renderable->cancel();
        }
    }
}

bool AnimComposer::renderTween(AnimTween& tween, double tweenProgress, int internalRender){
    const AnimValueType tweenValueType = tween.valueType;
    const AnimTweenType tweenType = tween.tweenType;
    const bool tweenIsObject = tweenType == AnimTweenType::OBJECT;
    const bool tweenIsNumber = tweenValueType == AnimValueType::NUMBER;
    // in-between values written as text are rounded; object numbers stay raw
    const int tweenPrecision = (tweenIsNumber && tweenIsObject) || tweenProgress == 0 || tweenProgress == 1 ? -1 : AnimConst::precision;
    const bool tweenHasComposition = tween.composition != AnimComposition::NONE;

    AnimWrite write;
    write.valueType = tweenValueType;
    double number = 0;

    if (tweenIsNumber){
        number = tween.modify(AnimMath::round(AnimMath::lerp(tween.fromNumber, tween.toNumber, tweenProgress), tweenPrecision));
        write.number = number;
    } else if (tweenValueType == AnimValueType::UNIT){
        number = tween.modify(AnimMath::round(AnimMath::lerp(tween.fromNumber, tween.toNumber, tweenProgress), tweenPrecision));
        write.number = number;
        write.text = AnimMath::toString(number) + tween.unit;
        write.unit = &tween.unit;
    } else if (tweenValueType == AnimValueType::COLOR){
        std::vector<double>& ns = tween.numbers;
        if (ns.size() < 4) ns.resize(4, 0.0);
        const std::vector<double>& fn = tween.fromNumbers;
        const std::vector<double>& tn = tween.toNumbers;
        auto at = [](const std::vector<double>& v, size_t i){ return i < v.size() ? v[i] : 0.0; };
        const double omt = 1 - tweenProgress;
        const double fr = at(fn, 0), fg = at(fn, 1), fb = at(fn, 2);
        const double tr = at(tn, 0), tg = at(tn, 1), tb = at(tn, 2);
        // rgb in pseudo-linear space (squares, square root): close to gamma-correct blending
        ns[0] = tween.modify(std::sqrt(fr * fr * omt + tr * tr * tweenProgress));
        ns[1] = tween.modify(std::sqrt(fg * fg * omt + tg * tg * tweenProgress));
        ns[2] = tween.modify(std::sqrt(fb * fb * omt + tb * tb * tweenProgress));
        ns[3] = tween.modify(AnimMath::lerp(at(fn, 3), at(tn, 3), tweenProgress));
        write.text = AnimValue::colorToRgbaString({ns[0], ns[1], ns[2], ns[3]});
        write.numbers = &tween.numbers;
    } else if (tweenValueType == AnimValueType::COMPLEX){
        write.text = composeComplexValue(tween, tweenProgress, tweenPrecision);
        write.numbers = &tween.numbers;
    }

    if (tweenHasComposition)
        tween.number = number;

    if (!internalRender && tween.composition != AnimComposition::BLEND){
        if (!tween.target || !tween.target->isValid())
            return false;
        tween.target->setValue(tween.property, tweenType, write);
        return true;
    }
    // composing timeline tweens without a real render
    tween.value = tweenIsNumber ? AnimRawValue(number) : AnimRawValue(write.text);
    return false;
}

void AnimComposer::updateAdditive(){
    AnimEngine& e = engine();
    if (!e.additiveHead)
        return;
    for (AnimTarget* target : e.additiveTargets){
        for (auto& entry : target->additiveSiblings){
            AnimTweenSiblings& tweens = entry.second;
            AnimTween* lookupTween = tweens.head;
            if (!lookupTween)
                continue;
            const AnimValueType valueType = lookupTween->valueType;
            const bool arrays = valueType == AnimValueType::COMPLEX || valueType == AnimValueType::COLOR;
            std::vector<double> additiveValues = arrays ? lookupTween->fromNumbers : std::vector<double>();
            double additiveValue = lookupTween->fromNumber;
            AnimTween* tween = tweens.tail;
            while (tween && tween != lookupTween){
                if (arrays){
                    for (size_t i = 0; i < tween->numbers.size(); i++){
                        if (i >= additiveValues.size()) additiveValues.resize(i + 1, 0.0);
                        additiveValues[i] += tween->numbers[i];
                    }
                } else {
                    additiveValue += tween->number;
                }
                tween = tween->prevAdd;
            }
            lookupTween->toNumber = additiveValue;
            lookupTween->toNumbers = additiveValues;
        }
    }
    // Anime renders its additive animation at its end (progress = ease(1))
    for (AnimTween* lookup = e.additiveHead; lookup; lookup = lookup->next){
        const double progress = lookup->ease ? lookup->ease(1.0) : 1.0;
        lookup->currentTime = AnimConst::minValue;
        renderTween(*lookup, progress, 0);
    }
}

void AnimComposer::resetAdditive(){
    AnimEngine& e = engine();
    for (AnimTarget* target : e.additiveTargets)
        target->additiveSiblings.clear();
    e.additiveTargets.clear();
    e.additiveHead = nullptr;
    e.additiveTail = nullptr;
    std::vector<std::unique_ptr<AnimTween>> dying;
    dying.swap(e.additiveTweens);
}

void AnimComposer::revertValues(AnimTimer& renderable, bool inlineStylesOnly){
    if (renderable.hasChildren){
        AnimTimeline& timeline = static_cast<AnimTimeline&>(renderable);
        AnimTimer* child = timeline.childTail;
        while (child){
            AnimTimer* prev = child->prev;
            revertValues(*child, inlineStylesOnly);
            child = prev;
        }
        return;
    }
    if (renderable.kind != AnimTimer::Kind::ANIMATION)
        return;
    AnimAnimation& animation = static_cast<AnimAnimation&>(renderable);
    animation.pause();
    forEachTween(animation, [&](AnimTween& tween){
        if (!tween.target)
            return;
        if (inlineStylesOnly && (tween.tweenType == AnimTweenType::OBJECT || tween.tweenType == AnimTweenType::ATTRIBUTE))
            return;
        tween.target->revertValue(tween.property, tween.tweenType, tween.inlineValue);
    });
    AnimProperty::flush();
}
