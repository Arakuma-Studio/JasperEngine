// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimTargets.h"

#include "AnimCommon.h"
#include "AnimProperty.h"
#include "AnimTimer.h"
#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/UIDocumentComponent.h"
#include "manager/SceneManager.h"
#include "object/ui/UIElementInternal.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;

namespace {

    const double PI = 3.14159265358979323846;
    Scene* animeSelectorScene = nullptr;

    // Anime's sync() target: an anime object's currentTime (and progress, speed)
    class TimerTarget : public AnimTarget {
    public:
        explicit TimerTarget(std::shared_ptr<AnimTimer> timer): timer(std::move(timer)) {}

        Kind getKind() const override{ return Kind::TIMER; }
        const void* getIdentity() const override{ return timer.get(); }

        AnimRawValue getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue) override{
            (void)type;
            double value = 0;
            if (property == "currentTime") value = timer->getCurrentTime();
            else if (property == "progress") value = timer->getProgress();
            else if (property == "iterationProgress") value = timer->getIterationProgress();
            else if (property == "speed" || property == "playbackRate") value = timer->getSpeed();
            if (value && inlineValue)
                *inlineValue = AnimRawValue(value);
            return AnimRawValue(value);
        }

        void setValue(const std::string& property, AnimTweenType type, const AnimWrite& value) override{
            (void)type;
            const double v = value.valueType == AnimValueType::NUMBER ? value.number : AnimMath::parseFloat(value.text);
            if (std::isnan(v))
                return;
            if (property == "currentTime") timer->setCurrentTime(v);
            else if (property == "progress") timer->setProgress(v);
            else if (property == "iterationProgress") timer->setIterationProgress(v);
            else if (property == "speed" || property == "playbackRate") timer->setSpeed(v);
        }

    private:
        std::shared_ptr<AnimTimer> timer;
    };

    // one shared target per synced timer while it is animated
    std::unordered_map<const AnimTimer*, std::weak_ptr<AnimTarget>>& timerTargets(){
        static auto* targets = new std::unordered_map<const AnimTimer*, std::weak_ptr<AnimTarget>>();
        return *targets;
    }

    void querySelectorAll(Rml::Element* root, const std::string& selector, AnimTargetList& out){
        Rml::ElementList elements;
        root->QuerySelectorAll(elements, selector);
        for (Rml::Element* element : elements)
            AnimTargets::appendUnique(out, AnimProperty::elementTarget(element));
    }

}

AnimTargets::ScopedSceneTargets::ScopedSceneTargets(Scene* scene) : previous(animeSelectorScene){
    animeSelectorScene = scene;
}

AnimTargets::ScopedSceneTargets::~ScopedSceneTargets(){
    animeSelectorScene = previous;
}

AnimTarget::~AnimTarget() = default;

AnimTweenType AnimTarget::getTweenType(const std::string& property) const{
    (void)property;
    return AnimTweenType::OBJECT;
}

std::string AnimTarget::sanitizeProperty(const std::string& property, AnimTweenType type) const{
    (void)type;
    return property;
}

void AnimTarget::revertValue(const std::string& property, AnimTweenType type, const AnimRawValue& original){
    // objects: the original value comes back when there was one (Anime's revertValues)
    if (original.isNil() || (original.isString() && original.text.empty()))
        return;
    AnimWrite write;
    if (original.isNumber()){
        write.valueType = AnimValueType::NUMBER;
        write.number = original.number;
    } else {
        write.valueType = AnimValueType::COMPLEX;
        write.text = original.text;
    }
    setValue(property, type, write);
}

void AnimTarget::convertUnit(AnimDecomposed& value, const std::string& unit, bool force){
    (void)force;
    static const std::unordered_map<std::string, double> angles = {{"deg", 1}, {"rad", 180 / PI}, {"turn", 360}};
    if (value.t == AnimValueType::UNIT && value.u == unit)
        return;
    auto from = angles.find(value.u);
    auto to = angles.find(unit);
    if (from != angles.end() && to != angles.end())
        value.n = value.n * from->second / to->second;
    value.u = unit;
}

AnimRawValue AnimTarget::resolveCssVariable(const std::string& name){
    (void)name;
    return AnimRawValue();
}

bool AnimTarget::getStaggerPosition(double& x, double& y, double& z, bool& has3D){
    (void)x; (void)y; (void)z; (void)has3D;
    return false;
}

// ---------------------------------------------------------------------------------------------

AnimObjectTarget::AnimObjectTarget(std::initializer_list<std::pair<const std::string, AnimRawValue>> initial): values(initial) {}

