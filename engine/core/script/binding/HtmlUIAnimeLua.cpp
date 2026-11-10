// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The global `anime` table (phase-1 §3.11): an Anime.js v4-shaped animation API for Lua on
// top of engine/core/htmlui/anim/.
//
//   anime.animate("#box", { translateX = 250, rotate = "1turn", duration = 800, ease = "outElastic(1, .6)" })
//   local tl = anime.createTimeline({ loop = true, alternate = true })
//   tl:add(".card", { opacity = { from = 0 }, delay = anime.stagger(100) }):label("shown")
//
// Targets are CSS selectors (every loaded document of the current scene, or `root`),
// UIElements, lists of them, and plain Lua tables (their numeric and colour fields).
// Lua callbacks, function values and table targets are registry refs released by
// AnimEngine::reset() (play stop, shutdown). Errors are raised (luaL_error-style) only once
// every C++ object of the call is destroyed: Lua is compiled as C and longjmps.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Log.h"
#include "htmlui/anim/AnimLua.h"
#include "htmlui/anim/AnimText.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimCommon.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// the HTML UI module (docs/specs/S34-player-size.md R1): the Lua API description reads this condition
#if JASPERENGINE_HTMLUI

using namespace jasperengine;

namespace {

    // ---------------------------------------------------------------------------------------------
    // registry refs

    class LuaRefHolder;

    std::unordered_set<LuaRefHolder*>& animeLiveRefs(){
        static auto* refs = new std::unordered_set<LuaRefHolder*>();
        return *refs;
    }

    // A registry reference, kept with the main thread of the state (never a coroutine's thread,
    // which ends long before an animation callback runs: LuaBinding::mainThread)
    class LuaRefHolder {
    public:
        LuaRefHolder(lua_State* from, int index): L(LuaBinding::mainThread(from)){
            lua_pushvalue(from, index);
            ref = luaL_ref(from, LUA_REGISTRYINDEX);
            animeLiveRefs().insert(this);
        }
        ~LuaRefHolder(){
            release();
            animeLiveRefs().erase(this);
        }
        LuaRefHolder(const LuaRefHolder&) = delete;
        LuaRefHolder& operator=(const LuaRefHolder&) = delete;

        void release(){
            if (ref != LUA_NOREF && ref != LUA_REFNIL && L && LuaBinding::getLuaState() == L)
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
            ref = LUA_NOREF;
        }
        bool isAlive() const{
            return ref != LUA_NOREF && L && LuaBinding::getLuaState() == L;
        }
        // pushes the value on the main thread; false (nothing pushed) once released or the state is gone
        bool push() const{
            return push(L);
        }
        // pushes the value on `onto`, any thread of the state (the one running the binding)
        bool push(lua_State* onto) const{
            if (!isAlive() || !onto || LuaBinding::mainThread(onto) != L)
                return false;
            lua_rawgeti(onto, LUA_REGISTRYINDEX, ref);
            return true;
        }
        lua_State* state() const{ return L; }

    private:
        lua_State* L;
        int ref = LUA_NOREF;
    };

    using LuaRefPtr = std::shared_ptr<LuaRefHolder>;

    void releaseAllRefs(){
        for (LuaRefHolder* ref : animeLiveRefs())
            ref->release();
    }

    // ---------------------------------------------------------------------------------------------
    // anime objects to Lua

    void pushTimer(lua_State* L, const std::shared_ptr<AnimTimer>& timer){
        if (!timer){
            lua_pushnil(L);
            return;
        }
        luabridge::Result result;
        switch (timer->getKind()){
            case AnimTimer::Kind::ANIMATION:
                result = luabridge::push(L, std::static_pointer_cast<AnimAnimation>(timer));
                break;
            case AnimTimer::Kind::TIMELINE:
                result = luabridge::push(L, std::static_pointer_cast<AnimTimeline>(timer));
                break;
            default:
                result = luabridge::push(L, timer);
                break;
        }
        if (!result)
            lua_pushnil(L);
    }

    // a Lua table animated as an object: its number fields, and colour strings (written back as #rrggbbaa)
    class TableTarget : public AnimTarget {
    public:
        TableTarget(lua_State* L, int index): table(std::make_shared<LuaRefHolder>(L, index)), identity(lua_topointer(L, index)) {}

        const void* getIdentity() const override{ return identity; }
        bool isAlive() const{ return table->isAlive(); }
        bool push(lua_State* onto) const{ return table->push(onto); }

        AnimRawValue getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue) override{
            (void)type;
            AnimRawValue value = get(property);
            if (value.isTruthy() && inlineValue)
                *inlineValue = value;
            return value.isTruthy() ? value : AnimRawValue(0.0);
        }

        void setValue(const std::string& property, AnimTweenType type, const AnimWrite& value) override{
            (void)type;
            if (!table->push())
                return;
            lua_State* L = table->state();
            lua_pushlstring(L, property.c_str(), property.size());
            if (value.valueType == AnimValueType::NUMBER){
                lua_pushnumber(L, value.number);
            } else if (value.valueType == AnimValueType::COLOR && value.numbers && value.numbers->size() >= 4){
                const std::vector<double>& c = *value.numbers;
                const std::string hex = AnimValue::colorToHexString({c[0], c[1], c[2], c[3]});
                lua_pushlstring(L, hex.c_str(), hex.size());
            } else {
                lua_pushlstring(L, value.text.c_str(), value.text.size());
            }
            lua_rawset(L, -3);
            lua_pop(L, 1);
        }

        bool getStaggerPosition(double& x, double& y, double& z, bool& has3D) override{
            AnimRawValue vx = get("x"), vy = get("y"), vz = get("z");
            if (!vx.isNumber() || !vy.isNumber())
                return false;
            x = vx.number;
            y = vy.number;
            has3D = vz.isNumber();
            z = has3D ? vz.number : 0;
            return true;
        }

    private:
        AnimRawValue get(const std::string& property) const{
            if (!table->push())
                return AnimRawValue();
            lua_State* L = table->state();
            lua_pushlstring(L, property.c_str(), property.size());
            lua_rawget(L, -2);
            AnimRawValue value;
            if (lua_type(L, -1) == LUA_TNUMBER){
                value = AnimRawValue((double)lua_tonumber(L, -1));
            } else if (lua_type(L, -1) == LUA_TSTRING){
                size_t length = 0;
                const char* text = lua_tolstring(L, -1, &length);
                value = AnimRawValue(std::string(text, length));
            }
            lua_pop(L, 2);
            return value;
        }

