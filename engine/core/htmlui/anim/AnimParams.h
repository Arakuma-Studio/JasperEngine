// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMPARAMS_H
#define ANIMPARAMS_H

// The parameters of the anime API: a JavaScript-object-like tree (what Anime.js receives as
// `{ x: 100, duration: 500, ease: 'outQuad', onComplete: fn }`), built from Lua tables by the
// binding or directly in C++:
//
//   Anime::animate(targets, AnimParam::object({{"translateX", 100}, {"duration", 500},
//                                              {"ease", "outElastic(1, .6)"}}));

#include "Export.h"
#include "AnimEasing.h"
#include "AnimValue.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    class AnimTimer;
    class AnimTimeline;
    class AnimTarget;
    class AnimSpring;

    using AnimTargetPtr = std::shared_ptr<AnimTarget>;
    using AnimTargetList = std::vector<AnimTargetPtr>;

    // what a function value receives: Anime's (target, index, targets, prevTween, timeline)
    struct AnimFunctionArgs {
        AnimTargetPtr target;
        int index = 0;
        int total = 0;
        const AnimTargetList* targets = nullptr;
        AnimTimeline* timeline = nullptr;    // stagger positions of Timeline::add
        AnimRawValue previousValue;         // previous keyframe endpoint, for SVG morphTo
    };

    class JASPERENGINE_API AnimParam {
    public:
        enum class Type { NIL, BOOL, NUMBER, STRING, ARRAY, OBJECT, FUNCTION, EASE, SPRING, CALLBACK, TIMER };
        // a function value (Anime's `(target, index, targets) => value`), stagger included
        using Function = std::function<AnimParam(const AnimFunctionArgs&)>;
        using Callback = std::function<void(AnimTimer&)>;
        using Member = std::pair<std::string, AnimParam>;

        AnimParam() = default;
        AnimParam(bool v);
        AnimParam(double v);
        AnimParam(int v);
        AnimParam(const char* v);
        AnimParam(std::string v);
        AnimParam(const AnimRawValue& v);

        static AnimParam array(std::initializer_list<AnimParam> items = {});
        static AnimParam array(std::vector<AnimParam> items);
        // members keep their insertion order (property order of the tweens)
        static AnimParam object(std::initializer_list<Member> members = {});
        static AnimParam function(Function fn);
        // an easing function, or a number -> number modifier
        static AnimParam ease(EaseFn fn);
        static AnimParam spring(std::shared_ptr<AnimSpring> spring);
        static AnimParam callback(Callback fn);
        static AnimParam timer(std::shared_ptr<AnimTimer> timer);

        Type getType() const { return type; }
        bool isNil() const { return type == Type::NIL; }
        bool isBool() const { return type == Type::BOOL; }
        bool isNumber() const { return type == Type::NUMBER; }
        bool isString() const { return type == Type::STRING; }
        bool isArray() const { return type == Type::ARRAY; }
        bool isObject() const { return type == Type::OBJECT; }
        bool isFunction() const { return type == Type::FUNCTION; }
        bool isEase() const { return type == Type::EASE; }
        bool isSpring() const { return type == Type::SPRING; }
        bool isCallback() const { return type == Type::CALLBACK; }
        bool isTimer() const { return type == Type::TIMER; }

        bool asBool() const { return boolValue; }
        double asNumber() const { return numberValue; }
        const std::string& asString() const { return stringValue; }
        const Function& asFunction() const { return functionValue; }
        const EaseFn& asEase() const { return easeValue; }
        const std::shared_ptr<AnimSpring>& asSpring() const { return springValue; }
        const Callback& asCallback() const { return callbackValue; }
        const std::shared_ptr<AnimTimer>& asTimer() const { return timerValue; }

        // JavaScript truthiness: nil, false, 0, NaN and "" are false; everything else true
        bool isTruthy() const;
        // JavaScript unary plus (NaN for objects, arrays of more than one item, functions)
        double toNumber() const;
        // NUMBER/STRING/BOOL as a raw value (BOOL as 1/0), nil otherwise
        AnimRawValue toRaw() const;

        // ARRAY
        size_t size() const;                                   // items (ARRAY) or members (OBJECT)
        const AnimParam& at(size_t index) const;               // nil out of range
        void push(AnimParam value);
        const std::vector<AnimParam>& getItems() const { return items; }

        // OBJECT
        const AnimParam* find(const std::string& key) const;   // nullptr when absent or nil
        const AnimParam& get(const std::string& key) const;    // nil when absent
        bool has(const std::string& key) const { return find(key) != nullptr; }
        void set(const std::string& key, AnimParam value);     // replaces, or appends
        void erase(const std::string& key);
        const std::vector<Member>& getMembers() const { return members; }

    private:
        Type type = Type::NIL;
        bool boolValue = false;
        double numberValue = 0.0;
        std::string stringValue;
        std::vector<AnimParam> items;
        std::vector<Member> members;
        Function functionValue;
        EaseFn easeValue;
        std::shared_ptr<AnimSpring> springValue;
        Callback callbackValue;
        std::shared_ptr<AnimTimer> timerValue;
    };

}

#endif //ANIMPARAMS_H
