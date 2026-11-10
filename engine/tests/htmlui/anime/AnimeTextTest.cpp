// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TestDocumentCommon.h"

#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimText.h"
#include "object/ui/UIElementInternal.h"
#include "LuaBinding.h"
#include "lua.hpp"
#include "LuaBridge.h"
#include "json.hpp"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Property.h>

#include <gtest/gtest.h>

#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const nlohmann::json& animTextReference(){
        static const nlohmann::json reference = [](){
            std::ifstream input(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/anime/text-reference.json");
            nlohmann::json result;
            if (input) input >> result;
            return result;
        }();
        return reference;
    }

    std::vector<std::string> animTextContents(const std::vector<UIElement>& elements){
        std::vector<std::string> result;
        for (const auto& element : elements) result.push_back(element.getTextContent());
        return result;
    }

    class AnimeTextTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure());
            AnimEngine::instance().reset();
            fixture = std::make_unique<SceneFixture>();
            document = &fixture->add<UIDocument>();
            ASSERT_TRUE(document->load("p1/anime.rml"));
            updateDocuments(fixture->getScene());
            label = document->getElementById("label");
            ASSERT_TRUE(label.isValid());
            label.setStyleProperty("width", "1000px");
        }

        void TearDown() override{
            AnimEngine::instance().reset();
            fixture.reset();
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
        }

        std::shared_ptr<AnimText> split(const AnimParam& params = AnimParam::object()){
            updateDocuments(fixture->getScene());
            return Anime::splitText({AnimTargets::fromElement(label)}, params);
        }

        std::unique_ptr<SceneFixture> fixture;
        UIDocument* document = nullptr;
        UIElement label;
    };

}

TEST_F(AnimeTextTest, WordsAreTheDefaultAndMarkupIsRestored){
    label.setInnerHTML("Hola <em>niño</em> &amp; adiós");
    const auto original = label.getInnerHTML();
    auto splitter = split(AnimParam::object({{"accessible", false}}));
    EXPECT_EQ(animTextContents(splitter->getWords()), (std::vector<std::string>{"Hola", "niño", "&", "adiós"}));
    EXPECT_TRUE(splitter->getChars().empty());
    EXPECT_TRUE(splitter->getLines().empty());
    EXPECT_TRUE(splitter->isReady());
    EXPECT_EQ(label.querySelectorAll("em").size(), 1u);
    splitter->revert();
    EXPECT_EQ(label.getInnerHTML(), original);
    EXPECT_TRUE(splitter->getWords().empty());
    EXPECT_FALSE(splitter->isReady());
    splitter->split();
    EXPECT_EQ(splitter->getWords().size(), 4u);
}

TEST_F(AnimeTextTest, CharactersAreExtendedGraphemes){
    label.setTextContent("á ñ e\xcc\x81 👩‍👩‍👧‍👦 🇪🇸");
    auto splitter = split(AnimParam::object({{"chars", true}, {"accessible", false}}));
    EXPECT_EQ(animTextContents(splitter->getChars()), (std::vector<std::string>{"á", "ñ", "e\xcc\x81", "👩‍👩‍👧‍👦", "🇪🇸"}));
    EXPECT_EQ(splitter->getWords().size(), 5u);
    for (size_t i = 0; i < splitter->getChars().size(); ++i)
        EXPECT_EQ(splitter->getChars()[i].getAttribute("data-char"), std::to_string(i));
}

TEST_F(AnimeTextTest, IncludedSpacesBelongToThePrecedingCharacter){
    // Anime.js v4.5.0 appends a NBSP to the last character of the preceding word.
    label.setTextContent("A B");
    auto splitter = split(AnimParam::object({{"chars", true}, {"includeSpaces", true}, {"accessible", false}}));
    EXPECT_EQ(animTextContents(splitter->getChars()), (std::vector<std::string>{"A\xc2\xa0", "B"}));
    EXPECT_EQ(animTextContents(splitter->getWords()), (std::vector<std::string>{"A\xc2\xa0", "B"}));
}

TEST_F(AnimeTextTest, MaskWrappersClipTheAnimatedInnerElements){
    label.setTextContent("Hola");
    auto splitter = split(AnimParam::object({
        {"chars", AnimParam::object({{"wrap", "clip"}, {"class", "letter"}})}, {"accessible", false}}));
    ASSERT_EQ(splitter->getChars().size(), 4u);
    for (const auto& character : splitter->getChars()){
        EXPECT_TRUE(character.hasClass("letter"));
        const auto wrapper = character.getParentElement();
        EXPECT_FALSE(wrapper.hasAttribute("data-char"));
        const auto* element = UIElementAccess::get(wrapper);
        EXPECT_EQ(element->GetComputedValues().overflow_x(), Rml::Style::Overflow::Hidden);
        EXPECT_EQ(element->GetComputedValues().overflow_y(), Rml::Style::Overflow::Hidden);
    }
}

