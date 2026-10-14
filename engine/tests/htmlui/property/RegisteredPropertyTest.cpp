// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Registered custom properties (phase-5 §3.11, §4.2 p5-property): `@property` through the whole
// pipeline, compat (CompatAtProperty: parsing, hoisting, diagnostics, the fallback) and the fork
// (FORK.md P5-1: typed properties, var() lookup, transitions, keyframes).
//
// - compile: registrations and their normalized initial values, the last valid rule winning,
//   hoisting, @media/@supports, invalid rules (HTMLUI-AT at-property-invalid), unsupported syntaxes
//   falling back to phase 2's root rule (at-property-unsupported-syntax), declarations that do not
//   match, the codes, and the phase-2 behavior without ForkFeature::RegisteredProperties;
// - load: inherited and non-inherited var() lookups, invalid values at computed-value time, the
//   transition midpoint of a :hover with the UI clock pinned (HtmlUIClock::setFixedTime), keyframes
//   at fixed times, an anime-driven registered property, the conflict of two documents (first wins,
//   process-global), registration after use, @starting-style and transition-behavior, the
//   unregistered fallback, the fork feature's probe;
// - goldens: the fixtures of fixtures/p3/property (also compared with Chromium by BrowserParity)
//   rendered by the engine into golden/property/<name>[.<capture>].png.
//
// Registrations are process-global and last until RmlUi shuts down: each test uses names of its own.

#include "FixtureRunner.h"
#include "harness/ImageCompare.h"
#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"
#include "RmlSystem.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimTargets.h"
#include "htmlui/compat/CompatAtProperty.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/VirtualFiles.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    std::string page(const std::string& css, const std::string& body = "<div id='a'></div>"){
        return "<!DOCTYPE html><html><head><style>" + css + "</style></head><body>" + body + "</body></html>";
    }

    std::shared_ptr<const CompatDocument> compile(const std::string& css, ForkFeatures fork = ForkFeatures::present(),
                                                  CompatSize viewport = CompatSize{800, 600}, const std::string& body = "<div id='a'></div>"){
        CompatOptions options;
        options.fork = fork;
        options.viewport = viewport;
        return DocumentLoader::compileString(page(css, body), "@htmlui/test/property/compile.html", options);
    }

    std::string rcssOf(const CompatDocument& compiled){
        std::string rcss;
        for (const StylesheetOutput& sheet : compiled.stylesheets)
            rcss += sheet.rcss;
        return rcss;
    }

    DiagCode codeNamed(const char* name){
        const DiagCodeInfo* info = findDiagnosticCode(name);
        EXPECT_NE(info, nullptr) << name;
        return info ? info->code : DiagCode{};
    }

    std::vector<Diagnostic> withCode(const std::vector<Diagnostic>& diagnostics, const char* name){
        const DiagCode code = codeNamed(name);
        std::vector<Diagnostic> found;
        for (const Diagnostic& d : diagnostics){
            if (d.code == code)
                found.push_back(d);
        }
        return found;
    }

    const RegisteredProperty* registration(const CompatDocument& compiled, const std::string& name){
        for (const RegisteredProperty& property : compiled.registeredProperties){
            if (property.name == name)
                return &property;
        }
        return nullptr;
    }

    // One document loaded through the compat layer into a context of its own (no rendering),
    // the UI clock pinned at 0.
    struct Page {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::shared_ptr<const CompatDocument> compiled;
        std::string path;
        std::string contextName;
        std::vector<std::string> errors;   // RmlUi's errors while the page lives

        Page(const std::string& name, const std::string& css, const std::string& body): ok(HtmlUI::acquire()),
            path("@htmlui/test/property/" + name + ".html"), contextName("property-" + name){
            if (!ok)
                return;
            HtmlUIClock::setFixedTime(0.0);
            HtmlUI::getSystemInterface().setLogListener([this](Rml::Log::Type type, const std::string& message){
                if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT)
                    errors.push_back(message);
            });
            context = Rml::CreateContext(contextName, Rml::Vector2i(400, 300), &recording);
            VirtualFiles::add(path, page(css, body));
            CompatOptions options;
            options.viewport = CompatSize{400, 300};
            options.fork = ForkFeatures::present();
            document = DocumentLoader::load(context, path, options, &compiled);
            if (document){
                document->Show();
                context->Update();
            }
        }
        ~Page(){
            if (document)
                document->Close();
            if (context){
                context->Update();
                Rml::RemoveContext(contextName);
            }
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            VirtualFiles::remove(path);
            DocumentLoader::clearCache();
            HtmlUIClock::reset();
            if (ok){
                HtmlUI::getSystemInterface().setLogListener(nullptr);
                HtmlUI::release();
            }
        }

        Rml::Element* byId(const char* id) const{
            return document ? document->GetElementById(id) : nullptr;
        }
        float width(const char* id) const{
            Rml::Element* element = byId(id);
            return element ? element->GetBox().GetSize(Rml::BoxArea::Border).x : -1.0f;
        }
        std::string value(const char* id, const char* name) const{
            Rml::Element* element = byId(id);
            const Rml::Property* property = element ? element->GetProperty(name) : nullptr;
            return property ? property->ToString() : std::string("<none>");
        }
        void at(double seconds){
            HtmlUIClock::setFixedTime(seconds);
            context->Update();
        }
        const std::vector<Diagnostic>& diagnostics() const{
            return DocumentLoader::diagnosticsOf(document);
        }
    };

}