AnimRawValue AnimObjectTarget::getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue){
    (void)type;
    AnimRawValue value = get(property);
    if (value.isTruthy() && inlineValue)
        *inlineValue = value;
    // Anime: target[prop] || 0
    return value.isTruthy() ? value : AnimRawValue(0.0);
}

void AnimObjectTarget::setValue(const std::string& property, AnimTweenType type, const AnimWrite& value){
    (void)type;
    if (value.valueType == AnimValueType::NUMBER){
        values[property] = AnimRawValue(value.number);
    } else {
        values[property] = AnimRawValue(value.text);
    }
}

void AnimObjectTarget::revertValue(const std::string& property, AnimTweenType type, const AnimRawValue& original){
    AnimTarget::revertValue(property, type, original);
}

bool AnimObjectTarget::getStaggerPosition(double& x, double& y, double& z, bool& has3D){
    AnimRawValue vx = get("x"), vy = get("y"), vz = get("z");
    if (!vx.isNumber() || !vy.isNumber() || std::isnan(vx.number) || std::isnan(vy.number))
        return false;
    x = vx.number;
    y = vy.number;
    has3D = vz.isNumber() && !std::isnan(vz.number);
    z = has3D ? vz.number : 0;
    return true;
}

AnimRawValue AnimObjectTarget::get(const std::string& property) const{
    auto it = values.find(property);
    return it != values.end() ? it->second : AnimRawValue();
}

double AnimObjectTarget::getNumber(const std::string& property) const{
    AnimRawValue value = get(property);
    return value.isNumber() ? value.number : std::nan("");
}

void AnimObjectTarget::set(const std::string& property, const AnimRawValue& value){
    values[property] = value;
}

// ---------------------------------------------------------------------------------------------

AnimTargetPtr AnimTargets::fromElement(const UIElement& element){
    return AnimProperty::elementTarget(UIElementAccess::get(element));
}

AnimTargetList AnimTargets::fromElements(const std::vector<UIElement>& elements){
    AnimTargetList list;
    for (const UIElement& element : elements)
        appendUnique(list, fromElement(element));
    return list;
}

AnimTargetList AnimTargets::fromSelector(const std::string& selector, const UIElement& root){
    AnimTargetList list;
    if (selector.empty())
        return list;
    Rml::Element* rootElement = UIElementAccess::get(root);
    if (rootElement){
        querySelectorAll(rootElement, selector, list);
        return list;
    }
    // The documents of every scene of the game, the current scene's first: a HUD is often a
    // child scene layered over the scene whose script animates it, and a scene's scripts start
    // (init) before the scene runs, so the game's created scenes (SceneManager) count too
    std::vector<Scene*> scenes;
    auto add = [&scenes](Scene* scene){
        if (scene && std::find(scenes.begin(), scenes.end(), scene) == scenes.end())
            scenes.push_back(scene);
    };
    if (animeSelectorScene){
        add(animeSelectorScene);
    } else {
        add(Engine::getScene());
        add(Engine::getMainScene());
        for (Scene* layer : Engine::getSceneLayers())
            add(layer);
        for (Scene* created : SceneManager::getScenePtrs())
            add(created);
    }
    for (Scene* scene : scenes){
        std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
        if (!system)
            continue;
        auto components = scene->getComponentArray<UIDocumentComponent>();
        for (size_t i = 0; i < components->size(); i++){
            Rml::ElementDocument* document = system->getDocument(components->getEntity(i));
            if (document)
                querySelectorAll(document, selector, list);
        }
    }
    return list;
}

AnimTargetList AnimTargets::fromSelector(const std::string& selector){
    return fromSelector(selector, UIElement());
}

AnimTargetPtr AnimTargets::fromTimer(const std::shared_ptr<AnimTimer>& timer){
    if (!timer)
        return nullptr;
    auto& targets = timerTargets();
    auto it = targets.find(timer.get());
    if (it != targets.end()){
        AnimTargetPtr existing = it->second.lock();
        if (existing)
            return existing;
    }
    // drop expired entries now and then
    for (auto e = targets.begin(); e != targets.end();){
        e = e->second.expired() ? targets.erase(e) : std::next(e);
    }
    AnimTargetPtr target = std::make_shared<TimerTarget>(timer);
    targets[timer.get()] = target;
    return target;
}

UIElement AnimTargets::getElement(const AnimTargetPtr& target){
    if (!target)
        return UIElement();
    return UIElementAccess::wrap(AnimProperty::getElement(*target));
}

void AnimTargets::appendUnique(AnimTargetList& list, const AnimTargetPtr& target){
    if (!target)
        return;
    for (const AnimTargetPtr& existing : list){
        if (existing == target)
            return;
    }
    list.push_back(target);
}

size_t AnimTargets::getElementTargetCount(){
    return AnimProperty::getElementTargetCount();
}
