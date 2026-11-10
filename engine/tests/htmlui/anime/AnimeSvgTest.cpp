// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "LuaBinding.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimCommon.h"
#include "htmlui/compat/DocumentLoader.h"
#include "object/ui/UIElementInternal.h"

#include "json.hpp"
#include "lua.hpp"
#include "LuaBridge.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cmath>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const nlohmann::ordered_json& svgReference(){
        static const auto result = [](){
            std::ifstream input(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/anime/svg-reference.json");
            nlohmann::ordered_json value;
            input >> value;
            return value;
        }();
        return result;
    }

    class AnimeSvgTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(HtmlUI::acquire());
            AnimEngine::instance().reset();
            AnimEngine::instance().tickMilliseconds(1000, 0);
            context = Rml::CreateContext("anime-svg-test", Rml::Vector2i(240, 240), &recording);
            ASSERT_NE(context, nullptr);
            document = context->LoadDocumentFromMemory(R"(
                <rml><head><style>body { width:240px; height:240px; } svg { display:block; width:200px; height:200px; }</style></head><body>
                <svg id="svg" width="200" height="200" viewBox="0 0 100 100">
                    <path id="source" d="M0 0L100 0" fill="none" stroke="red" stroke-width="4"/>
                    <path id="destination" d="M0 0L0 100L100 100" fill="none" stroke="blue"/>
                    <polyline id="poly" points="0,20 100,20" fill="none" stroke="green" stroke-width="4"/>
                </svg></body></rml>)", "p1/vector/anime-svg.rml");
            ASSERT_NE(document, nullptr);
            document->Show();
            render();
        }

        void TearDown() override{
            AnimEngine::instance().reset();
            context->UnloadAllDocuments();
            context->Update();
            Rml::RemoveContext("anime-svg-test");
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            htmlSource.reset();
            HtmlUI::release();
            EXPECT_EQ(recording.getLiveTextureCount(), 0u);
            EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
        }

        UIElement element(const char* id){ return UIElementAccess::wrap(document->GetElementById(id)); }
        AnimTargetList target(const char* id){ return {AnimTargets::fromElement(element(id))}; }

        void render(){ context->Update(); recording.beginFrame(); context->Render(); }

        std::vector<uint8_t> pixels(){
            for (const auto& draw : recording.getDraws()){
                const auto* texture = recording.getTexture(draw.texture);
                if (texture && !texture->pixels.empty()) return texture->pixels;
            }
            return {};
        }

        std::string lua(const std::string& code){
            lua_State* state = LuaBinding::getLuaState();
            if (luaL_loadstring(state, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(state, 0, 0) != LUA_OK){
                const std::string error = lua_tostring(state, -1);
                lua_pop(state, 1);
                return error;
            }
            return "";
        }

        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::shared_ptr<const htmlui::compat::CompatDocument> htmlSource;
    };

}

TEST_F(AnimeSvgTest, InlineDescendantsAreEditableAndInvalidateTheirImage){
    UIElement path = element("source");
    ASSERT_TRUE(path.isValid()); // Red control: inline SVG previously had no DOM descendants.
    EXPECT_EQ(path.getTagName(), "path");
    const std::pair<const char*, const char*> changes[] = {
        {"d", "M10 10L90 90"}, {"stroke-dasharray", "10 10"},
        {"stroke-dashoffset", "10"}, {"transform", "translate(20 0)"}};
    auto previous = pixels();
    ASSERT_FALSE(previous.empty());
    for (const auto& change : changes){
        path.setAttribute(change.first, change.second);
        render();
        const auto next = pixels();
        EXPECT_NE(previous, next) << change.first;
        previous = next;
    }
    element("poly").setAttribute("points", "0,80 100,80");
    render();
    EXPECT_NE(previous, pixels());
}

TEST_F(AnimeSvgTest, DrawableRangesMatchAnime450AndRevert){
    const auto targets = Anime::createDrawable(target("source"));
    ASSERT_EQ(targets.size(), 1u);
    UIElement path = element("source");
    for (const auto& reference : svgReference()["draws"]){
        AnimUtils::set(targets, AnimParam::object({{"draw", reference["draw"].get<std::string>()}}));
        const double offset = AnimMath::toNumber(path.getAttribute("stroke-dashoffset"));
        EXPECT_NEAR(offset, reference["offset"].get<double>() * 0.1, 1e-9);
        std::istringstream actual(path.getAttribute("stroke-dasharray")), expected(reference["dash"].get<std::string>());
        double a = 0, b = 0;
        while (expected >> b){ ASSERT_TRUE((bool)(actual >> a)); EXPECT_NEAR(a, b * 0.1, 1e-9); }
    }
    Anime::createDrawable(targets);
    auto animation = Anime::animate(targets, AnimParam::object({{"draw", "0 1"}, {"autoplay", false}, {"duration", 1000}, {"ease", "linear"}}));
    animation->seek(500);
    EXPECT_EQ(path.getAttribute("draw"), "0 0.5");
    animation->revert();
    EXPECT_EQ(path.getAttribute("draw"), "0 0");
    EXPECT_EQ(path.getAttribute("stroke-dasharray"), "0 101");
}