// ---- compile

TEST(RegisteredPropertyTest, ParseAndRegistration){
    std::shared_ptr<const CompatDocument> compiled = compile(
        "@property --rp-len { syntax: '<length>'; inherits: false; initial-value: 1in; }\n"
        "@property --rp-col { syntax: \"<color>\"; inherits: TRUE; initial-value: rebeccapurple; }\n"
        "@property --rp-ang { syntax: ' <angle> '; inherits: false; initial-value: 0.5turn; }\n"
        "@property --rp-num { syntax: '<number>'; inherits: false; initial-value: calc(1 + 2); }\n"
        "@property --rp-int { syntax: '<integer>'; inherits: false; initial-value: 3; }\n"
        "@property --rp-pct { syntax: '<percentage>'; inherits: false; initial-value: 50%; }\n"
        "@property --rp-lp { syntax: '<length-percentage>'; inherits: false; initial-value: 10%; }\n"
        "@property --rp-time { syntax: '<time>'; inherits: false; initial-value: 250ms; }\n"
        "@property --rp-any { syntax: '*'; inherits: false; }\n"
        "@property --rp-any2 { syntax: '*'; inherits: true; initial-value: foo bar; }\n"
        "#a { --rp-len: 2em; width: var(--rp-len); }");
    ASSERT_TRUE(compiled->compiled);
    EXPECT_TRUE(compiled->diagnostics.empty()) << (compiled->diagnostics.empty() ? "" : compiled->diagnostics[0].message);
    ASSERT_EQ(compiled->registeredProperties.size(), 10u);

    const RegisteredProperty* len = registration(*compiled, "--rp-len");
    ASSERT_NE(len, nullptr);
    EXPECT_EQ(len->type, AtPropertySyntax::Length);
    EXPECT_EQ(len->syntax, "<length>");
    EXPECT_FALSE(len->inherits);
    EXPECT_TRUE(len->hasInitialValue);
    EXPECT_EQ(len->initialValue, "1in");
    EXPECT_TRUE(len->ref.valid());
    const RegisteredProperty* col = registration(*compiled, "--rp-col");
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->type, AtPropertySyntax::Color);
    EXPECT_TRUE(col->inherits);
    EXPECT_FALSE(col->initialValue.empty());
    EXPECT_EQ(registration(*compiled, "--rp-ang")->type, AtPropertySyntax::Angle);
    EXPECT_EQ(registration(*compiled, "--rp-num")->type, AtPropertySyntax::Number);
    EXPECT_EQ(registration(*compiled, "--rp-int")->type, AtPropertySyntax::Integer);
    EXPECT_EQ(registration(*compiled, "--rp-pct")->type, AtPropertySyntax::Percentage);
    EXPECT_EQ(registration(*compiled, "--rp-lp")->type, AtPropertySyntax::LengthPercentage);
    EXPECT_EQ(registration(*compiled, "--rp-time")->type, AtPropertySyntax::Time);
    const RegisteredProperty* any = registration(*compiled, "--rp-any");
    ASSERT_NE(any, nullptr);
    EXPECT_EQ(any->type, AtPropertySyntax::Universal);
    EXPECT_FALSE(any->hasInitialValue);
    EXPECT_EQ(registration(*compiled, "--rp-any2")->initialValue, "foo bar");

    // the rules are not written: no root rule, no @property in the RCSS; the declarations stay
    const std::string rcss = rcssOf(*compiled);
    EXPECT_EQ(rcss.find("@property"), std::string::npos) << rcss;
    EXPECT_EQ(rcss.find("1in"), std::string::npos) << rcss;
    EXPECT_NE(rcss.find("--rp-len: 2em"), std::string::npos) << rcss;
}

TEST(RegisteredPropertyTest, SyntaxStrings){
    bool valid = false;
    EXPECT_EQ(CompatAtProperty::parseSyntax("<length>", valid), AtPropertySyntax::Length);
    EXPECT_TRUE(valid);
    EXPECT_EQ(CompatAtProperty::parseSyntax("*", valid), AtPropertySyntax::Universal);
    // valid CSS syntaxes the fork does not register
    for (const char* other : {"<length> | auto", "<length>+", "<color>#", "<image>", "<transform-function>", "<transform-list>", "auto",
                              "<resolution>", "<custom-ident>", "<url>", "<string>"}){
        EXPECT_FALSE(CompatAtProperty::parseSyntax(other, valid).has_value()) << other;
        EXPECT_TRUE(valid) << other;
    }
    // invalid syntax strings (Chromium drops the rule)
    for (const char* invalid : {"", "<LENGTH>", "<length", "<nonsense>", "<length> |", "| <length>", "<length> <number>", "inherit",
                                "<transform-list>+", "* | <length>"}){
        EXPECT_FALSE(CompatAtProperty::parseSyntax(invalid, valid).has_value()) << invalid;
        EXPECT_FALSE(valid) << invalid;
    }
    EXPECT_STREQ(CompatAtProperty::syntaxName(AtPropertySyntax::LengthPercentage), "<length-percentage>");
}