TEST_F(AnimeTextTest, LineSplitsRefreshSynchronouslyAfterLayoutChanges){
    label.setTextContent("A A A A");
    auto measure = split(AnimParam::object({{"accessible", false}}));
    ASSERT_EQ(measure->getWords().size(), 4u);
    const float stride = measure->getWords()[1].getOffsetLeft() - measure->getWords()[0].getOffsetLeft();
    ASSERT_GT(stride, 0.f);
    measure->revert();
    label.setStyleProperty("width", std::to_string(stride * 2.f) + "px");
    auto splitter = split(AnimParam::object({{"lines", AnimParam::object({{"wrap", "clip"}})}, {"chars", true}, {"accessible", false}}));
    ASSERT_EQ(splitter->getLines().size(), 2u);
    EXPECT_EQ(splitter->getWords().size(), 4u);
    EXPECT_EQ(splitter->getChars().size(), 4u);
    EXPECT_EQ(splitter->getChars()[0].getAttribute("data-line"), "0");
    EXPECT_EQ(splitter->getChars()[3].getAttribute("data-line"), "1");
    const auto oldCharacter = splitter->getChars()[0];
    label.setStyleProperty("width", "1000px");
    updateDocuments(fixture->getScene());
    EXPECT_EQ(splitter->getLines().size(), 1u);
    EXPECT_EQ(splitter->getChars().size(), 4u);
    EXPECT_FALSE(oldCharacter.isValid());
}

TEST_F(AnimeTextTest, LinesOnlyDoNotExposeHelperWords){
    label.setInnerHTML("A<br/>B");
    auto splitter = split(AnimParam::object({{"lines", true}, {"words", false}, {"accessible", false}}));
    ASSERT_EQ(splitter->getLines().size(), 2u);
    EXPECT_EQ(animTextContents(splitter->getLines()), (std::vector<std::string>{"A", "B"}));
    EXPECT_TRUE(splitter->getWords().empty());
    EXPECT_TRUE(splitter->getChars().empty());
    EXPECT_TRUE(label.querySelectorAll("[data-word]").empty());
}

TEST_F(AnimeTextTest, CharacterOnlyRefreshKeepsTargetsAndRunningAnimations){
    label.setTextContent("niño");
    auto splitter = split(AnimParam::object({{"chars", true}, {"accessible", false}}));
    const auto first = splitter->getChars()[0];
    auto animation = Anime::animate(AnimTargets::fromElements(splitter->getChars()), AnimParam::object({
        {"opacity", AnimParam::array({0, 1})}, {"duration", 1000}, {"ease", "linear"}, {"autoplay", false}}));
    animation->seek(250);
    label.setStyleProperty("width", "40px");
    updateDocuments(fixture->getScene());
    EXPECT_TRUE(first.isValid());
    ASSERT_EQ(splitter->getChars().size(), 4u);
    EXPECT_TRUE(first == splitter->getChars()[0]);
    animation->seek(500);
    EXPECT_NEAR(AnimUtils::get({AnimTargets::fromElement(first)}, "opacity").toNumber(), 0.5, 1e-6);
    splitter->split();
    EXPECT_TRUE(first == splitter->getChars()[0]);
}

TEST_F(AnimeTextTest, InitialCharacterSplitKeepsExistingInlineElements){
    label.setInnerHTML("Hola <em>niño</em>");
    auto emphasis = label.querySelector("em");
    ASSERT_TRUE(emphasis.isValid());
    int events = 0;
    emphasis.addEventListener("click", [&events](UIEvent&){ ++events; });
    auto splitter = split(AnimParam::object({{"chars", true}, {"accessible", false}}));
    ASSERT_TRUE(emphasis.isValid());
    EXPECT_TRUE(emphasis == label.querySelector("em"));
    emphasis.dispatchEvent("click");
    EXPECT_EQ(events, 1);
    EXPECT_EQ(splitter->getChars().size(), 8u);
}

TEST_F(AnimeTextTest, AccessibilityKeepsOneUnsplitCopyAndRevertRemovesIt){
    label.setTextContent("niño");
    auto splitter = split(AnimParam::object({{"chars", true}}));
    const auto copies = label.querySelectorAll("[data-anime-accessible]");
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].getTextContent(), "niño");
    for (const auto& unit : splitter->getChars()) EXPECT_EQ(unit.getAttribute("aria-hidden"), "true");
    splitter->refresh();
    EXPECT_EQ(label.querySelectorAll("[data-anime-accessible]").size(), 1u);
    splitter->revert();
    EXPECT_EQ(label.getTextContent(), "niño");
    EXPECT_TRUE(label.querySelectorAll("[data-anime-accessible]").empty());
}

TEST_F(AnimeTextTest, AccessibilityCopyDoesNotDuplicateIdsOrKeyboardTargets){
    label.setInnerHTML("<a id='split-link' href='https://example.com'>niño</a> <button id='split-button'>más</button>");
    auto splitter = split(AnimParam::object({{"chars", true}}));
    const auto copies = label.querySelectorAll("[data-anime-accessible]");
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].getTextContent(), "niño más");
    EXPECT_TRUE(copies[0].getChildren().empty());
    EXPECT_EQ(label.querySelectorAll("#split-link").size(), 1u);
    EXPECT_EQ(label.querySelectorAll("#split-button").size(), 1u);
    splitter->revert();
    EXPECT_EQ(label.querySelectorAll("#split-link").size(), 1u);
}

