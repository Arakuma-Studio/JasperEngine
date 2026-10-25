// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Per-context raster density is also read by the production shape decorators. Independent
// managers must retain their own configuration; synthesis and effect pixels have GPU owners.

#include "harness/RecordingRenderInterface.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "RmlFile.h"
#include "RmlSystem.h"
#include "util/FontRegistry.h"

#include <RmlUi/Core.h>
#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST(RmlFontEffectsTest, PerContextRasterScale){
    ASSERT_FALSE(HtmlUI::isInitialized());
    FontRegistry::clear();
    RecordingRenderInterface firstRenderer, otherRenderer;
    RmlFontEngine engine;
    Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
    Rml::SetFileInterface(&HtmlUI::getFileInterface());
    Rml::SetFontEngineInterface(&engine);
    ASSERT_TRUE(Rml::Initialise());
    Rml::Context* first = Rml::CreateContext("font-scale-first", Rml::Vector2i(640, 360), &firstRenderer);
    Rml::Context* other = Rml::CreateContext("font-scale-other", Rml::Vector2i(320, 180), &otherRenderer);
    ASSERT_TRUE(first && other);
    engine.setRasterScale(first->GetRenderManager(), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first->GetRenderManager()), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other->GetRenderManager()), 1.0f);
    engine.setRasterScale(other->GetRenderManager(), 1.5f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first->GetRenderManager()), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other->GetRenderManager()), 1.5f);
    Rml::RemoveContext("font-scale-first");
    Rml::RemoveContext("font-scale-other");
    Rml::Shutdown();
    FontRegistry::clear();
}