TEST(RegisteredPropertyTest, HoistedAndLastValidRuleWins){
    std::shared_ptr<const CompatDocument> compiled = compile(
        "#a { --rp-h: 3px; width: var(--rp-h); }\n"
        "@property --rp-h { syntax: '<length>'; inherits: false; initial-value: 1px; }\n"
        "@property --rp-h { syntax: '<color>'; inherits: true; initial-value: red; }\n"
        "@property --rp-h { syntax: '<number>'; inherits: true; }\n");
    // registration after use: the rule applies to the whole document; the last valid rule wins (Chromium),
    // the invalid third one (no initial-value) is reported and changes nothing
    const RegisteredProperty* h = registration(*compiled, "--rp-h");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(compiled->registeredProperties.size(), 1u);
    EXPECT_EQ(h->type, AtPropertySyntax::Color);
    EXPECT_TRUE(h->inherits);
    const std::vector<Diagnostic> invalid = withCode(compiled->diagnostics, "at-property-invalid");
    ASSERT_EQ(invalid.size(), 2u);   // the rule, and '3px' is not a <color>
    EXPECT_EQ(invalid[0].line, 1u);
    EXPECT_EQ(invalid[0].severity, Severity::Warning);
    EXPECT_NE(invalid[0].message.find("'3px' is not a <color>"), std::string::npos) << invalid[0].message;
    EXPECT_EQ(invalid[1].line, 4u);
    EXPECT_EQ(invalid[1].severity, Severity::Error);
    EXPECT_NE(invalid[1].message.find("initial-value is missing"), std::string::npos) << invalid[1].message;
}

TEST(RegisteredPropertyTest, InvalidRulesAreDropped){
    struct Case {
        const char* css;
        const char* message;
    };
    const Case cases[] = {
        {"@property --rp-i { inherits: false; initial-value: 1px; }", "syntax descriptor is missing"},
        {"@property --rp-i { syntax: <length>; inherits: false; initial-value: 1px; }", "syntax must be a string"},
        {"@property --rp-i { syntax: '<LENGTH>'; inherits: false; initial-value: 1px; }", "not a valid syntax"},
        {"@property --rp-i { syntax: '<length>'; initial-value: 1px; }", "inherits descriptor is missing"},
        {"@property --rp-i { syntax: '<length>'; inherits: maybe; initial-value: 1px; }", "inherits must be true or false"},
        {"@property --rp-i { syntax: '<length>'; inherits: false; }", "initial-value is missing"},
        {"@property --rp-i { syntax: '<length>'; inherits: false; initial-value: 1em; }", "computationally independent"},
        {"@property --rp-i { syntax: '<length>'; inherits: false; initial-value: red; }", "computationally independent"},
        {"@property --rp-i { syntax: '<angle>'; inherits: false; initial-value: 0; }", "computationally independent"},
        {"@property --rp-i { syntax: '<length>'; inherits: false; initial-value: var(--x); }", "cannot use var()"},
        {"@property --rp-i { syntax: '*'; inherits: false; initial-value: var(--x); }", "cannot use var()"},
        {"@property rp-i { syntax: '<length>'; inherits: false; initial-value: 1px; }", "custom property name"},
        {"div { @property --rp-i { syntax: '<length>'; inherits: false; initial-value: 1px; } }", "inside a style rule"},
    };
    for (const Case& c : cases){
        std::shared_ptr<const CompatDocument> compiled = compile(std::string(c.css) + "\n#a { width: var(--rp-i, 5px); }");
        const std::vector<Diagnostic> invalid = withCode(compiled->diagnostics, "at-property-invalid");
        ASSERT_EQ(invalid.size(), 1u) << c.css;
        EXPECT_EQ(invalid[0].severity, Severity::Error) << c.css;
        EXPECT_EQ(invalid[0].line, 1u) << c.css;
        EXPECT_GT(invalid[0].column, 0u) << c.css;
        EXPECT_NE(invalid[0].message.find(c.message), std::string::npos) << c.css << ": " << invalid[0].message;
        EXPECT_TRUE(compiled->registeredProperties.empty()) << c.css;
        // dropped as in CSS: no root rule either
        EXPECT_EQ(rcssOf(*compiled).find("--rp-i: "), std::string::npos) << c.css;
    }
}

