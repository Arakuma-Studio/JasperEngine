// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTINSTANCERS_H
#define HTMLUI_EFFECTS_EFFECTINSTANCERS_H

// The RmlUi side of the effects (phase-5 spec 3.6), private to the HTML UI:
//  - the typed property parsers jfx-number, jfx-integer, jfx-percentage, jfx-length, jfx-angle,
//    jfx-time, jfx-color, jfx-position-x, jfx-position-y, jfx-enum, jfx-boolean, jfx-palette
//    (a slot written v(<css>), vx(...), vy(...) or bare);
//  - per effect, a decorator instancer (paint effects) and a filter instancer (every effect)
//    named jfx-<name>: one RmlUi property per slot, a FallThrough `decorator`/`filter`
//    shorthand over them all, so RmlUi interpolates two values of one effect slot by slot;
//  - the fallback instancers named `effect` (a whole effect() that reached RmlUi without the
//    compatibility layer: a custom property holding it, a script path), which parse it at
//    instancing and log their diagnostics to the Console.

#include "Export.h"
#include "EffectRuntime.h"

#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Filter.h>
#include <RmlUi/Core/PropertyParser.h>

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::htmlui {

    class JASPERENGINE_INTERNAL EffectPropertyParser : public Rml::PropertyParser {
    public:
        // axis: 'x' / 'y' for the two halves of a position, 0 otherwise
        EffectPropertyParser(EffectParamType type, char axis = 0);
        bool ParseValue(Rml::Property& property, const Rml::String& value, const Rml::ParameterMap& parameters) const override;
        static const char* parserName(EffectParamType type, char axis = 0);

    private:
        EffectParamType type;
        char axis;
    };

    // the parameter values of a property dictionary (typed slots or a raw effect() text), with
    // the problems found (logged with the element's address when the effect is used)
    struct EffectSnapshotResult {
        std::shared_ptr<EffectParamsSnapshot> snapshot;
        std::vector<std::string> problems;
    };

    class JASPERENGINE_INTERNAL EffectDecorator : public Rml::Decorator {
    public:
        EffectDecorator(std::shared_ptr<const EffectParamsSnapshot> snapshot, std::vector<std::string> problems);
        Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override;
        void ReleaseElementData(Rml::DecoratorDataHandle element_data) const override;
        void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle element_data) const override;
        // fork (P5-2): the effect clips itself to its area's rounded box (or draws beyond it,
        // @geometry bounds); the background layer needs no clip mask
        bool PaintsOwnClip() const override { return true; }
        const std::shared_ptr<const EffectParamsSnapshot>& getSnapshot() const { return snapshot; }

    private:
        std::shared_ptr<const EffectParamsSnapshot> snapshot;
        std::vector<std::string> problems;
        mutable bool reported = false;
    };

    class JASPERENGINE_INTERNAL EffectFilter : public Rml::Filter {
    public:
        EffectFilter(std::shared_ptr<const EffectParamsSnapshot> snapshot, std::vector<std::string> problems);
        Rml::CompiledFilter CompileFilter(Rml::Element* element) const override;
        void ExtendInkOverflow(Rml::Element* element, Rml::Rectanglef& overflow) const override;
        const std::shared_ptr<const EffectParamsSnapshot>& getSnapshot() const { return snapshot; }

    private:
        std::shared_ptr<const EffectParamsSnapshot> snapshot;
        std::vector<std::string> problems;
        mutable bool reported = false;
    };

    // the slots of one effect form: property ids in slot order
    struct EffectSlots {
        std::vector<Rml::PropertyId> params;   // per manifest parameter; a position: x then y in positionY
        std::vector<Rml::PropertyId> positionY;
        Rml::PropertyId opacity{}, timeScale{}, timeOffset{}, blend{};
        bool hasBlend = false;
    };

    // the typed instancers jfx-<name>
    class JASPERENGINE_INTERNAL EffectDecoratorInstancer : public Rml::DecoratorInstancer {
    public:
        explicit EffectDecoratorInstancer(const EffectManifest& manifest);
        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& name, const Rml::PropertyDictionary& properties,
                                                         const Rml::DecoratorInstancerInterface& instancer_interface) override;
        const EffectManifest& getManifest() const { return manifest; }
        EffectSnapshotResult snapshotOf(const Rml::PropertyDictionary& properties) const;
        // fork (P5-3): background-image transitions interpolate the effect's parameters
        bool InterpolatesAsImageLayer() const override { return true; }

    private:
        const EffectManifest& manifest;
        EffectSlots slots;
        // the 256 most recently used decorators by canonical value
        std::list<std::pair<std::string, Rml::SharedPtr<EffectDecorator>>> cache;
        std::unordered_map<std::string, std::list<std::pair<std::string, Rml::SharedPtr<EffectDecorator>>>::iterator> cacheIndex;
    };

    class JASPERENGINE_INTERNAL EffectFilterInstancer : public Rml::FilterInstancer {
    public:
        explicit EffectFilterInstancer(const EffectManifest& manifest);
        Rml::SharedPtr<Rml::Filter> InstanceFilter(const Rml::String& name, const Rml::PropertyDictionary& properties) override;
        const EffectManifest& getManifest() const { return manifest; }
        EffectSnapshotResult snapshotOf(const Rml::PropertyDictionary& properties) const;

    private:
        const EffectManifest& manifest;
        EffectSlots slots;
    };

    // the fallback instancers `effect`
    class JASPERENGINE_INTERNAL RawEffectDecoratorInstancer : public Rml::DecoratorInstancer {
    public:
        RawEffectDecoratorInstancer();
        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& name, const Rml::PropertyDictionary& properties,
                                                         const Rml::DecoratorInstancerInterface& instancer_interface) override;
        EffectSnapshotResult snapshotOf(const Rml::PropertyDictionary& properties) const;

    private:
        Rml::PropertyId valueId{};
    };

    class JASPERENGINE_INTERNAL RawEffectFilterInstancer : public Rml::FilterInstancer {
    public:
        RawEffectFilterInstancer();
        Rml::SharedPtr<Rml::Filter> InstanceFilter(const Rml::String& name, const Rml::PropertyDictionary& properties) override;
        EffectSnapshotResult snapshotOf(const Rml::PropertyDictionary& properties) const;

    private:
        Rml::PropertyId valueId{};
    };

    // An effect the element uses, as getEffects() reports it: the property ("background-image",
    // "mask-image", "filter", "backdrop-filter"), the 1-based index of the layer or function in
    // it, the form, and every parameter's canonical text (universal ones included)
    struct EffectUse {
        std::string property;
        int index = 0;
        std::string name;
        bool filterForm = false;
        std::vector<std::pair<std::string, std::string>> params;
    };
    JASPERENGINE_INTERNAL std::vector<EffectUse> collectEffectUses(Rml::Element* element);

    // the snapshot of an effect from the values of a raw effect() call (fallback instancers)
    JASPERENGINE_INTERNAL EffectSnapshotResult snapshotFromCall(const std::string& arguments, bool filterForm);
    // a snapshot's canonical texts and cache key
    JASPERENGINE_INTERNAL void finishSnapshot(EffectParamsSnapshot& snapshot);

}

#endif //HTMLUI_EFFECTS_EFFECTINSTANCERS_H
