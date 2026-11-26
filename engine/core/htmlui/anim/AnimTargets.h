// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMTARGETS_H
#define ANIMTARGETS_H

// What the anime API animates (Anime.js 4.5.0 core/targets.js): HTML UI elements (UIElement,
// lists of them, CSS selectors resolved against the loaded documents of the current scene)
// and plain objects (Lua tables through the binding's TableTarget, AnimObjectTarget in C++).
// A target is shared by every animation of the same element or object, and holds the
// per-property tween lists the compositions (replace, blend) work on.

#include "Export.h"
#include "AnimParams.h"
#include "AnimValue.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine {

    class UIElement;
    class Scene;
    struct AnimTween;

    // Anime's tweenTypes
    enum class AnimTweenType { OBJECT = 0, ATTRIBUTE = 1, CSS = 2, TRANSFORM = 3, CSS_VAR = 4 };

    // a rendered value given to a target
    struct AnimWrite {
        AnimValueType valueType = AnimValueType::NUMBER;
        double number = 0.0;                           // NUMBER and UNIT values (modified)
        std::string text;                              // the value as Anime writes it ("12px", "rgba(...)", complex strings); empty for NUMBER
        const std::vector<double>* numbers = nullptr;  // COLOR channels (0-255 r g b, 0-1 a) / COMPLEX numbers
        const std::string* unit = nullptr;             // UNIT
    };

    // a tween list of one property of one target (Anime's TweenPropertySiblings)
    struct AnimTweenSiblings {
        AnimTween* head = nullptr;
        AnimTween* tail = nullptr;
    };

    class JASPERENGINE_API AnimTarget : public std::enable_shared_from_this<AnimTarget> {
    public:
        enum class Kind { OBJECT, ELEMENT, TIMER };

        AnimTarget() = default;
        AnimTarget(const AnimTarget&) = delete;
        AnimTarget& operator=(const AnimTarget&) = delete;
        virtual ~AnimTarget();

        virtual Kind getKind() const { return Kind::OBJECT; }
        // false once an element is gone: its tweens are skipped
        virtual bool isValid() const { return true; }

        // getTweenType: how a property of this target is animated (objects: OBJECT)
        virtual AnimTweenType getTweenType(const std::string& property) const;
        // sanitizePropertyName: the name the tweens use (CSS: kebab-case, x -> translateX)
        virtual std::string sanitizeProperty(const std::string& property, AnimTweenType type) const;
        // getOriginalAnimatableValue; *inlineValue (when given) receives what revert() restores
        virtual AnimRawValue getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue) = 0;
        virtual void setValue(const std::string& property, AnimTweenType type, const AnimWrite& value) = 0;
        // revertValues: restores the value before the animation; nil removes it (elements) or leaves it (objects)
        virtual void revertValue(const std::string& property, AnimTweenType type, const AnimRawValue& original);
        // convertValueUnit: value to unit (angles everywhere; lengths on elements); other
        // conversions keep the number and take the unit
        virtual void convertUnit(AnimDecomposed& value, const std::string& unit, bool force);
        // a `var(--name[, fallback])` function value: the custom property on the target (elements)
        virtual AnimRawValue resolveCssVariable(const std::string& name);
        // the position stagger({grid: true}) uses: the centre of an element's box, x/y(/z)
        // fields of an object; false when none
        virtual bool getStaggerPosition(double& x, double& y, double& z, bool& has3D);
        // identity for selector deduplication and the binding (element pointer, table, timer)
        virtual const void* getIdentity() const { return this; }

        // composition lists (Anime's lookups._rep / lookups._add), per sanitized property
        std::unordered_map<std::string, AnimTweenSiblings> replaceSiblings;
        std::unordered_map<std::string, AnimTweenSiblings> additiveSiblings;
    };

    // a C++ object target: named numeric or string values, read and written like a JS object
    class JASPERENGINE_API AnimObjectTarget : public AnimTarget {
    public:
        AnimObjectTarget() = default;
        AnimObjectTarget(std::initializer_list<std::pair<const std::string, AnimRawValue>> values);

        AnimRawValue getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue) override;
        void setValue(const std::string& property, AnimTweenType type, const AnimWrite& value) override;
        void revertValue(const std::string& property, AnimTweenType type, const AnimRawValue& original) override;
        bool getStaggerPosition(double& x, double& y, double& z, bool& has3D) override;

        AnimRawValue get(const std::string& property) const;   // nil when absent
        double getNumber(const std::string& property) const;    // NaN unless a number
        void set(const std::string& property, const AnimRawValue& value);
        const std::map<std::string, AnimRawValue>& getValues() const { return values; }

    private:
        std::map<std::string, AnimRawValue> values;
    };

    class JASPERENGINE_API AnimTargets {
    public:
        // Private previews resolve unrooted selectors only in their own scene.
        class JASPERENGINE_API ScopedSceneTargets {
        public:
            explicit ScopedSceneTargets(Scene* scene);
            ~ScopedSceneTargets();
            ScopedSceneTargets(const ScopedSceneTargets&) = delete;
            ScopedSceneTargets& operator=(const ScopedSceneTargets&) = delete;
        private:
            Scene* previous;
        };

        // the shared target of an element (invalid element: nullptr)
        static AnimTargetPtr fromElement(const UIElement& element);
        static AnimTargetList fromElements(const std::vector<UIElement>& elements);
        // querySelectorAll on root's document subtree, or (root invalid) on every loaded document
        // of the game's scenes: the current scene, the running layers, then the scenes created
        // but not running yet (SceneManager); a bad selector logs a warning
        static AnimTargetList fromSelector(const std::string& selector, const UIElement& root);
        static AnimTargetList fromSelector(const std::string& selector);
        // the shared target animating an anime object's currentTime/progress (Timeline::sync)
        static AnimTargetPtr fromTimer(const std::shared_ptr<AnimTimer>& timer);
        // the element behind an element target (an invalid UIElement otherwise)
        static UIElement getElement(const AnimTargetPtr& target);
        // targets without duplicates, in order (Anime's parseTargets)
        static void appendUnique(AnimTargetList& list, const AnimTargetPtr& target);
        // live element targets (tests)
        static size_t getElementTargetCount();
    };

}

#endif //ANIMTARGETS_H