TEST(RegisteredPropertyTest, UnsupportedSyntaxFallsBackToTheRootRule){
    std::shared_ptr<const CompatDocument> compiled = compile(
        "@property --rp-u { syntax: '<length> | auto'; inherits: false; initial-value: auto; }\n"
        "@property --rp-v { syntax: '<image>'; inherits: true; initial-value: none; }\n");
    EXPECT_TRUE(compiled->registeredProperties.empty());
    const std::vector<Diagnostic> unsupported = withCode(compiled->diagnostics, "at-property-unsupported-syntax");
    ASSERT_EQ(unsupported.size(), 2u);
    EXPECT_EQ(unsupported[0].severity, Severity::Warning);
    EXPECT_EQ(unsupported[0].line, 1u);
    EXPECT_NE(unsupported[0].message.find("'<length> | auto'"), std::string::npos) << unsupported[0].message;
    EXPECT_NE(unsupported[0].suggestion.find("<length>"), std::string::npos);
    // phase 2's emulation: the initial value on the root, and its note about inherits: false
    const std::string rcss = rcssOf(*compiled);
    EXPECT_NE(rcss.find("--rp-u: auto"), std::string::npos) << rcss;
    EXPECT_NE(rcss.find("--rp-v: none"), std::string::npos) << rcss;
    EXPECT_EQ(withCode(compiled->diagnostics, "property-approximated").size(), 1u);
}

TEST(RegisteredPropertyTest, ConditionalGroupRules){
    const std::string css =
        "@media (min-width: 600px) { @property --rp-wide { syntax: '<length>'; inherits: false; initial-value: 1px; } }\n"
        "@media (max-width: 599px) { @property --rp-narrow { syntax: '<length>'; inherits: false; initial-value: 1px; } }\n"
        "@supports (display: grid) { @property --rp-grid { syntax: '<length>'; inherits: false; initial-value: 1px; } }\n"
        "@supports (display: nonsense) { @property --rp-none { syntax: '<length>'; inherits: false; initial-value: 1px; } }\n"
        "@layer base { @property --rp-layer { syntax: '<length>'; inherits: false; initial-value: 1px; } }\n";
    // Chromium registers them while the condition holds; compat at the compile's viewport (compiled again on a resize)
    std::shared_ptr<const CompatDocument> wide = compile(css, ForkFeatures::present(), CompatSize{800, 600});
    EXPECT_NE(registration(*wide, "--rp-wide"), nullptr);
    EXPECT_EQ(registration(*wide, "--rp-narrow"), nullptr);
    EXPECT_NE(registration(*wide, "--rp-grid"), nullptr);
    EXPECT_EQ(registration(*wide, "--rp-none"), nullptr);
    EXPECT_NE(registration(*wide, "--rp-layer"), nullptr);
    EXPECT_TRUE(wide->dependsOnViewport);
    std::shared_ptr<const CompatDocument> narrow = compile(css, ForkFeatures::present(), CompatSize{400, 600});
    EXPECT_EQ(registration(*narrow, "--rp-wide"), nullptr);
    EXPECT_NE(registration(*narrow, "--rp-narrow"), nullptr);
}

TEST(RegisteredPropertyTest, DeclarationsThatDoNotMatchAreReported){
    std::shared_ptr<const CompatDocument> compiled = compile(
        "@property --rp-d { syntax: '<length>'; inherits: false; initial-value: 1px; }\n"
        "#a { --rp-d: red; }\n"
        "#a { --rp-d: 2em; --rp-d: calc(1px + 2em); --rp-d: var(--other); --rp-d: inherit; }\n");
    const std::vector<Diagnostic> invalid = withCode(compiled->diagnostics, "at-property-invalid");
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid[0].severity, Severity::Warning);
    EXPECT_EQ(invalid[0].line, 2u);
    EXPECT_EQ(invalid[0].column, 6u);
    EXPECT_NE(invalid[0].message.find("takes its initial value (initial-value 1px)"), std::string::npos) << invalid[0].message;
    // still written (a color, as compat writes colors): the fork makes it invalid at computed-value time, as in CSS
    EXPECT_NE(rcssOf(*compiled).find("--rp-d: #ff0000ff"), std::string::npos) << rcssOf(*compiled);
}

TEST(RegisteredPropertyTest, DiagnosticCodes){
    const DiagCodeInfo* unsupported = findDiagnosticCode("at-property-unsupported-syntax");
    const DiagCodeInfo* conflict = findDiagnosticCode("at-property-conflict");
    const DiagCodeInfo* invalid = findDiagnosticCode("at-property-invalid");
    ASSERT_NE(unsupported, nullptr);
    ASSERT_NE(conflict, nullptr);
    ASSERT_NE(invalid, nullptr);
    // appended to area AT after phase 2's HTMLUI-AT-0011
    EXPECT_EQ(unsupported->code.id(), "HTMLUI-AT-0012");
    EXPECT_EQ(conflict->code.id(), "HTMLUI-AT-0013");
    EXPECT_EQ(invalid->code.id(), "HTMLUI-AT-0014");
    EXPECT_EQ(unsupported->severity, Severity::Warning);
    EXPECT_EQ(conflict->severity, Severity::Warning);
    EXPECT_EQ(invalid->severity, Severity::Error);
    EXPECT_TRUE(invalid->severityByCase);
    for (const DiagCodeInfo* info : {unsupported, conflict, invalid})
        EXPECT_GT(std::string(info->help).size(), 80u);
    EXPECT_EQ(findDiagnosticCode("HTMLUI-AT-0013"), conflict);
}

