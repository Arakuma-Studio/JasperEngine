// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The registered compat properties (phase-2 §3.11.1): every name registered once per RmlUi
// initialisation, defaults, parsing, inheritance as specified, invalid values rejected by
// RmlUi, names the fork registers left to the fork, and the readers.

#include "RuntimeTestCommon.h"

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/runtime/CompatProperties.h"

#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    struct Expected {
        CompatProperty property;
        const char* name;
        bool inherited;
        const char* defaultText;   // "" = unset
        bool fork = false;         // the fork's own property (phase 3): found registered, not registered here
    };

    const Expected kExpected[] = {
        // list-style-*: the fork's since its ListMarkers (p3-cascade)
        {CompatProperty::ListStyleType, "list-style-type", true, "disc", true},
        {CompatProperty::ListStylePosition, "list-style-position", true, "outside", true},
        {CompatProperty::ListStyleImage, "list-style-image", true, "none", true},
        // object-fit, object-position: the fork's since its ObjectFit (p3-paint)
        {CompatProperty::ObjectFit, "object-fit", false, "fill", true},
        {CompatProperty::ObjectPosition, "object-position", false, "50% 50%", true},
        {CompatProperty::ImageRendering, "image-rendering", true, "auto"},
        {CompatProperty::AnimationFill, "-html-animation-fill", false, "none"},
        {CompatProperty::Fill, "fill", true, ""},
        {CompatProperty::Stroke, "stroke", true, ""},
        {CompatProperty::StrokeWidth, "stroke-width", true, ""},
        {CompatProperty::StrokeOpacity, "stroke-opacity", true, ""},
        {CompatProperty::FillOpacity, "fill-opacity", true, ""},
        {CompatProperty::StrokeLinecap, "stroke-linecap", true, ""},
        {CompatProperty::StrokeLinejoin, "stroke-linejoin", true, ""},
        {CompatProperty::StrokeDasharray, "stroke-dasharray", true, ""},
    };

    using CompatPropertiesTest = RuntimeContextTest;

}


TEST_F(CompatPropertiesTest, DefaultsAndUnset){
    ASSERT_NE(show("<div id='a'/>"), nullptr);
    Rml::Element* element = byId("a");
    for (const Expected& expected : kExpected)
        EXPECT_EQ(CompatProperties::text(*element, expected.property), expected.defaultText) << expected.name;
    Rml::Colourb colour;
    EXPECT_FALSE(CompatProperties::color(*element, CompatProperty::Fill, colour));
}

TEST_F(CompatPropertiesTest, ValuesParseAndInheritAsSpecified){
    ASSERT_NE(show("<div id='parent'><div id='child'/></div>",
                   "#parent { list-style-type: lower-roman; list-style-position: inside; list-style-image: \"/ui/bullet.png\"; "
                   "object-fit: cover; object-position: right 10px top; image-rendering: pixelated; -html-animation-fill: forwards; "
                   "fill: #ff000080; stroke: currentcolor; stroke-width: 1.5; stroke-opacity: 50%; fill-opacity: 0.25; "
                   "stroke-linecap: round; stroke-linejoin: bevel; stroke-dasharray: 4 2; }"), nullptr);
    Rml::Element* parent = byId("parent");
    Rml::Element* child = byId("child");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ListStyleType), "lower-roman");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ListStylePosition), "inside");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ListStyleImage), "/ui/bullet.png");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ObjectFit), "cover");
    // the fork's <position>: an offset from the right edge is a calc() (without ObjectFit: as written)
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ObjectPosition),
              ForkFeatures::present().has(ForkFeature::ObjectFit) ? "calc(100% - 10px) 0%" : "right 10px top");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::ImageRendering), "pixelated");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::AnimationFill), "forwards");
    Rml::Colourb fill;
    ASSERT_TRUE(CompatProperties::color(*parent, CompatProperty::Fill, fill));
    EXPECT_EQ(fill, Rml::Colourb(255, 0, 0, 128));
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::Stroke), "currentcolor");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::StrokeWidth), "1.5");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::StrokeOpacity), "50%");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::FillOpacity), "0.25");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::StrokeLinecap), "round");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::StrokeLinejoin), "bevel");
    EXPECT_EQ(CompatProperties::text(*parent, CompatProperty::StrokeDasharray), "4 2");

    // inherited ones reach the child, the others keep their defaults
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::ListStyleType), "lower-roman");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::ListStylePosition), "inside");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::ImageRendering), "pixelated");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::StrokeWidth), "1.5");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::ObjectFit), "fill");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::ObjectPosition), "50% 50%");
    EXPECT_EQ(CompatProperties::text(*child, CompatProperty::AnimationFill), "none");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
}

TEST_F(CompatPropertiesTest, StringMarkersAndInvalidValues){
    ASSERT_NE(show("<div id='a'/>", "#a { list-style-type: \"\xE2\x86\x92 \"; object-fit: banana; image-rendering: blurry; fill: notacolor; }"), nullptr);
    Rml::Element* element = byId("a");
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ListStyleType), "\xE2\x86\x92 ");
    // RmlUi rejects the invalid ones (and says so): the defaults stay
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ObjectFit), "fill");
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ImageRendering), "auto");
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::Fill), "");
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_WARN, "object-fit"), 1u);
}

TEST_F(CompatPropertiesTest, InlineStyleAndLocalProperties){
    ASSERT_NE(show("<div id='a' style='object-fit: contain; list-style-type: square;'/>"), nullptr);
    Rml::Element* element = byId("a");
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ObjectFit), "contain");
    EXPECT_TRUE(element->SetProperty("object-fit", "scale-down"));
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ObjectFit), "scale-down");
    EXPECT_FALSE(element->SetProperty("object-fit", "stretch"));
    EXPECT_EQ(CompatProperties::text(*element, CompatProperty::ListStyleType), "square");
}