        LuaRefPtr table;
        const void* identity;
    };

    // one target per table while it is animated
    std::unordered_map<const void*, std::weak_ptr<TableTarget>>& tableTargets(){
        static auto* targets = new std::unordered_map<const void*, std::weak_ptr<TableTarget>>();
        return *targets;
    }

    AnimTargetPtr tableTarget(lua_State* L, int index){
        const void* identity = lua_topointer(L, index);
        auto& targets = tableTargets();
        auto it = targets.find(identity);
        if (it != targets.end()){
            std::shared_ptr<TableTarget> existing = it->second.lock();
            if (existing && existing->isAlive()){
                return existing;
            }
        }
        for (auto e = targets.begin(); e != targets.end();){
            e = e->second.expired() ? targets.erase(e) : std::next(e);
        }
        std::shared_ptr<TableTarget> target = std::make_shared<TableTarget>(L, index);
        targets[identity] = target;
        return target;
    }

    // whether a class is bound in this state (UIElement comes with the element API)
    template<class T>
    bool isClassRegistered(lua_State* L){
        lua_rawgetp(L, LUA_REGISTRYINDEX, luabridge::detail::getClassRegistryKey<T>());
        const bool registered = lua_istable(L, -1);
        lua_pop(L, 1);
        return registered;
    }

    void pushTarget(lua_State* L, const AnimTargetPtr& target){
        if (!target){
            lua_pushnil(L);
            return;
        }
        const int top = lua_gettop(L);
        if (target->getKind() == AnimTarget::Kind::ELEMENT){
            UIElement element = AnimTargets::getElement(target);
            if (!isClassRegistered<UIElement>(L) || !luabridge::push(L, element)){
                lua_settop(L, top);
                lua_pushnil(L);
            }
            return;
        }
        TableTarget* table = dynamic_cast<TableTarget*>(target.get());
        if (table && table->push(L))
            return;
        lua_settop(L, top);
        lua_pushnil(L);
    }

    void logLuaError(lua_State* L, const char* what){
        Log::error("[HTML UI] anime: %s: %s", what, LuaBinding::getLuaStackErrorString(L, -1).c_str());
        lua_pop(L, 1);
    }

    // ---------------------------------------------------------------------------------------------
    // Lua values to parameters

    bool isCallbackKey(const std::string& key){
        return key == "onBegin" || key == "onBeforeUpdate" || key == "onUpdate" || key == "onRender" ||
               key == "onLoop" || key == "onPause" || key == "onComplete";
    }

    bool isNumberFunctionKey(const std::string& key){
        return key == "ease" || key == "playbackEase" || key == "modifier";
    }

    AnimParam toParam(lua_State* L, int index, const std::string& key);

    AnimParam::Callback callbackFromLua(lua_State* L, int index){
        LuaRefPtr fn = std::make_shared<LuaRefHolder>(L, index);
        return [fn](AnimTimer& timer){
            if (!fn->push())
                return;
            lua_State* state = fn->state();
            pushTimer(state, timer.shared_from_this());
            if (LuaBinding::pcallWithTraceback(state, 1, 0) != 0)
                logLuaError(state, "callback");
        };
    }

    EaseFn numberFunctionFromLua(lua_State* L, int index){
        LuaRefPtr fn = std::make_shared<LuaRefHolder>(L, index);
        return [fn](double t){
            if (!fn->push())
                return t;
            lua_State* state = fn->state();
            lua_pushnumber(state, t);
            if (LuaBinding::pcallWithTraceback(state, 1, 1) != 0){
                logLuaError(state, "easing/modifier function");
                return t;
            }
            const double v = lua_isnumber(state, -1) ? (double)lua_tonumber(state, -1) : t;
            lua_pop(state, 1);
            return v;
        };
    }

    AnimParam::Function functionValueFromLua(lua_State* L, int index){
        LuaRefPtr fn = std::make_shared<LuaRefHolder>(L, index);
        return [fn](const AnimFunctionArgs& args) -> AnimParam {
            if (!fn->push())
                return AnimParam();
            lua_State* state = fn->state();
            pushTarget(state, args.target);
            lua_pushinteger(state, args.index);
            lua_pushinteger(state, args.total);
            if (LuaBinding::pcallWithTraceback(state, 3, 1) != 0){
                logLuaError(state, "function value");
                return AnimParam();
            }
            AnimParam result = toParam(state, -1, "");
            lua_pop(state, 1);
            return result;
        };
    }

    template<class T>
    T* userdataOf(lua_State* L, int index){
        if (!luabridge::isInstance<T>(L, index))
            return nullptr;
        auto result = luabridge::Stack<T*>::get(L, index);
        return result ? *result : nullptr;
    }

    std::shared_ptr<AnimTimer> timerOf(lua_State* L, int index){
        AnimTimer* timer = userdataOf<AnimTimer>(L, index);
        return timer ? timer->shared_from_this() : nullptr;
    }

    AnimParam toParam(lua_State* L, int index, const std::string& key){
        index = lua_absindex(L, index);
        switch (lua_type(L, index)){
            case LUA_TBOOLEAN:
                return AnimParam(lua_toboolean(L, index) != 0);
            case LUA_TNUMBER:
                return AnimParam((double)lua_tonumber(L, index));
            case LUA_TSTRING: {
                size_t length = 0;
                const char* text = lua_tolstring(L, index, &length);
                return AnimParam(std::string(text, length));
            }
            case LUA_TFUNCTION:
                if (isCallbackKey(key))
                    return AnimParam::callback(callbackFromLua(L, index));
                if (isNumberFunctionKey(key))
                    return AnimParam::ease(numberFunctionFromLua(L, index));
                return AnimParam::function(functionValueFromLua(L, index));
            case LUA_TUSERDATA: {
                if (AnimEaseFunction* ease = userdataOf<AnimEaseFunction>(L, index))
                    return AnimParam::ease(ease->getFunction());
                if (AnimSpring* spring = userdataOf<AnimSpring>(L, index))
                    return AnimParam::spring(spring->shared_from_this());
                if (AnimStaggerFunction* stagger = userdataOf<AnimStaggerFunction>(L, index))
                    return AnimParam::function(stagger->getFunction());
                if (std::shared_ptr<AnimTimer> timer = timerOf(L, index))
                    return AnimParam::timer(timer);
                return AnimParam();
            }
            case LUA_TTABLE: {
                const lua_Integer length = (lua_Integer)lua_rawlen(L, index);
                if (length > 0){
                    std::vector<AnimParam> items;
                    for (lua_Integer i = 1; i <= length; i++){
                        lua_rawgeti(L, index, i);
                        items.push_back(toParam(L, -1, key));
                        lua_pop(L, 1);
                    }
                    return AnimParam::array(std::move(items));
                }
                // keys sorted: Lua tables have no order, the tweens get a stable one
                std::vector<std::string> keys;
                lua_pushnil(L);
                while (lua_next(L, index) != 0){
                    if (lua_type(L, -2) == LUA_TSTRING){
                        size_t keyLength = 0;
                        const char* k = lua_tolstring(L, -2, &keyLength);
                        keys.emplace_back(k, keyLength);
                    } else if (lua_type(L, -2) == LUA_TNUMBER){
                        keys.push_back(AnimMath::toString((double)lua_tonumber(L, -2)));
                    }
                    lua_pop(L, 1);
                }
                std::sort(keys.begin(), keys.end());
                AnimParam object = AnimParam::object();
                for (const std::string& k : keys){
                    lua_pushlstring(L, k.c_str(), k.size());
                    lua_rawget(L, index);
                    if (lua_isnil(L, -1)){
                        lua_pop(L, 1);
                        // a numeric key
                        const double n = AnimMath::toNumber(k);
                        if (std::isnan(n))
                            continue;
                        lua_pushnumber(L, n);
                        lua_rawget(L, index);
                    }
                    object.set(k, toParam(L, -1, k));
                    lua_pop(L, 1);
                }
                return object;
            }
            default:
                return AnimParam();
        }
    }

    // targets: selectors, UIElements, lists (flattened), plain tables, anime objects
    void toTargets(lua_State* L, int index, const UIElement& root, AnimTargetList& out, int depth = 0){
        index = lua_absindex(L, index);
        switch (lua_type(L, index)){
            case LUA_TSTRING: {
                size_t length = 0;
                const char* text = lua_tolstring(L, index, &length);
                for (const AnimTargetPtr& t : AnimTargets::fromSelector(std::string(text, length), root))
                    AnimTargets::appendUnique(out, t);
                break;
            }
            case LUA_TUSERDATA: {
                if (luabridge::isInstance<UIElement>(L, index)){
                    auto element = luabridge::Stack<UIElement>::get(L, index);
                    if (element)
                        AnimTargets::appendUnique(out, AnimTargets::fromElement(*element));
                } else if (std::shared_ptr<AnimTimer> timer = timerOf(L, index)){
                    AnimTargets::appendUnique(out, AnimTargets::fromTimer(timer));
                }
                break;
            }
            case LUA_TTABLE: {
                const lua_Integer length = (lua_Integer)lua_rawlen(L, index);
                if (length > 0 && depth < 16){
                    for (lua_Integer i = 1; i <= length; i++){
                        lua_rawgeti(L, index, i);
                        toTargets(L, -1, root, out, depth + 1);
                        lua_pop(L, 1);
                    }
                } else {
                    AnimTargets::appendUnique(out, tableTarget(L, index));
                }
                break;
            }
            default:
                break;
        }
    }

    // the `root` of a parameters table: a UIElement or a UIDocument
    UIElement animeRootOf(lua_State* L, int paramsIndex){
        UIElement root;
        if (lua_type(L, paramsIndex) != LUA_TTABLE)
            return root;
        lua_getfield(L, paramsIndex, "root");
        if (luabridge::isInstance<UIElement>(L, -1)){
            auto element = luabridge::Stack<UIElement>::get(L, -1);
            if (element)
                root = *element;
        } else if (luabridge::isInstance<UIDocument>(L, -1)){
            auto document = luabridge::Stack<UIDocument*>::get(L, -1);
            if (document && *document)
                root = (*document)->getBody();
        }
        lua_pop(L, 1);
        return root;
    }

    // ---------------------------------------------------------------------------------------------
    // anime.* (each *Impl returns false after pushing an error message; the caller raises it
    // once the Impl's C++ objects are gone)

    bool checkClock(lua_State* L, const AnimParam& params){
        std::string error;
        AnimClock clock;
        if (!parseAnimClock(params.get("clock"), clock, &error)){
            lua_pushstring(L, error.c_str());
            return false;
        }
        return true;
    }

    bool animateImpl(lua_State* L){
        if (lua_type(L, 2) != LUA_TTABLE){
            lua_pushstring(L, "anime.animate(targets, params): params must be a table");
            return false;
        }
        UIElement root = animeRootOf(L, 2);
        AnimTargetList targets;
        toTargets(L, 1, root, targets);
        AnimParam params = toParam(L, 2, "");
        if (!checkClock(L, params))
            return false;
        pushTimer(L, Anime::animate(targets, params));
        return true;
    }

    int luaAnimate(lua_State* L){
        if (!animateImpl(L))
            return lua_error(L);
        return 1;
    }

    void splitTextImpl(lua_State* L){
        AnimTargetList targets;
        toTargets(L, 1, UIElement(), targets);
        const AnimParam params = lua_type(L, 2) == LUA_TTABLE ? toParam(L, 2, "") : AnimParam::object();
        if (!luabridge::push(L, Anime::splitText(targets, params))) lua_pushnil(L);
    }

    int luaSplitText(lua_State* L){
        splitTextImpl(L);
        return 1;
    }

    bool createTimelineImpl(lua_State* L){
        AnimParam params = lua_type(L, 1) == LUA_TTABLE ? toParam(L, 1, "") : AnimParam::object();
        if (!checkClock(L, params))
            return false;
        pushTimer(L, Anime::createTimeline(params));
        return true;
    }

    int luaCreateTimeline(lua_State* L){
        if (!createTimelineImpl(L))
            return lua_error(L);
        return 1;
    }

    bool createTimerImpl(lua_State* L){
        AnimParam params = lua_type(L, 1) == LUA_TTABLE ? toParam(L, 1, "") : AnimParam::object();
        if (!checkClock(L, params))
            return false;
        pushTimer(L, Anime::createTimer(params));
        return true;
    }

    int luaCreateTimer(lua_State* L){
        if (!createTimerImpl(L))
            return lua_error(L);
        return 1;
    }

    void createSpringImpl(lua_State* L){
        AnimSpringParams params;
        if (lua_type(L, 1) == LUA_TTABLE){
            auto number = [&](const char* name, std::optional<double>& out){
                lua_getfield(L, 1, name);
                if (lua_isnumber(L, -1))
                    out = (double)lua_tonumber(L, -1);
                lua_pop(L, 1);
            };
            number("mass", params.mass);
            number("stiffness", params.stiffness);
            number("damping", params.damping);
            number("velocity", params.velocity);
            number("bounce", params.bounce);
            number("duration", params.duration);
            lua_getfield(L, 1, "onComplete");
            if (lua_isfunction(L, -1)){
                AnimParam::Callback callback = callbackFromLua(L, -1);
                params.onComplete = [callback](AnimTimer* timer){
                    if (timer && callback)
                        callback(*timer);
                };
            }
            lua_pop(L, 1);
        }
        std::shared_ptr<AnimSpring> spring = Anime::createSpring(params);
        if (!luabridge::push(L, spring))
            lua_pushnil(L);
    }

    int luaCreateSpring(lua_State* L){
        createSpringImpl(L);
        return 1;
    }

    void staggerImpl(lua_State* L){
        AnimParam value = toParam(L, 1, "");
        AnimParam params = lua_type(L, 2) == LUA_TTABLE ? toParam(L, 2, "") : AnimParam::object();
        AnimStaggerFunction stagger(Anime::stagger(value, params).asFunction());
        if (!luabridge::push(L, stagger))
            lua_pushnil(L);
    }

    int luaStagger(lua_State* L){
        staggerImpl(L);
        return 1;
    }

    void pushEase(lua_State* L, const AnimEaseFunction& ease){
        if (!luabridge::push(L, ease))
            lua_pushnil(L);
    }

    std::vector<double> numberArgs(lua_State* L, int first){
        std::vector<double> args;
        for (int i = first; i <= lua_gettop(L); i++)
            args.push_back(lua_isnumber(L, i) ? (double)lua_tonumber(L, i) : std::nan(""));
        return args;
    }

    double numberArg(lua_State* L, int index, double fallback){
        return lua_isnumber(L, index) ? (double)lua_tonumber(L, index) : fallback;
    }

    int luaCubicBezier(lua_State* L){
        pushEase(L, AnimEaseFunction(AnimEasing::cubicBezier(numberArg(L, 1, .5), numberArg(L, 2, 0), numberArg(L, 3, .5), numberArg(L, 4, 1)), "cubicBezier"));
        return 1;
    }

    int luaSteps(lua_State* L){
        pushEase(L, AnimEaseFunction(AnimEasing::steps(numberArg(L, 1, 10), lua_toboolean(L, 2) != 0), "steps"));
        return 1;
    }

    void linearImpl(lua_State* L){
        std::vector<AnimLinearPoint> points;
        for (int i = 1; i <= lua_gettop(L); i++){
            AnimLinearPoint point;
            if (lua_type(L, i) == LUA_TSTRING){
                std::string text = lua_tostring(L, i);
                point.value = AnimMath::parseFloat(text);
                size_t space = text.find(' ');
                if (space != std::string::npos){
                    point.hasPercent = true;
                    point.percent = AnimMath::parseFloat(text.substr(space + 1));
                }
            } else {
                point.value = numberArg(L, i, 0);
            }
            points.push_back(point);
        }
        pushEase(L, AnimEaseFunction(AnimEasing::linear(points), "linear"));
    }

    int luaLinear(lua_State* L){
        linearImpl(L);
        return 1;
    }

    int luaIrregular(lua_State* L){
        pushEase(L, AnimEaseFunction(AnimEasing::irregular((int)numberArg(L, 1, 10), numberArg(L, 2, 1)), "irregular"));
        return 1;
    }

    // ease(t), or a parametric ease's factory: ease(params...) -> a new ease
    void easeCallImpl(lua_State* L){
        AnimEaseFunction* ease = userdataOf<AnimEaseFunction>(L, 1);
        if (!ease){
            lua_pushnil(L);
            return;
        }
        if (ease->isParametric()){
            pushEase(L, ease->withParameters(numberArgs(L, 2)));
        } else {
            lua_pushnumber(L, ease->evaluate(numberArg(L, 2, 0)));
        }
    }

    int luaEaseCall(lua_State* L){
        easeCallImpl(L);
        return 1;
    }

    // stagger(target, index, total): the value for a target (index 0-based, as Anime)
    void staggerCallImpl(lua_State* L){
        AnimStaggerFunction* stagger = userdataOf<AnimStaggerFunction>(L, 1);
        if (!stagger || !stagger->getFunction()){
            lua_pushnil(L);
            return;
        }
        AnimTargetList targets;
        if (!lua_isnil(L, 2))
            toTargets(L, 2, UIElement(), targets);
        const int total = (int)numberArg(L, 4, targets.empty() ? 1 : (double)targets.size());
        AnimTargetList all = targets;
        while ((int)all.size() < total)
            all.push_back(std::make_shared<AnimObjectTarget>());
        AnimFunctionArgs args;
        args.target = all.empty() ? nullptr : all[0];
        args.index = (int)numberArg(L, 3, 0);
        args.total = total;
        args.targets = &all;
        AnimParam result = stagger->getFunction()(args);
        if (result.isNumber()){
            lua_pushnumber(L, result.asNumber());
        } else if (result.isString()){
            lua_pushstring(L, result.asString().c_str());
        } else {
            lua_pushnil(L);
        }
    }

    int luaStaggerCall(lua_State* L){
        staggerCallImpl(L);
        return 1;
    }

    // ---------------------------------------------------------------------------------------------
    // timer methods (each returns the object itself, typed as it is, for chaining)

    template<class T>
    std::shared_ptr<T> self(T* t){
        return std::static_pointer_cast<T>(t->shared_from_this());
    }

    template<class T> std::shared_ptr<T> timerPlay(T* t){ t->play(); return self(t); }
    template<class T> std::shared_ptr<T> timerPause(T* t){ t->pause(); return self(t); }
    template<class T> std::shared_ptr<T> timerResume(T* t){ t->resume(); return self(t); }
    template<class T> std::shared_ptr<T> timerRestart(T* t){ t->restart(); return self(t); }
    template<class T> std::shared_ptr<T> timerReverse(T* t){ t->reverse(); return self(t); }
    template<class T> std::shared_ptr<T> timerAlternate(T* t){ t->alternate(); return self(t); }
    template<class T> std::shared_ptr<T> timerCancel(T* t){ t->cancel(); return self(t); }
    template<class T> std::shared_ptr<T> timerRevert(T* t){ t->revert(); return self(t); }
    template<class T> std::shared_ptr<T> timerReset(T* t){ t->reset(); return self(t); }
    template<class T> std::shared_ptr<T> timerInit(T* t){ t->init(); return self(t); }
    template<class T> std::shared_ptr<T> timerRefresh(T* t){ t->refresh(); return self(t); }
    template<class T> std::shared_ptr<T> timerResetTime(T* t){ t->resetTime(); return self(t); }
    template<class T> std::shared_ptr<T> timerStretch(T* t, double duration){ t->stretch(duration); return self(t); }
    template<class T> std::shared_ptr<T> timerSeek(T* t, double time, bool muteCallbacks){ t->seek(time, muteCallbacks); return self(t); }
    template<class T> std::shared_ptr<T> timerComplete(T* t, bool muteCallbacks){ t->complete(muteCallbacks); return self(t); }

    // andThen(fn): Anime's then(), renamed (`then` is a Lua keyword)
    void thenImpl(lua_State* L){
        std::shared_ptr<AnimTimer> timer = timerOf(L, 1);
        if (timer && lua_isfunction(L, 2))
            timer->then(callbackFromLua(L, 2));
        pushTimer(L, timer);
    }

    int luaThen(lua_State* L){
        thenImpl(L);
        return 1;
    }

    luabridge::LuaRef timerId(const AnimTimer* timer, lua_State* L){
        const AnimParam& id = timer->getId();
        if (id.isNumber())
            return luabridge::LuaRef(L, id.asNumber());
        if (id.isString())
            return luabridge::LuaRef(L, id.asString());
        return luabridge::LuaRef(L);
    }

    luabridge::LuaRef animationTargets(const AnimAnimation* animation, lua_State* L){
        lua_createtable(L, (int)animation->getTargets().size(), 0);
        int i = 1;
        for (const AnimTargetPtr& target : animation->getTargets()){
            pushTarget(L, target);
            lua_rawseti(L, -2, i++);
        }
        luabridge::LuaRef table = luabridge::LuaRef::fromStack(L);
        return table;
    }

    luabridge::LuaRef timelineLabels(const AnimTimeline* timeline, lua_State* L){
        lua_createtable(L, 0, (int)timeline->getLabels().size());
        for (const auto& label : timeline->getLabels()){
            lua_pushnumber(L, label.second);
            lua_setfield(L, -2, label.first.c_str());
        }
        luabridge::LuaRef table = luabridge::LuaRef::fromStack(L);
        return table;
    }

    double timerIterationCount(const AnimTimer* timer){
        return timer->getIterationCount();
    }

    // ---------------------------------------------------------------------------------------------
    // timeline methods

    AnimParam positionArg(lua_State* L, int index){
        return lua_isnoneornil(L, index) ? AnimParam() : toParam(L, index, "");
    }

    // tl:add(targets, params[, position]) or tl:add(timerParams[, position]) or tl:add(timer[, position])
    bool timelineAddImpl(lua_State* L){
        AnimTimeline* timeline = userdataOf<AnimTimeline>(L, 1);
        if (!timeline){
            lua_pushstring(L, "AnimeTimeline:add: call it with ':' on a timeline");
            return false;
        }
        std::shared_ptr<AnimTimeline> keep = self(timeline);
        if (lua_type(L, 3) == LUA_TTABLE){
            UIElement root = animeRootOf(L, 3);
            AnimTargetList targets;
            toTargets(L, 2, root, targets);
            timeline->add(targets, toParam(L, 3, ""), positionArg(L, 4));
        } else if (std::shared_ptr<AnimTimer> timer = timerOf(L, 2)){
            timeline->sync(timer, positionArg(L, 3));
        } else if (lua_type(L, 2) == LUA_TTABLE){
            timeline->add(toParam(L, 2, ""), positionArg(L, 3));
        } else {
            lua_pushstring(L, "AnimeTimeline:add(targets, params[, position]) or :add(timerParams[, position])");
            return false;
        }
        pushTimer(L, keep);
        return true;
    }

    int luaTimelineAdd(lua_State* L){
        if (!timelineAddImpl(L))
            return lua_error(L);
        return 1;
    }

    bool timelineSetImpl(lua_State* L){
        AnimTimeline* timeline = userdataOf<AnimTimeline>(L, 1);
        if (!timeline || lua_type(L, 3) != LUA_TTABLE){
            lua_pushstring(L, "AnimeTimeline:set(targets, props[, position])");
            return false;
        }
        std::shared_ptr<AnimTimeline> keep = self(timeline);
        UIElement root = animeRootOf(L, 3);
        AnimTargetList targets;
        toTargets(L, 2, root, targets);
        timeline->set(targets, toParam(L, 3, ""), positionArg(L, 4));
        pushTimer(L, keep);
        return true;
    }

    int luaTimelineSet(lua_State* L){
        if (!timelineSetImpl(L))
            return lua_error(L);
        return 1;
    }

    bool timelineCallImpl(lua_State* L){
        AnimTimeline* timeline = userdataOf<AnimTimeline>(L, 1);
        if (!timeline || !lua_isfunction(L, 2)){
            lua_pushstring(L, "AnimeTimeline:call(fn[, position])");
            return false;
        }
        std::shared_ptr<AnimTimeline> keep = self(timeline);
        timeline->call(callbackFromLua(L, 2), positionArg(L, 3));
        pushTimer(L, keep);
        return true;
    }

    int luaTimelineCall(lua_State* L){
        if (!timelineCallImpl(L))
            return lua_error(L);
        return 1;
    }

    bool timelineSyncImpl(lua_State* L){
        AnimTimeline* timeline = userdataOf<AnimTimeline>(L, 1);
        std::shared_ptr<AnimTimer> timer = timerOf(L, 2);
        if (!timeline || !timer){
            lua_pushstring(L, "AnimeTimeline:sync(animation[, position])");
            return false;
        }
        std::shared_ptr<AnimTimeline> keep = self(timeline);
        timeline->sync(timer, positionArg(L, 3));
        pushTimer(L, keep);
        return true;
    }

    int luaTimelineSync(lua_State* L){
        if (!timelineSyncImpl(L))
            return lua_error(L);
        return 1;
    }

    bool timelineRemoveImpl(lua_State* L){
        AnimTimeline* timeline = userdataOf<AnimTimeline>(L, 1);
        if (!timeline){
            lua_pushstring(L, "AnimeTimeline:remove(targets[, property])");
            return false;
        }
        std::shared_ptr<AnimTimeline> keep = self(timeline);
        AnimTargetList targets;
        toTargets(L, 2, UIElement(), targets);
        const std::string property = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : "";
        timeline->remove(targets, property);
        pushTimer(L, keep);
        return true;
    }

    int luaTimelineRemove(lua_State* L){
        if (!timelineRemoveImpl(L))
            return lua_error(L);
        return 1;
    }

    std::shared_ptr<AnimTimeline> timelineLabel(AnimTimeline* timeline, const std::string& name, lua_State* L){
        timeline->label(name, positionArg(L, 3));
        return self(timeline);
    }

    // ---------------------------------------------------------------------------------------------
    // anime.utils

    // Anime's chainable utils: called with fewer arguments than the function takes, a util
    // returns a function of the missing value (`modifier = anime.utils.round(0)`)
    int luaCurried(lua_State* L){
        const int stored = (int)lua_tointeger(L, lua_upvalueindex(2));
        const bool right = lua_toboolean(L, lua_upvalueindex(3)) != 0;
        const int given = lua_gettop(L) > 0 ? 1 : 0;
        lua_pushvalue(L, lua_upvalueindex(1));
        if (right){
            for (int i = 0; i < stored; i++) lua_pushvalue(L, lua_upvalueindex(4 + i));
            if (given) lua_pushvalue(L, 1); else lua_pushnil(L);
        } else {
            if (given) lua_pushvalue(L, 1); else lua_pushnil(L);
            for (int i = 0; i < stored; i++) lua_pushvalue(L, lua_upvalueindex(4 + i));
        }
        lua_call(L, stored + 1, 1);
        return 1;
    }

    // true (and a curried function pushed) when the util got fewer than arity arguments
    bool curry(lua_State* L, lua_CFunction fn, int arity, bool right = false){
        const int given = lua_gettop(L);
        if (given >= arity)
            return false;
        lua_pushcfunction(L, fn);
        lua_pushinteger(L, given);
        lua_pushboolean(L, right);
        for (int i = 1; i <= given; i++)
            lua_pushvalue(L, i);
        lua_pushcclosure(L, luaCurried, 3 + given);
        return true;
    }

    double num(lua_State* L, int index){
        return lua_isnumber(L, index) ? (double)lua_tonumber(L, index) : std::nan("");
    }

    int luaUtilsClamp(lua_State* L){
        if (curry(L, luaUtilsClamp, 3)) return 1;
        lua_pushnumber(L, AnimUtils::clamp(num(L, 1), num(L, 2), num(L, 3)));
        return 1;
    }

    int luaUtilsRound(lua_State* L){
        if (curry(L, luaUtilsRound, 2)) return 1;
        lua_pushnumber(L, AnimUtils::round(num(L, 1), (int)numberArg(L, 2, 0)));
        return 1;
    }

    int luaUtilsSnap(lua_State* L){
        if (curry(L, luaUtilsSnap, 2)) return 1;
        if (lua_type(L, 2) == LUA_TTABLE){
            std::vector<double> points;
            const lua_Integer length = (lua_Integer)lua_rawlen(L, 2);
            for (lua_Integer i = 1; i <= length; i++){
                lua_rawgeti(L, 2, i);
                points.push_back(num(L, -1));
                lua_pop(L, 1);
            }
            lua_pushnumber(L, AnimUtils::snap(num(L, 1), points));
        } else {
            lua_pushnumber(L, AnimUtils::snap(num(L, 1), numberArg(L, 2, 0)));
        }
        return 1;
    }

    int luaUtilsWrap(lua_State* L){
        if (curry(L, luaUtilsWrap, 3)) return 1;
        lua_pushnumber(L, AnimUtils::wrap(num(L, 1), num(L, 2), num(L, 3)));
        return 1;
    }

    int luaUtilsMapRange(lua_State* L){
        if (curry(L, luaUtilsMapRange, 5)) return 1;
        lua_pushnumber(L, AnimUtils::mapRange(num(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5)));
        return 1;
    }

    int luaUtilsLerp(lua_State* L){
        if (curry(L, luaUtilsLerp, 3, true)) return 1;
        lua_pushnumber(L, AnimUtils::lerp(num(L, 1), num(L, 2), num(L, 3)));
        return 1;
    }

    int luaUtilsDamp(lua_State* L){
        if (curry(L, luaUtilsDamp, 4, true)) return 1;
        lua_pushnumber(L, AnimUtils::damp(num(L, 1), num(L, 2), num(L, 3), num(L, 4)));
        return 1;
    }

    int luaUtilsDegToRad(lua_State* L){
        if (curry(L, luaUtilsDegToRad, 1)) return 1;
        lua_pushnumber(L, AnimUtils::degToRad(num(L, 1)));
        return 1;
    }

    int luaUtilsRadToDeg(lua_State* L){
        if (curry(L, luaUtilsRadToDeg, 1)) return 1;
        lua_pushnumber(L, AnimUtils::radToDeg(num(L, 1)));
        return 1;
    }

    std::string stringArg(lua_State* L, int index){
        if (lua_type(L, index) == LUA_TNUMBER)
            return AnimMath::toString((double)lua_tonumber(L, index));
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        return text ? std::string(text, length) : std::string();
    }

    void pushString(lua_State* L, const std::string& text){
        lua_pushlstring(L, text.c_str(), text.size());
    }

    void roundPadImpl(lua_State* L){
        pushString(L, AnimUtils::roundPad(num(L, 1), (int)numberArg(L, 2, 0)));
    }

    int luaUtilsRoundPad(lua_State* L){
        if (curry(L, luaUtilsRoundPad, 2)) return 1;
        roundPadImpl(L);
        return 1;
    }

    void padImpl(lua_State* L, bool start){
        const std::string value = stringArg(L, 1);
        const std::string pad = lua_isnoneornil(L, 3) ? std::string(" ") : stringArg(L, 3);
        pushString(L, start ? AnimUtils::padStart(value, (int)numberArg(L, 2, 0), pad) : AnimUtils::padEnd(value, (int)numberArg(L, 2, 0), pad));
    }

    int luaUtilsPadStart(lua_State* L){
        if (curry(L, luaUtilsPadStart, 3)) return 1;
        padImpl(L, true);
        return 1;
    }

    int luaUtilsPadEnd(lua_State* L){
        if (curry(L, luaUtilsPadEnd, 3)) return 1;
        padImpl(L, false);
        return 1;
    }

    int luaUtilsRandom(lua_State* L){
        lua_pushnumber(L, AnimUtils::random(numberArg(L, 1, 0), numberArg(L, 2, 1), (int)numberArg(L, 3, 0)));
        return 1;
    }

    // a seeded generator: an upvalue holds its state
    int luaSeededRandomCall(lua_State* L){
        double* state = (double*)lua_touserdata(L, lua_upvalueindex(1));
        const double min = numberArg(L, 1, lua_tonumber(L, lua_upvalueindex(2)));
        const double max = numberArg(L, 2, lua_tonumber(L, lua_upvalueindex(3)));
        const int decimals = (int)numberArg(L, 3, lua_tonumber(L, lua_upvalueindex(4)));
        lua_pushnumber(L, AnimUtils::seededRandomNext(*state, min, max, decimals));
        return 1;
    }

    int luaUtilsCreateSeededRandom(lua_State* L){
        double* state = (double*)lua_newuserdatauv(L, sizeof(double), 0);
        *state = numberArg(L, 1, 0);
        lua_pushnumber(L, numberArg(L, 2, 0));
        lua_pushnumber(L, numberArg(L, 3, 1));
        lua_pushnumber(L, numberArg(L, 4, 0));
        lua_pushcclosure(L, luaSeededRandomCall, 4);
        return 1;
    }

    // randomPick(table | string): an item / a character
    int luaUtilsRandomPick(lua_State* L){
        if (lua_type(L, 1) == LUA_TSTRING){
            size_t length = 0;
            const char* text = lua_tolstring(L, 1, &length);
            if (!length){
                lua_pushnil(L);
                return 1;
            }
            const size_t i = (size_t)AnimUtils::random(0, (double)length - 1, 0);
            lua_pushlstring(L, text + i, 1);
            return 1;
        }
        if (lua_type(L, 1) != LUA_TTABLE){
            lua_pushnil(L);
            return 1;
        }
        const lua_Integer length = (lua_Integer)lua_rawlen(L, 1);
        if (!length){
            lua_pushnil(L);
            return 1;
        }
        lua_rawgeti(L, 1, (lua_Integer)AnimUtils::random(0, (double)length - 1, 0) + 1);
        return 1;
    }

    // shuffle(table): in place, returns it
    int luaUtilsShuffle(lua_State* L){
        if (lua_type(L, 1) != LUA_TTABLE){
            lua_pushvalue(L, 1);
            return 1;
        }
        lua_Integer m = (lua_Integer)lua_rawlen(L, 1);
        while (m){
            const lua_Integer i = (lua_Integer)AnimUtils::random(0, (double)(--m), 0);
            lua_rawgeti(L, 1, m + 1);
            lua_rawgeti(L, 1, i + 1);
            lua_rawseti(L, 1, m + 1);
            lua_rawseti(L, 1, i + 1);
        }
        lua_pushvalue(L, 1);
        return 1;
    }

    bool utilsSetImpl(lua_State* L){
        if (lua_type(L, 2) != LUA_TTABLE){
            lua_pushstring(L, "anime.utils.set(targets, props): props must be a table");
            return false;
        }
        UIElement root = animeRootOf(L, 2);
        AnimTargetList targets;
        toTargets(L, 1, root, targets);
        pushTimer(L, AnimUtils::set(targets, toParam(L, 2, "")));
        return true;
    }

    int luaUtilsSet(lua_State* L){
        if (!utilsSetImpl(L))
            return lua_error(L);
        return 1;
    }

    // get(targets, property[, unit]): unit a string converts, false gives the number
    void utilsGetImpl(lua_State* L){
        AnimTargetList targets;
        toTargets(L, 1, UIElement(), targets);
        const std::string property = stringArg(L, 2);
        const bool numberOnly = lua_isboolean(L, 3) && !lua_toboolean(L, 3);
        const std::string unit = lua_type(L, 3) == LUA_TSTRING ? stringArg(L, 3) : std::string();
        AnimRawValue value = AnimUtils::get(targets, property, unit, numberOnly);
        if (value.isNumber()){
            lua_pushnumber(L, value.number);
        } else if (value.isString()){
            pushString(L, value.text);
        } else {
            lua_pushnil(L);
        }
    }

    int luaUtilsGet(lua_State* L){
        utilsGetImpl(L);
        return 1;
    }

    // remove(targets[, animation][, property])
    void utilsRemoveImpl(lua_State* L){
        AnimTargetList targets;
        toTargets(L, 1, UIElement(), targets);
        std::shared_ptr<AnimTimer> renderable = timerOf(L, 2);
        std::string property;
        if (lua_type(L, 2) == LUA_TSTRING)
            property = stringArg(L, 2);
        else if (lua_type(L, 3) == LUA_TSTRING)
            property = stringArg(L, 3);
        AnimUtils::remove(targets, renderable.get(), property);
    }

    int luaUtilsRemove(lua_State* L){
        utilsRemoveImpl(L);
        lua_pushvalue(L, 1);
        return 1;
    }

    void setTableFunction(lua_State* L, const char* name, lua_CFunction fn){
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
    }

    // anime.utils
    void pushUtilsTable(lua_State* L){
        lua_createtable(L, 0, 20);
        setTableFunction(L, "set", luaUtilsSet);
        setTableFunction(L, "get", luaUtilsGet);
        setTableFunction(L, "remove", luaUtilsRemove);
        setTableFunction(L, "random", luaUtilsRandom);
        setTableFunction(L, "createSeededRandom", luaUtilsCreateSeededRandom);
        setTableFunction(L, "randomPick", luaUtilsRandomPick);
        setTableFunction(L, "shuffle", luaUtilsShuffle);
        setTableFunction(L, "clamp", luaUtilsClamp);
        setTableFunction(L, "round", luaUtilsRound);
        setTableFunction(L, "snap", luaUtilsSnap);
        setTableFunction(L, "wrap", luaUtilsWrap);
        setTableFunction(L, "mapRange", luaUtilsMapRange);
        setTableFunction(L, "lerp", luaUtilsLerp);
        setTableFunction(L, "damp", luaUtilsDamp);
        setTableFunction(L, "degToRad", luaUtilsDegToRad);
        setTableFunction(L, "radToDeg", luaUtilsRadToDeg);
        setTableFunction(L, "roundPad", luaUtilsRoundPad);
        setTableFunction(L, "padStart", luaUtilsPadStart);
        setTableFunction(L, "padEnd", luaUtilsPadEnd);
        setTableFunction(L, "stagger", luaStagger);
    }

    // anime.eases: every name of Anime's eases table
    void pushEasesTable(lua_State* L){
        lua_createtable(L, 0, (int)AnimEasing::getNames().size());
        for (const std::string& name : AnimEasing::getNames()){
            const bool parametric = AnimEasing::hasParameters(name);
            pushEase(L, AnimEaseFunction(parametric ? nullptr : AnimEasing::get(name), name, parametric));
            lua_setfield(L, -2, name.c_str());
        }
    }

    uint64_t resetListener = 0;

}