TEST(RegisteredPropertyTest, WithoutTheForkFeatureItIsPhaseTwo){
    const ForkFeatures without = ForkFeatures::present().with(ForkFeature::RegisteredProperties, false);
    std::shared_ptr<const CompatDocument> compiled = compile(
        "@property --rp-p2 { syntax: '<length>'; inherits: false; initial-value: 4px; }\n#a { width: var(--rp-p2); }", without);
    EXPECT_TRUE(compiled->registeredProperties.empty());
    EXPECT_NE(rcssOf(*compiled).find("--rp-p2: 4px"), std::string::npos);
    const std::vector<Diagnostic> approximated = withCode(compiled->diagnostics, "property-approximated");
    ASSERT_EQ(approximated.size(), 1u);
    EXPECT_NE(approximated[0].message.find("inherits: false"), std::string::npos);
    EXPECT_TRUE(withCode(compiled->diagnostics, "at-property-invalid").empty());
    EXPECT_TRUE(ForkFeatures::present().has(ForkFeature::RegisteredProperties));
    EXPECT_STREQ(forkFeatureName(ForkFeature::RegisteredProperties), "RegisteredProperties");
    EXPECT_EQ(findForkFeature("RegisteredProperties"), ForkFeature::RegisteredProperties);
}

// ---- load

TEST(RegisteredPropertyTest, ForkFeatureProbe){
    ASSERT_TRUE(HtmlUI::acquire());
    const std::optional<bool> probed = ForkFeatures::probe(ForkFeature::RegisteredProperties);
    ASSERT_TRUE(probed.has_value());
    EXPECT_TRUE(*probed);
    EXPECT_EQ(Rml::GetNumContexts(), 0);
    HtmlUI::release();
}

// Registered properties take RmlUi's free property ids (256 in all; the engine's own properties use the rest): what is
// left with every engine property registered, and a registration past the last id stays unregistered with a warning.
TEST(RegisteredPropertyTest, PropertyIdCapacity){
    ASSERT_TRUE(HtmlUI::acquire());
    const int free = Rml::StyleSheetSpecification::GetNumFreePropertyIds();
    std::cout << "[ PROPERTY ] " << free << " property ids free for registered custom properties\n";
    EXPECT_GE(free, 32);
    RecordProperty("free_property_ids", free);

    // (only when this test's RmlUi ends with it: the ids it takes come back at Rml::Shutdown)
    if (HtmlUI::getReferenceCount() == 1){
        SourceFiles files;
        const uint32_t file = files.add("ui/capacity.css", "@property --cap { syntax: '<length>'; inherits: false; initial-value: 0px; }");
        std::vector<RegisteredProperty> properties;
        for (int i = 0; i <= free; i++){
            RegisteredProperty property;
            property.name = "--rp-cap-" + std::to_string(i);
            property.syntax = "<length>";
            property.type = AtPropertySyntax::Length;
            property.inherits = false;
            property.hasInitialValue = true;
            property.initialValue = "0px";
            property.ref = SourceRef{file, 0, 10};
            properties.push_back(property);
        }
        const std::vector<Diagnostic> diagnostics = CompatAtProperty::registerAll(properties, files);
        EXPECT_EQ(Rml::StyleSheetSpecification::GetNumFreePropertyIds(), 0);
        ASSERT_EQ(diagnostics.size(), 1u);
        EXPECT_EQ(diagnostics[0].code, codeNamed("at-property-unsupported-syntax"));
        EXPECT_NE(diagnostics[0].message.find("--rp-cap-" + std::to_string(free)), std::string::npos) << diagnostics[0].message;
        EXPECT_NE(diagnostics[0].message.find("no property id left"), std::string::npos) << diagnostics[0].message;
        EXPECT_EQ(Rml::StyleSheetSpecification::GetPropertyId("--rp-cap-" + std::to_string(free)), Rml::PropertyId::Invalid);
    }
    HtmlUI::release();
}