TEST_F(AnimeSvgTest, MorphWithDifferentPointCountsMatchesAnime450AtFixedTimes){
    const AnimParam morph = Anime::morphTo(target("destination"), .02);
    ASSERT_TRUE(morph.isFunction());
    AnimFunctionArgs args;
    args.target = target("source")[0];
    const AnimParam values = morph.asFunction()(args);
    ASSERT_TRUE(values.isArray());
    EXPECT_EQ(values.at(0).asString(), svgReference()["morph"][0].get<std::string>());
    EXPECT_EQ(values.at(1).asString(), svgReference()["morph"][1].get<std::string>());
    auto animation = Anime::animate(target("source"), AnimParam::object({{"d", morph}, {"autoplay", false}, {"duration", 1000}, {"ease", "linear"}}));
    std::vector<double> a, b;
    std::vector<std::string> unused;
    AnimValue::splitComplex(values.at(0).asString(), a, unused);
    AnimValue::splitComplex(values.at(1).asString(), b, unused);
    for (const auto& time : svgReference()["times"]){
        const double progress = time.get<double>();
        animation->seek(progress * 1000);
        std::vector<double> actual;
        AnimValue::splitComplex(element("source").getAttribute("d"), actual, unused);
        ASSERT_EQ(actual.size(), a.size());
        for (size_t i = 0; i < a.size(); ++i) EXPECT_NEAR(actual[i], a[i] + (b[i] - a[i]) * progress, 1e-3);
    }
}

TEST_F(AnimeSvgTest, MotionPathAndOffsetsMatchAnime450AtFixedTimes){
    for (const auto& entry : svgReference()["motion"].items()){
        const double offset = std::stod(entry.key());
        AnimParam parameters = Anime::createMotionPath(target("destination"), offset);
        ASSERT_TRUE(parameters.isObject());
        parameters.set("duration", 1000);
        parameters.set("ease", "linear");
        parameters.set("autoplay", false);
        auto object = std::make_shared<AnimObjectTarget>();
        auto animation = Anime::animate({object}, parameters);
        for (size_t i = 0; i < svgReference()["times"].size(); ++i){
            animation->seek(svgReference()["times"][i].get<double>() * 1000);
            for (const auto& value : entry.value()[i].items())
                EXPECT_NEAR(object->getNumber(value.key()), value.value().get<double>(), 1e-6) << offset << ":" << i << ":" << value.key();
        }
    }
}