size_t AnimLua::getLiveLuaRefs(){
    size_t count = 0;
    for (LuaRefHolder* ref : animeLiveRefs()){
        if (ref->isAlive())
            count++;
    }
    return count;
}

void LuaBinding::registerHtmlUIAnimeClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // the play stop and shutdown free every Lua reference anime holds
    if (!resetListener){
        resetListener = AnimEngine::instance().addResetListener([](){
            releaseAllRefs();
            tableTargets().clear();
        });
    }

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimText>("AnimeTextSplitter")
        .addProperty("chars", &AnimText::getChars)
        .addProperty("words", &AnimText::getWords)
        .addProperty("lines", &AnimText::getLines)
        .addProperty("target", &AnimText::getTarget)
        .addProperty("ready", &AnimText::isReady)
        .addFunction("split", &AnimText::split)
        .addFunction("refresh", &AnimText::refresh)
        .addFunction("revert", &AnimText::revert)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimEaseFunction>("AnimeEase")
        .addProperty("name", &AnimEaseFunction::getName)
        .addFunction("evaluate", &AnimEaseFunction::evaluate)
        .addFunction("__call", &luaEaseCall)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimStaggerFunction>("AnimeStagger")
        .addFunction("__call", &luaStaggerCall)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimSpring>("AnimeSpring")
        .addProperty("bounce", &AnimSpring::getBounce, &AnimSpring::setBounce)
        .addProperty("duration", &AnimSpring::getDuration, &AnimSpring::setDuration)
        .addProperty("stiffness", &AnimSpring::getStiffness, &AnimSpring::setStiffness)
        .addProperty("damping", &AnimSpring::getDamping, &AnimSpring::setDamping)
        .addProperty("mass", &AnimSpring::getMass, &AnimSpring::setMass)
        .addProperty("velocity", &AnimSpring::getVelocity, &AnimSpring::setVelocity)
        .addProperty("settlingDuration", &AnimSpring::getSettlingDuration)
        .addFunction("ease", &AnimSpring::ease)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<AnimTimer>("AnimeTimer")
        .addProperty("id", &timerId)
        .addProperty("currentTime", &AnimTimer::getCurrentTime, &AnimTimer::setCurrentTime)
        .addProperty("iterationCurrentTime", &AnimTimer::getIterationCurrentTime, &AnimTimer::setIterationCurrentTime)
        .addProperty("progress", &AnimTimer::getProgress, &AnimTimer::setProgress)
        .addProperty("iterationProgress", &AnimTimer::getIterationProgress, &AnimTimer::setIterationProgress)
        .addProperty("currentIteration", &AnimTimer::getCurrentIteration, &AnimTimer::setCurrentIteration)
        .addProperty("reversed", &AnimTimer::isReversed, &AnimTimer::setReversed)
        .addProperty("speed", &AnimTimer::getSpeed, &AnimTimer::setSpeed)
        .addProperty("fps", &AnimTimer::getFps, &AnimTimer::setFps)
        .addProperty("cancelled", &AnimTimer::isCancelled, &AnimTimer::setCancelled)
        .addProperty("duration", &AnimTimer::getDuration)
        .addProperty("iterationDuration", &AnimTimer::getIterationDuration)
        .addProperty("iterationCount", &timerIterationCount)
        .addProperty("delay", &AnimTimer::getDelay)
        .addProperty("deltaTime", &AnimTimer::getDeltaTime)
        .addProperty("paused", &AnimTimer::isPaused)
        .addProperty("began", &AnimTimer::hasBegun)
        .addProperty("completed", &AnimTimer::isCompleted)
        .addProperty("backwards", &AnimTimer::isBackwards)
        .addFunction("play", &timerPlay<AnimTimer>)
        .addFunction("pause", &timerPause<AnimTimer>)
        .addFunction("resume", &timerResume<AnimTimer>)
        .addFunction("restart", &timerRestart<AnimTimer>)
        .addFunction("reverse", &timerReverse<AnimTimer>)
        .addFunction("alternate", &timerAlternate<AnimTimer>)
        .addFunction("cancel", &timerCancel<AnimTimer>)
        .addFunction("revert", &timerRevert<AnimTimer>)
        .addFunction("reset", &timerReset<AnimTimer>)
        .addFunction("init", &timerInit<AnimTimer>)
        .addFunction("refresh", &timerRefresh<AnimTimer>)
        .addFunction("resetTime", &timerResetTime<AnimTimer>)
        .addFunction("stretch", &timerStretch<AnimTimer>)
        .addFunction("seek", &timerSeek<AnimTimer>)
        .addFunction("complete", &timerComplete<AnimTimer>)
        .addFunction("andThen", &luaThen)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<AnimAnimation, AnimTimer>("AnimeAnimation")
        .addProperty("targets", &animationTargets)
        .addFunction("play", &timerPlay<AnimAnimation>)
        .addFunction("pause", &timerPause<AnimAnimation>)
        .addFunction("resume", &timerResume<AnimAnimation>)
        .addFunction("restart", &timerRestart<AnimAnimation>)
        .addFunction("reverse", &timerReverse<AnimAnimation>)
        .addFunction("alternate", &timerAlternate<AnimAnimation>)
        .addFunction("cancel", &timerCancel<AnimAnimation>)
        .addFunction("revert", &timerRevert<AnimAnimation>)
        .addFunction("reset", &timerReset<AnimAnimation>)
        .addFunction("init", &timerInit<AnimAnimation>)
        .addFunction("refresh", &timerRefresh<AnimAnimation>)
        .addFunction("resetTime", &timerResetTime<AnimAnimation>)
        .addFunction("stretch", &timerStretch<AnimAnimation>)
        .addFunction("seek", &timerSeek<AnimAnimation>)
        .addFunction("complete", &timerComplete<AnimAnimation>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<AnimTimeline, AnimTimer>("AnimeTimeline")
        .addProperty("labels", &timelineLabels)
        .addFunction("add", &luaTimelineAdd)
        .addFunction("set", &luaTimelineSet)
        .addFunction("call", &luaTimelineCall)
        .addFunction("label", &timelineLabel)
        .addFunction("sync", &luaTimelineSync)
        .addFunction("remove", &luaTimelineRemove)
        .addFunction("play", &timerPlay<AnimTimeline>)
        .addFunction("pause", &timerPause<AnimTimeline>)
        .addFunction("resume", &timerResume<AnimTimeline>)
        .addFunction("restart", &timerRestart<AnimTimeline>)
        .addFunction("reverse", &timerReverse<AnimTimeline>)
        .addFunction("alternate", &timerAlternate<AnimTimeline>)
        .addFunction("cancel", &timerCancel<AnimTimeline>)
        .addFunction("revert", &timerRevert<AnimTimeline>)
        .addFunction("reset", &timerReset<AnimTimeline>)
        .addFunction("init", &timerInit<AnimTimeline>)
        .addFunction("refresh", &timerRefresh<AnimTimeline>)
        .addFunction("resetTime", &timerResetTime<AnimTimeline>)
        .addFunction("stretch", &timerStretch<AnimTimeline>)
        .addFunction("seek", &timerSeek<AnimTimeline>)
        .addFunction("complete", &timerComplete<AnimTimeline>)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Anime>("anime")
        .addStaticFunction("animate", &luaAnimate)
        .addStaticFunction("splitText", &luaSplitText)
        .addStaticFunction("createTimeline", &luaCreateTimeline)
        .addStaticFunction("createTimer", &luaCreateTimer)
        .addStaticFunction("createSpring", &luaCreateSpring)
        .addStaticFunction("spring", &luaCreateSpring)
        .addStaticFunction("stagger", &luaStagger)
        .addStaticFunction("cubicBezier", &luaCubicBezier)
        .addStaticFunction("steps", &luaSteps)
        .addStaticFunction("linear", &luaLinear)
        .addStaticFunction("irregular", &luaIrregular)
        .endClass();

    // anime.eases and anime.utils: plain tables on the class table
    lua_getglobal(L, "anime");
    if (lua_istable(L, -1)){
        lua_pushstring(L, "eases");
        pushEasesTable(L);
        lua_rawset(L, -3);
        lua_pushstring(L, "utils");
        pushUtilsTable(L);
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);

#endif //DISABLE_LUA_BINDINGS
}

#endif // JASPERENGINE_HTMLUI
