// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_STAND_INS_H
#define COMPAT_RUNTIME_STAND_INS_H

// The CSS rewriter writes properties and decorators the compat runtime registers
// (phase-2 §3.11.1, §3.11.3: list-style-*, object-fit, image-rendering, the SVG paint
// properties, -html-animation-fill; html-layer, html-shape, html-border-image). Tests that
// parse its RCSS with RmlUi register stand-ins for whichever of them the runtime has not
// registered (the real ones take precedence: names already registered are left alone).

#include <RmlUi/Core.h>
#include <RmlUi/Core/Decorator.h>

#include <string>

namespace jasperengine::test::compat {

    // a decorator taking n positional string arguments
    class StandInDecorator : public Rml::DecoratorInstancer {
    public:
        explicit StandInDecorator(int count){
            std::string names;
            for (int i = 0; i < count; i++){
                RegisterProperty("argument-" + std::to_string(i), "").AddParser("string");
                names += (i ? ", " : "") + std::string("argument-") + std::to_string(i);
            }
            RegisterShorthand("decorator", names, Rml::ShorthandType::FallThrough);
        }
        Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary&,
                                                         const Rml::DecoratorInstancerInterface&) override {
            return nullptr;
        }
    };

    inline void registerCompatRuntimeStandIns(){
        using Rml::StyleSheetSpecification;
        auto property = [](const char* name, const char* initial, bool inherited, const char* parser, const char* parameters){
            if (StyleSheetSpecification::GetProperty(name) || StyleSheetSpecification::GetShorthand(name))
                return;
            Rml::PropertyDefinition& definition = StyleSheetSpecification::RegisterProperty(name, initial, inherited, false);
            if (parameters)
                definition.AddParser(parser, parameters);
            else
                definition.AddParser(parser);
        };
        property("list-style-type", "disc", true, "string", nullptr);
        property("list-style-position", "outside", true, "keyword", "inside, outside");
        property("list-style-image", "none", true, "string", nullptr);
        property("object-fit", "fill", false, "keyword", "fill, contain, cover, none, scale-down");
        property("object-position", "50% 50%", false, "string", nullptr);
        property("image-rendering", "auto", true, "keyword", "auto, smooth, high-quality, pixelated, crisp-edges");
        property("-html-animation-fill", "none", false, "keyword", "none, forwards, backwards, both");
        for (const char* name : {"fill", "stroke", "stroke-width", "stroke-opacity", "fill-opacity", "stroke-linecap", "stroke-linejoin",
                                 "stroke-dasharray"})
            property(name, "", true, "string", nullptr);
        static StandInDecorator layer(10), shape(1), borderImage(18);
        if (!Rml::Factory::GetDecoratorInstancer("html-layer"))
            Rml::Factory::RegisterDecoratorInstancer("html-layer", &layer);
        if (!Rml::Factory::GetDecoratorInstancer("html-shape"))
            Rml::Factory::RegisterDecoratorInstancer("html-shape", &shape);
        if (!Rml::Factory::GetDecoratorInstancer("html-border-image"))
            Rml::Factory::RegisterDecoratorInstancer("html-border-image", &borderImage);
    }

}

#endif //COMPAT_RUNTIME_STAND_INS_H