TEST(RegisteredPropertyTest, InheritedAndNonInheritedLookup){
    Page p("lookup",
           "@property --rpl-own { syntax: '<length>'; inherits: false; initial-value: 20px; }\n"
           "@property --rpl-down { syntax: '<length>'; inherits: true; initial-value: 20px; }\n"
           "@property --rpl-em { syntax: '<length>'; inherits: true; initial-value: 0px; }\n"
           "#parent { font-size: 10px; --rpl-own: 120px; --rpl-down: 120px; --rpl-em: 6em; }\n"
           "#parent > div { display: block; height: 10px; }\n"
           "#own { width: var(--rpl-own); } #down { width: var(--rpl-down); } #em { font-size: 20px; width: var(--rpl-em); }\n",
           "<div id='parent'><div id='own'></div><div id='down'></div><div id='em'></div></div>");
    ASSERT_NE(p.document, nullptr);
    EXPECT_EQ(p.width("own"), 20.0f);    // the initial value: not inherited
    EXPECT_EQ(p.width("down"), 120.0f);
    EXPECT_EQ(p.width("em"), 60.0f);     // the parent's computed 6em of 10px
    EXPECT_EQ(p.value("parent", "--rpl-own"), "120px");
    EXPECT_EQ(p.value("own", "--rpl-own"), "20px");
    EXPECT_EQ(p.value("em", "--rpl-em"), "60px");
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
    EXPECT_TRUE(withCode(p.diagnostics(), "at-property-conflict").empty());
}

TEST(RegisteredPropertyTest, InvalidValuesTakeTheInitialValue){
    Page p("invalid",
           "@property --rpi-w { syntax: '<length>'; inherits: false; initial-value: 30px; }\n"
           "@property --rpi-in { syntax: '<length>'; inherits: true; initial-value: 30px; }\n"
           "#parent { --rpi-in: 150px; } #parent > div { display: block; height: 10px; }\n"
           "#cascade { --rpi-w: 200px; --rpi-w: red; width: var(--rpi-w); }\n"
           "#substituted { --rpi-text: blue; --rpi-w: var(--rpi-text); width: var(--rpi-w); }\n"
           "#inherited { --rpi-in: red; width: var(--rpi-in); }\n",
           "<div id='parent'><div id='cascade'></div><div id='substituted'></div><div id='inherited'></div></div>");
    ASSERT_NE(p.document, nullptr);
    EXPECT_EQ(p.width("cascade"), 30.0f);
    EXPECT_EQ(p.width("substituted"), 30.0f);
    EXPECT_EQ(p.width("inherited"), 150.0f);
    // as in CSS, silently at runtime (compat reported the declarations it could check)
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
    EXPECT_EQ(withCode(p.diagnostics(), "at-property-invalid").size(), 2u);
}

TEST(RegisteredPropertyTest, TransitionMidpointOnHover){
    Page p("hover",
           "@property --rph-w { syntax: '<length>'; inherits: false; initial-value: 40px; }\n"
           "@property --rph-c { syntax: '<color>'; inherits: false; initial-value: #2050d0; }\n"
           "#box { position: absolute; left: 20px; top: 20px; width: var(--rph-w); height: 60px; background-color: var(--rph-c);"
           "  transition: --rph-w 1s linear, --rph-c 1s linear; }\n"
           "#box:hover { --rph-w: 240px; --rph-c: #e0a020; }\n",
           "<div id='box'></div>");
    ASSERT_NE(p.document, nullptr);
    EXPECT_EQ(p.width("box"), 40.0f);
    p.context->ProcessMouseMove(30, 40, 0);
    p.at(0.0);
    p.at(0.5);
    // Chromium at 50%: 140px, rgb(128, 120, 120)
    EXPECT_NEAR(p.width("box"), 140.0f, 0.5f);
    EXPECT_EQ(p.value("box", "--rph-w"), "140px");
    const Rml::Colourb c = p.byId("box")->GetComputedValues().background_color();
    EXPECT_NEAR(c.red, 128, 1);
    EXPECT_NEAR(c.green, 120, 1);
    EXPECT_NEAR(c.blue, 120, 1);
    p.at(1.1);
    EXPECT_EQ(p.width("box"), 240.0f);
    // the pointer leaves: the transition runs back
    p.context->ProcessMouseMove(380, 280, 0);
    p.at(1.1);
    p.at(1.6);
    EXPECT_NEAR(p.width("box"), 140.0f, 0.5f);
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
}

TEST(RegisteredPropertyTest, KeyframesAtFixedTimes){
    Page p("keyframes",
           "@property --rpk-spin { syntax: '<angle>'; inherits: false; initial-value: 0deg; }\n"
           "@property --rpk-o { syntax: '<number>'; inherits: false; initial-value: 1; }\n"
           "@keyframes rpk-spin { to { --rpk-spin: 360deg; } }\n"
           "@keyframes rpk-fade { from { --rpk-o: 1; } to { --rpk-o: 0.2; } }\n"
           "#dial { width: 100px; height: 100px; background: conic-gradient(from var(--rpk-spin), red, blue, red);"
           "  transform: rotate(var(--rpk-spin)); animation: rpk-spin 4s linear infinite; }\n"
           "#fade { width: 10px; height: 10px; opacity: var(--rpk-o); animation: rpk-fade 2s linear forwards; }\n",
           "<div id='dial'></div><div id='fade'></div>");
    ASSERT_NE(p.document, nullptr);
    p.at(1.0);
    EXPECT_EQ(p.value("dial", "--rpk-spin"), "90deg");
    EXPECT_NEAR(p.byId("fade")->GetComputedValues().opacity(), 0.6f, 0.01f);
    const Rml::Property* transform = p.byId("dial")->GetProperty(Rml::PropertyId::Transform);
    ASSERT_NE(transform, nullptr);
    EXPECT_EQ(transform->ToString(), "rotate(90deg)");
    p.at(4.5);   // the second iteration
    EXPECT_EQ(p.value("dial", "--rpk-spin"), "45deg");
    EXPECT_NEAR(p.byId("fade")->GetComputedValues().opacity(), 0.2f, 0.01f);   // forwards
    const Rml::Property* image = p.byId("dial")->GetProperty(Rml::PropertyId::BackgroundImage);
    ASSERT_NE(image, nullptr);
    EXPECT_NE(image->ToString().find("45deg"), std::string::npos) << image->ToString();
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
}