TEST_F(AnimeSvgTest, LuaFactoriesAndProceduralAttributeWrites){
    lua_State* state = LuaBinding::getLuaState();
    ASSERT_TRUE((bool)luabridge::push(state, element("svg")));
    lua_setglobal(state, "svgRoot");
    ASSERT_EQ(lua(R"(
        local from = svgRoot:querySelector('#source')
        local to = svgRoot:querySelector('#destination')
        assert(from:isValid() and to:isValid())
        assert(type(anime.svg) == 'table' and type(anime.svg.createDrawable) == 'function')
        local drawable = anime.svg.createDrawable(from)
        assert(#drawable == 1)
        drawable[1]:setAttribute('draw', '0 1')
        assert(from:getAttribute('stroke-dasharray') == '100 0')
        drawable[1]:setAttribute('draw', '0 0')
        local draw = anime.animate(drawable, {draw='0 1', autoplay=false, duration=1000, ease='linear'})
        draw:seek(500)
        assert(from:getAttribute('draw') == '0 0.5')
        local morph = anime.morphTo(to, .02)
        assert(type(morph(from, 0, 1)) == 'table')
        local motion = anime.createMotionPath(to)
        local position = {translateX=0, translateY=0, rotate=0}
        anime.animate(position, {translateX=motion.translateX, translateY=motion.translateY, autoplay=false, duration=1000, ease='linear'}):seek(250)
        assert(position.translateX == 0 and position.translateY == 100)
        anime.createTimer({duration=1000, autoplay=false, onUpdate=function(self)
            from:setAttribute('d', 'M0 50L100 ' .. tostring(self.progress * 100))
        end}):seek(500)
        assert(from:getAttribute('d'):find('M0 50L100') == 1)
        svgRoot = nil
    )"), "");
    render();
    EXPECT_FALSE(pixels().empty());
}

TEST_F(AnimeSvgTest, SvgCaseAndUnknownPresentationStylesSurviveEditing){
    UIElement root = element("svg");
    root.setInnerHTML(R"(<defs><linearGradient id="gradient"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient></defs><path id="painted" d="M10 10H90V90H10Z" style="fill:url(#gradient);stroke-miterlimit:3"/>)");
    ASSERT_TRUE(element("painted").isValid());
    element("painted").setAttribute("transform", "translate(1 0)");
    const std::string source = root.getInnerHTML();
    EXPECT_NE(source.find("linearGradient"), std::string::npos);
    EXPECT_NE(source.find("stroke-miterlimit:3"), std::string::npos);
    render();
    EXPECT_FALSE(pixels().empty());
}

TEST_F(AnimeSvgTest, ProceduralThirtyShapesReportFrameCost){
    UIElement root = element("svg");
    std::string markup;
    for (int i = 0; i < 30; ++i)
        markup += "<path id='star" + std::to_string(i) + "' fill='#2c86ff'/>";
    root.setInnerHTML(markup);
    std::vector<UIElement> stars;
    for (int i = 0; i < 30; ++i) stars.push_back(element(("star" + std::to_string(i)).c_str()));
    const auto random = AnimUtils::createSeededRandom(42);
    std::vector<double> phases;
    for (int i = 0; i < 30; ++i) phases.push_back(random(0, 6.28, 4));
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 60; ++frame){
        const double time = frame / 30.0;
        for (int i = 0; i < 30; ++i){
            std::string path;
            for (int vertex = 0; vertex < 10; ++vertex){
                const double angle = vertex * std::acos(-1.0) / 5 + time + phases[i];
                const double radius = vertex % 2 ? 3 : 7;
                const double x = 10 + (i % 6) * 16 + std::cos(angle) * radius;
                const double y = 10 + (i / 6) * 18 + std::sin(angle) * radius;
                path += std::string(vertex ? "L" : "M") + AnimMath::toString(x) + " " + AnimMath::toString(y);
            }
            stars[i].setAttribute("d", path + "Z");
        }
        render();
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 60;
    std::printf("[bench] anime SVG: 30 procedural paths, 200x200 raster, 60 frames: %.3f ms/frame\n", milliseconds);
    RecordProperty("procedural_ms_per_frame", std::to_string(milliseconds));
    EXPECT_FALSE(pixels().empty());
}

TEST_F(AnimeSvgTest, SvgMotionTargetsSerializeTransformsInUserUnits){
    const auto before = pixels();
    auto animation = Anime::animate(target("poly"), AnimParam::object({{"translateY", 20}, {"autoplay", false}, {"duration", 1000}, {"ease", "linear"}}));
    animation->seek(500);
    render();
    const std::string source = element("svg").getInnerHTML();
    EXPECT_NE(source.find("matrix(1 0 0 1 0 10)"), std::string::npos);
    EXPECT_NE(before, pixels());
}

TEST_F(AnimeSvgTest, HtmlSvgDescendantsUseSvgNamesAndDrawableGeometry){
    context->UnloadAllDocuments();
    context->Update();
    htmlSource = htmlui::compat::DocumentLoader::compileString(R"(
        <!doctype html><html><head><style>svg{width:200px;height:200px}</style></head><body>
        <svg id="svg" width="200" height="200" viewBox="0 0 100 100"><path id="source" d="M0 0L100 0" stroke="red" stroke-width="4"/></svg>
        </body></html>)", "p1/vector/anime-svg.html", htmlui::compat::CompatOptions());
    ASSERT_TRUE(htmlSource->compiled);
    document = context->LoadDocumentFromMemory(htmlSource->rml, htmlSource->sourcePath);
    ASSERT_NE(document, nullptr);
    document->Show();
    render();
    ASSERT_TRUE(element("source").isValid());
    EXPECT_EQ(element("source").getTagName(), "path");
    const auto drawable = Anime::createDrawable(target("source"));
    ASSERT_EQ(drawable.size(), 1u);
    element("source").setAttribute("draw", "0 .5");
    EXPECT_EQ(element("source").getAttribute("stroke-dasharray"), "50 51");
    render();
    EXPECT_FALSE(pixels().empty());
}