TEST_F(AnimeTextTest, AccessibilityCopySeparatesExplicitAndBlockLines){
    label.setInnerHTML("Hola<br/><span style='display:block'>niño</span>");
    // Lua can split immediately after changing markup, before the next document update.
    auto splitter = Anime::splitText({AnimTargets::fromElement(label)}, AnimParam::object({{"chars", true}}));
    const auto copies = label.querySelectorAll("[data-anime-accessible]");
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].getTextContent(), "Hola\nniño");
    EXPECT_EQ(splitter->getChars().size(), 8u);
}

TEST_F(AnimeTextTest, InitiallyHiddenTargetsKeepTheirAccessibleReadingText){
    label.setTextContent("niño");
    label.setStyleProperty("display", "none");
    auto splitter = split(AnimParam::object({{"chars", true}}));
    const auto copies = label.querySelectorAll("[data-anime-accessible]");
    ASSERT_EQ(copies.size(), 1u);
    EXPECT_EQ(copies[0].getTextContent(), "niño");
    EXPECT_EQ(splitter->getChars().size(), 4u);
}

TEST_F(AnimeTextTest, SplitCharactersUseAnimeStaggerAtExactTimes){
    // Same three targets and seek times as the upstream Anime.js 4.5.0 text reference.
    label.setTextContent("ñáe\xcc\x81");
    auto splitter = split(AnimParam::object({{"chars", true}, {"accessible", false}}));
    auto targets = AnimTargets::fromElements(splitter->getChars());
    auto animation = Anime::animate(targets, AnimParam::object({
        {"translateY", AnimParam::array({100, 0})}, {"duration", 500}, {"delay", Anime::stagger(100)},
        {"ease", "linear"}, {"autoplay", false}}));
    ASSERT_TRUE(animTextReference().contains("seek"));
    for (const auto& frame : animTextReference()["seek"]){
        animation->seek(frame["time"].get<double>());
        for (size_t i = 0; i < targets.size(); ++i)
            EXPECT_NEAR(std::stod(AnimUtils::get({targets[i]}, "translateY").toString()), frame["translateY"][i].get<double>(), 1e-6);
    }
}

TEST_F(AnimeTextTest, WordsAndCharactersMatchAnime450Reference){
    ASSERT_TRUE(animTextReference().contains("samples"));
    for (const auto& sample : animTextReference()["samples"]){
        label.setInnerHTML(sample["html"].get<std::string>());
        const auto& options = sample["params"];
        auto splitter = split(AnimParam::object({{"chars", true}, {"accessible", false},
            {"includeSpaces", options.value("includeSpaces", false)}}));
        EXPECT_EQ(animTextContents(splitter->getWords()), sample["words"].get<std::vector<std::string>>());
        EXPECT_EQ(animTextContents(splitter->getChars()), sample["chars"].get<std::vector<std::string>>());
        splitter->revert();
    }
}

TEST_F(AnimeTextTest, LuaArraysAreAnimatableAndMethodsChain){
    label.setTextContent("niño");
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    ASSERT_TRUE(luabridge::push(L, label));
    lua_setglobal(L, "splitTextLabel");
    const char* source = R"(
        local split = anime.splitText(splitTextLabel, {chars = {wrap = 'clip'}, accessible = false})
        assert(split.ready and #split.chars == 4 and #split.words == 1)
        assert(split.chars[3].textContent == 'ñ')
        anime.animate(split.chars, {opacity = 0.5, duration = 0})
        assert(anime.utils.get(split.chars[1], 'opacity') == 0.5)
        assert(split:refresh().ready)
        assert(not split:revert().ready)
        assert(splitTextLabel.textContent == 'niño')
        splitTextLabel = nil
    )";
    ASSERT_EQ(luaL_loadstring(L, source), LUA_OK);
    const int status = LuaBinding::pcallWithTraceback(L, 0, 0);
    const std::string error = status == LUA_OK ? "" : lua_tostring(L, -1);
    if (status != LUA_OK) lua_pop(L, 1);
    ASSERT_EQ(status, LUA_OK) << error;
}

TEST_F(AnimeTextTest, ResetRestoresGeneratedTreesAndDestroyedDocumentsAreSafe){
    label.setTextContent("hola");
    auto splitter = split(AnimParam::object({{"chars", true}}));
    AnimEngine::instance().reset();
    EXPECT_EQ(label.getTextContent(), "hola");
    EXPECT_FALSE(splitter->isReady());
    splitter = split(AnimParam::object({{"chars", true}}));
    fixture.reset();
    document = nullptr;
    AnimText::updateLayouts(nullptr);
    EXPECT_NO_THROW(splitter->revert());
}