TEST(RegisteredPropertyTest, AnimeDrivesARegisteredProperty){
    Page p("anime",
           "@property --rpa-glow { syntax: '<number>'; inherits: false; initial-value: 0; }\n"
           "@property --rpa-size { syntax: '<length>'; inherits: true; initial-value: 10px; }\n"
           "#a { width: 10px; height: 10px; opacity: var(--rpa-glow); }\n"
           "#child { height: 10px; width: var(--rpa-size); }\n",
           "<div id='a'><div id='child'></div></div>");
    ASSERT_NE(p.document, nullptr);
    AnimEngine::instance().reset();
    AnimEngine::instance().tick(1.0);
    const UIElement a = UIElementAccess::wrap(p.byId("a"));
    auto animation = Anime::animate(AnimTargetList{AnimTargets::fromElement(a)},
                                    AnimParam::object({{"--rpa-glow", 0.8}, {"--rpa-size", "110px"}, {"duration", 1000}, {"ease", "linear"}}));
    AnimEngine::instance().tick(1.5);
    p.at(0.5);
    // typed writes: the registered number and length interpolate, var() follows (and the inherited length reaches the child)
    EXPECT_EQ(p.value("a", "--rpa-glow"), "0.4");
    EXPECT_NEAR(p.byId("a")->GetComputedValues().opacity(), 0.4f, 0.001f);
    EXPECT_NEAR(p.width("child"), 60.0f, 0.01f);
    AnimEngine::instance().tick(2.2);
    p.at(1.2);
    EXPECT_NEAR(p.byId("a")->GetComputedValues().opacity(), 0.8f, 0.001f);
    EXPECT_NEAR(p.width("child"), 110.0f, 0.01f);
    animation.reset();
    AnimEngine::instance().reset();
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
}

TEST(RegisteredPropertyTest, ConflictFirstRegistrationWins){
    ASSERT_TRUE(HtmlUI::acquire());   // one RmlUi for both documents: registrations are process-global
    {
        Page first("conflict-a", "@property --rpc-x { syntax: '<length>'; inherits: false; initial-value: 5px; }\n"
                                 "#a { height: 10px; width: var(--rpc-x); }\n", "<div id='a'></div>");
        ASSERT_NE(first.document, nullptr);
        EXPECT_EQ(first.width("a"), 5.0f);
        EXPECT_TRUE(withCode(first.diagnostics(), "at-property-conflict").empty());

        Page second("conflict-b", "@property --rpc-x { syntax: '<color>'; inherits: true; initial-value: red; }\n"
                                  "#a { height: 10px; width: var(--rpc-x); }\n", "<div id='a'></div>");
        ASSERT_NE(second.document, nullptr);
        const std::vector<Diagnostic> conflict = withCode(second.diagnostics(), "at-property-conflict");
        ASSERT_EQ(conflict.size(), 1u);
        EXPECT_EQ(conflict[0].severity, Severity::Warning);
        EXPECT_EQ(conflict[0].line, 1u);
        EXPECT_NE(conflict[0].message.find("syntax '<length>', inherits false, initial-value 5px"), std::string::npos) << conflict[0].message;
        // the first registration stays in effect for every document
        EXPECT_EQ(second.width("a"), 5.0f);

        // the same definition again (a reload) is no conflict
        Page third("conflict-c", "@property --rpc-x { syntax: '<length>'; inherits: false; initial-value: 5px; }\n", "<div id='a'></div>");
        EXPECT_TRUE(withCode(third.diagnostics(), "at-property-conflict").empty());
    }
    HtmlUI::release();
}

TEST(RegisteredPropertyTest, StartingStyleAndTransitionBehavior){
    Page p("starting",
           "@property --rps-w { syntax: '<length>'; inherits: false; initial-value: 0px; }\n"
           "@property --rps-pick { syntax: '*'; inherits: false; initial-value: #e04040; }\n"
           "#panel { display: none; height: 10px; --rps-w: 240px; width: var(--rps-w); transition: --rps-w 1s linear; }\n"
           "#panel.open { display: block; }\n"
           "@starting-style { #panel.open { --rps-w: 40px; } }\n"
           "#flip { height: 10px; width: 10px; background-color: var(--rps-pick); transition: --rps-pick 1s linear allow-discrete; }\n"
           "#flip.blue { --rps-pick: #4060e0; }\n"
           "#plain { height: 10px; width: 10px; background-color: var(--rps-pick); transition: --rps-pick 1s linear; }\n"
           "#plain.blue { --rps-pick: #4060e0; }\n",
           "<div id='panel'></div><div id='flip'></div><div id='plain'></div>");
    ASSERT_NE(p.document, nullptr);
    p.byId("panel")->SetClass("open", true);
    p.byId("flip")->SetClass("blue", true);
    p.byId("plain")->SetClass("blue", true);
    p.at(0.0);
    EXPECT_EQ(p.width("panel"), 40.0f);
    p.at(0.4);
    EXPECT_NEAR(p.width("panel"), 120.0f, 0.5f);
    EXPECT_EQ(p.byId("flip")->GetComputedValues().background_color(), Rml::Colourb(0xe0, 0x40, 0x40, 255));
    EXPECT_EQ(p.byId("plain")->GetComputedValues().background_color(), Rml::Colourb(0x40, 0x60, 0xe0, 255));
    p.at(0.6);
    EXPECT_EQ(p.byId("flip")->GetComputedValues().background_color(), Rml::Colourb(0x40, 0x60, 0xe0, 255));
    p.at(1.1);
    EXPECT_EQ(p.width("panel"), 240.0f);
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
}

TEST(RegisteredPropertyTest, UnregisteredFallback){
    Page p("unregistered",
           "@property --rpu-reg { syntax: '<length>'; inherits: true; initial-value: 40px; }\n"
           "#parent { --rpu-plain: 40px; --rpu-reg: 40px; transition: --rpu-plain 1s linear, --rpu-reg 1s linear, all 1s linear; }\n"
           "#parent.wide { --rpu-plain: 240px; --rpu-reg: 240px; }\n"
           "#plain { height: 10px; width: var(--rpu-plain); } #reg { height: 10px; width: var(--rpu-reg); }\n",
           "<div id='parent'><div id='plain'></div><div id='reg'></div></div>");
    ASSERT_NE(p.document, nullptr);
    p.byId("parent")->SetClass("wide", true);
    p.at(0.0);
    p.at(0.5);
    // an unregistered custom property changes at once (it inherits, untyped); the registered one transitions
    EXPECT_EQ(p.width("plain"), 240.0f);
    EXPECT_EQ(p.value("plain", "--rpu-plain"), "240px");
    EXPECT_NEAR(p.width("reg"), 140.0f, 0.5f);
    // a name that was never registered is a custom property of the old kind
    EXPECT_EQ(Rml::StyleSheetSpecification::GetPropertyId("--rpu-plain"), Rml::PropertyId::Invalid);
    EXPECT_TRUE(Rml::StyleSheetSpecification::IsRegisteredCustomProperty(Rml::StyleSheetSpecification::GetPropertyId("--rpu-reg")));
    EXPECT_TRUE(p.errors.empty()) << p.errors.front();
}

// ---- goldens: the engine's render of the parity fixtures

namespace {

    std::vector<std::string> propertyFixtures(){
        std::vector<std::string> names;
        for (const BrowserFixture& fixture : listBrowserFixtures()){
            if (fixture.area == "property")
                names.push_back(fixture.name);
        }
        return names;
    }

}

class RegisteredPropertyGolden : public ::testing::TestWithParam<std::string> {};

TEST_P(RegisteredPropertyGolden, Renders){
    const std::string name = GetParam();
    FixtureSidecar sidecar;
    std::string error;
    ASSERT_TRUE(loadSidecar(browserFixturesDirectory() + "/property/" + name + ".json", sidecar, &error)) << error;
    FixtureRunner runner(sidecar.width, sidecar.height, sidecar.sampleCount);
    if (!runner.valid())
        GTEST_SKIP() << "no headless GPU context";
    std::vector<FixtureRunner::Capture> captures;
    ASSERT_TRUE(runner.load("fixtures/p3/property/" + name + ".html", &error)) << error;
    ASSERT_TRUE(runner.run(sidecar, captures, &error)) << error;
    ASSERT_FALSE(captures.empty());
    for (const FixtureRunner::Capture& capture : captures){
        const std::string golden = "property/" + name + (capture.suffix.empty() ? std::string() : "." + capture.suffix);
        const std::vector<uint8_t> straight = compat::unpremultiplied(capture.image);
        const CompareResult result = compareWithGolden(golden, straight, capture.image.width, capture.image.height);
        EXPECT_TRUE(result.passed) << golden << ": " << result.message;
    }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, RegisteredPropertyGolden, ::testing::ValuesIn(propertyFixtures()),
                         [](const ::testing::TestParamInfo<std::string>& info){
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });
