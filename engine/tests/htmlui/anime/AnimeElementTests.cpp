// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// anime on HTML UI elements: individual transforms composed into one Rml::Transform in
// Anime's order, typed writes (opacity, lengths, colours), custom properties, selectors,
// layout untouched by transform/opacity, invalid targets cancelled silently, revert, and the
// UI clock / game clock driving through the engine frame.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimCommon.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // Element::IsLayoutDirty is protected (ElementDocument overrides it, ElementDocument.h:146):
    // reached through a member pointer named in a derived class
    struct LayoutProbe : Rml::Element {
        static bool isDirty(Rml::Element* element){
            bool (Rml::Element::*method)() = &LayoutProbe::IsLayoutDirty;
            return (element->*method)();
        }
        // the style pass of Context::Update without the layout pass: property changes reach
        // OnPropertyChange, which dirties the layout for layout properties only
        static void updateStyle(Rml::Element* document){
            void (Rml::Element::*method)(float, Rml::Vector2f) = &LayoutProbe::Update;
            (document->*method)(1.0f, Rml::Vector2f(640, 360));
        }
    };

    // the inline value (GetLocalProperty includes the matching stylesheet rules)
    const Rml::Property* inlineProperty(Rml::Element* e, Rml::PropertyId id){
        const Rml::PropertyMap& properties = e->GetLocalStyleProperties();
        auto it = properties.find(id);
        return it != properties.end() ? &it->second : nullptr;
    }

    class AnimeElementTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            AnimEngine::instance().reset();
            HtmlUIClock::reset();
            fixture.reset(new SceneFixture());
            document = &fixture->add<UIDocument>();
            ASSERT_TRUE(document->load("p1/anime.rml"));
            updateDocuments(scene());
            TestSystem::get().clearLogs();
            // the UI clock pinned and stepped by the tests: the document updates tick the same time
            now = 1000;
            HtmlUIClock::setFixedTime(now / 1000.0);
            AnimEngine::instance().tick(HtmlUIClock::now());
        }

        void TearDown() override{
            AnimEngine::instance().reset();
            fixture.reset();
            HtmlUIClock::reset();
            Engine::setTimeScale(1.0f);
            Engine::setFixedDeltatime(0.0);
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
        }

        Scene& scene(){ return fixture->getScene(); }

        Rml::Element* element(const char* id){
            return UIElementAccess::get(document->getElementById(id));
        }

        AnimTargetList target(const char* id){
            return {AnimTargets::fromElement(document->getElementById(id))};
        }

        void advance(double ms){
            now += ms;
            HtmlUIClock::setFixedTime(now / 1000.0);
            AnimEngine::instance().tick(HtmlUIClock::now());
        }

        std::vector<Rml::TransformPrimitive::Type> transformTypes(Rml::Element* e){
            std::vector<Rml::TransformPrimitive::Type> types;
            const Rml::Property* p = e->GetLocalProperty(Rml::PropertyId::Transform);
            if (p && p->unit == Rml::Unit::TRANSFORM){
                Rml::TransformPtr t = p->Get<Rml::TransformPtr>();
                if (t){
                    for (const Rml::TransformPrimitive& primitive : t->GetPrimitives())
                        types.push_back(primitive.type);
                }
            }
            return types;
        }

        std::unique_ptr<SceneFixture> fixture;
        UIDocument* document = nullptr;
        double now = 0;
    };

}

TEST_F(AnimeElementTest, IndividualTransformsComposeInAnimeOrder){
    // given scale, rotate, translateX: written perspective > translate > rotate > scale > skew
    auto animation = Anime::animate(target("box"), AnimParam::object({
        {"scale", 2}, {"rotate", "1turn"}, {"x", 100}, {"skewX", 10}, {"duration", 1000}, {"ease", "linear"}}));
    advance(500);
    Rml::Element* box = element("box");
    using P = Rml::TransformPrimitive;
    EXPECT_EQ(transformTypes(box), (std::vector<P::Type>{P::TRANSLATEX, P::ROTATE2D, P::SCALE2D, P::SKEWX}));

    const Rml::TransformPtr t = box->GetLocalProperty(Rml::PropertyId::Transform)->Get<Rml::TransformPtr>();
    const auto& primitives = t->GetPrimitives();
    EXPECT_FLOAT_EQ(primitives[0].translate_x.values[0].number, 50.f);
    EXPECT_EQ(primitives[0].translate_x.values[0].unit, Rml::Unit::PX);
    // half a turn, in radians once resolved
    EXPECT_NEAR(primitives[1].rotate_2d.values[0], 3.14159265f, 1e-4);
    EXPECT_FLOAT_EQ(primitives[2].scale_2d.values[0], 1.5f);

    advance(600);
    EXPECT_TRUE(animation->isCompleted());
    EXPECT_FLOAT_EQ(box->GetLocalProperty(Rml::PropertyId::Transform)->Get<Rml::TransformPtr>()->GetPrimitives()[0].translate_x.values[0].number, 100.f);

    // a second animation keeps the other individual transforms
    Anime::animate(target("box"), AnimParam::object({{"translateY", 20}, {"duration", 100}, {"ease", "linear"}}));
    advance(200);
    EXPECT_EQ(transformTypes(box), (std::vector<P::Type>{P::TRANSLATEX, P::TRANSLATEY, P::ROTATE2D, P::SCALE2D, P::SKEWX}));
    EXPECT_EQ(AnimUtils::get(target("box"), "translateX").toString(), "100px");
    EXPECT_EQ(AnimUtils::get(target("box"), "y").toString(), "20px");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(AnimeElementTest, InlineTransformIsTheStartingPoint){
    Rml::Element* styled = element("styled");
    // the inline translateY(10px) rotate(5deg) is split into individual transforms
    EXPECT_EQ(AnimUtils::get(target("styled"), "translateY").toString(), "10px");
    EXPECT_EQ(AnimUtils::get(target("styled"), "rotate").toString(), "5deg");

    auto animation = Anime::animate(target("styled"), AnimParam::object({{"rotate", 45}, {"translateX", "2em"}, {"duration", 100}, {"ease", "linear"}}));
    advance(200);
    using P = Rml::TransformPrimitive;
    EXPECT_EQ(transformTypes(styled), (std::vector<P::Type>{P::TRANSLATEX, P::TRANSLATEY, P::ROTATE2D}));
    const auto& primitives = styled->GetLocalProperty(Rml::PropertyId::Transform)->Get<Rml::TransformPtr>()->GetPrimitives();
    EXPECT_EQ(primitives[0].translate_x.values[0].unit, Rml::Unit::EM);
    EXPECT_NEAR(primitives[2].rotate_2d.values[0], 45 * 3.14159265f / 180, 1e-5);

    // revert: the original inline transform comes back
    animation->revert();
    EXPECT_EQ(AnimUtils::get(target("styled"), "rotate").toString(), "5deg");
    EXPECT_EQ(transformTypes(styled), (std::vector<P::Type>{P::TRANSLATEY, P::ROTATE2D}));
}

TEST_F(AnimeElementTest, TransformAndOpacityDoNotDirtyTheLayout){
    Rml::Element* doc = element("box")->GetOwnerDocument();
    updateDocuments(scene());
    ASSERT_FALSE(LayoutProbe::isDirty(doc));

    Anime::animate(target("box"), AnimParam::object({{"translateX", 100}, {"rotate", 30}, {"opacity", 0.2}, {"duration", 1000}}));
    advance(100);
    LayoutProbe::updateStyle(doc);
    EXPECT_FALSE(LayoutProbe::isDirty(doc));
    advance(100);
    LayoutProbe::updateStyle(doc);
    EXPECT_FALSE(LayoutProbe::isDirty(doc));
    updateDocuments(scene());
    EXPECT_LT(element("box")->GetComputedValues().opacity(), 1.0f);

    Anime::animate(target("box"), AnimParam::object({{"width", 300}, {"duration", 1000}}));
    advance(100);
    LayoutProbe::updateStyle(doc);
    EXPECT_TRUE(LayoutProbe::isDirty(doc));
}

TEST_F(AnimeElementTest, LengthsColoursAndCustomProperties){
    auto animation = Anime::animate(target("box"), AnimParam::object({
        {"width", 200}, {"backgroundColor", "#0000ff"}, {"opacity", 0}, {"duration", 1000}, {"ease", "linear"}}));
    Anime::animate(target("vars"), AnimParam::object({{"--glow", "10px"}, {"duration", 1000}, {"ease", "linear"}}));
    Anime::animate(target("label"), AnimParam::object({{"textContent", 100}, {"modifier", AnimParam::ease([](double v){ return AnimMath::round(v, 0); })},
                                                        {"duration", 1000}, {"ease", "linear"}}));
    advance(500);
    Rml::Element* box = element("box");
    // width from the used 100px: typed px
    const Rml::Property* width = box->GetLocalProperty(Rml::PropertyId::Width);
    ASSERT_NE(width, nullptr);
    EXPECT_EQ(width->unit, Rml::Unit::PX);
    EXPECT_FLOAT_EQ(width->Get<float>(), 150.f);
    const Rml::Property* colour = box->GetLocalProperty(Rml::PropertyId::BackgroundColor);
    ASSERT_NE(colour, nullptr);
    EXPECT_EQ(colour->unit, Rml::Unit::COLOUR);
    const Rml::Colourb c = colour->Get<Rml::Colourb>();
    // pseudo-linear blend of red and blue at 0.5: sqrt(255^2 / 2) = 180
    EXPECT_EQ((int)c.red, 180);
    EXPECT_EQ((int)c.green, 0);
    EXPECT_EQ((int)c.blue, 180);
    const Rml::Property* opacity = box->GetLocalProperty(Rml::PropertyId::Opacity);
    ASSERT_NE(opacity, nullptr);
    EXPECT_FLOAT_EQ(opacity->Get<float>(), 0.5f);

    const Rml::Property* glow = element("vars")->GetLocalProperty("--glow");
    ASSERT_NE(glow, nullptr);
    EXPECT_NE(glow->ToString().find("5px"), std::string::npos) << glow->ToString();
    EXPECT_EQ(document->getElementById("label").getTextContent(), "50");

    advance(600);
    updateDocuments(scene());
    EXPECT_FLOAT_EQ(box->GetBox().GetSize(Rml::BoxArea::Content).x, 200.f);
    EXPECT_EQ(document->getElementById("label").getTextContent(), "100");

    // revert: no inline width/colour/opacity left
    animation->revert();
    EXPECT_EQ(inlineProperty(box, Rml::PropertyId::Width), nullptr);
    EXPECT_EQ(inlineProperty(box, Rml::PropertyId::BackgroundColor), nullptr);
    EXPECT_EQ(inlineProperty(box, Rml::PropertyId::Opacity), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(AnimeElementTest, PrivatePreviewSelectorsStayWithinTheirScene){
    const auto original = AnimTargets::fromSelector("#box");
    ASSERT_EQ(original.size(), 1u);
    Scene privateScene(EntityPool::System);
    UIDocument privateDocument(&privateScene);
    ASSERT_TRUE(privateDocument.load("p1/anime.rml"));
    updateDocuments(privateScene);
    const auto privateTarget = AnimTargets::fromElement(privateDocument.getElementById("box"));
    ASSERT_NE(privateTarget, original.front());
    {
        AnimTargets::ScopedSceneTargets scope(&privateScene);
        ASSERT_EQ(AnimTargets::fromSelector("#box").size(), 1u);
        EXPECT_EQ(AnimTargets::fromSelector("#box").front(), privateTarget);
        Scene emptyScene(EntityPool::System);
        {
            AnimTargets::ScopedSceneTargets nested(&emptyScene);
            EXPECT_TRUE(AnimTargets::fromSelector("#box").empty());
            EXPECT_EQ(AnimTargets::fromSelector("#box", document->getBody()).front(), original.front());
        }
        EXPECT_EQ(AnimTargets::fromSelector("#box").front(), privateTarget);
    }
    EXPECT_EQ(AnimTargets::fromSelector("#box").front(), original.front());
}

TEST_F(AnimeElementTest, SelectorsAndStaggeredDelays){
    AnimTargetList cards = AnimTargets::fromSelector(".card");
    ASSERT_EQ(cards.size(), 3u);
    // the same element, the same target
    EXPECT_EQ(AnimTargets::fromSelector("#box")[0], AnimTargets::fromElement(document->getElementById("box")));
    // scoped to a root
    EXPECT_EQ(AnimTargets::fromSelector(".card", document->getElementById("box")).size(), 0u);
    EXPECT_EQ(AnimTargets::fromSelector(".card", document->getElementById("holder")).size(), 3u);

    Anime::animate(cards, AnimParam::object({{"opacity", 0}, {"delay", AnimUtils::stagger(AnimParam(100))}, {"duration", 100}, {"ease", "linear"}}));
    advance(150);
    std::vector<float> opacities;
    for (const AnimTargetPtr& card : cards)
        opacities.push_back(AnimTargets::getElement(card).isValid() ? UIElementAccess::get(AnimTargets::getElement(card))->GetLocalProperty(Rml::PropertyId::Opacity)->Get<float>() : -1);
    EXPECT_FLOAT_EQ(opacities[0], 0.f);
    EXPECT_FLOAT_EQ(opacities[1], 0.5f);
    EXPECT_FLOAT_EQ(opacities[2], 1.f);
}

TEST_F(AnimeElementTest, GoneTargetsCancelTheAnimationSilently){
    int updates = 0, completes = 0, pauses = 0;
    AnimParam params = AnimParam::object({{"translateX", 100}, {"duration", 1000}});
    params.set("onUpdate", AnimParam::callback([&](AnimTimer&){ updates++; }));
    params.set("onComplete", AnimParam::callback([&](AnimTimer&){ completes++; }));
    params.set("onPause", AnimParam::callback([&](AnimTimer&){ pauses++; }));
    auto animation = Anime::animate(target("dyn"), params);
    advance(100);
    EXPECT_EQ(updates, 1);
    EXPECT_EQ(AnimEngine::instance().getActiveCount(), 1u);

    document->getElementById("holder").setInnerHTML("<p>gone</p>");
    EXPECT_FALSE(document->getElementById("dyn").isValid());
    advance(100);
    advance(2000);
    EXPECT_EQ(updates, 1);
    EXPECT_EQ(completes, 0);
    EXPECT_EQ(pauses, 0);
    EXPECT_TRUE(animation->isCancelled());
    EXPECT_EQ(AnimEngine::instance().getActiveCount(), 0u);

    // a mix of live and gone targets keeps running on the live ones
    auto mixed = Anime::animate(AnimTargetList{AnimTargets::fromElement(document->getElementById("box")), animation->getTargets()[0]},
                                AnimParam::object({{"opacity", 0.5}, {"duration", 100}}));
    advance(200);
    EXPECT_TRUE(mixed->isCompleted());
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(AnimeElementTest, ElementTargetsGoWithTheDocument){
    const size_t before = AnimTargets::getElementTargetCount();
    {
        auto animation = Anime::animate(AnimTargets::fromSelector(".card"), AnimParam::object({{"opacity", 0}, {"duration", 100}}));
        EXPECT_EQ(AnimTargets::getElementTargetCount(), before + 3);
        advance(200);
    }
    // completed and released: the engine drops it, and its targets
    advance(16);
    EXPECT_EQ(AnimTargets::getElementTargetCount(), before);

    // a running animation whose document unloads: cancelled at the next tick, targets freed by reset
    Anime::animate(target("box"), AnimParam::object({{"x", 10}, {"duration", 1000}}));
    document->load("ecs/basic.rml");
    advance(16);
    EXPECT_EQ(AnimEngine::instance().getActiveCount(), 0u);
    AnimEngine::instance().reset();
    EXPECT_EQ(AnimTargets::getElementTargetCount(), 0u);
}

TEST_F(AnimeElementTest, UnitConversionOnElements){
    // from 100px (used width) to 50%: the from value converted to % of the parent
    Anime::animate(target("box"), AnimParam::object({{"left", "50%"}, {"duration", 100}, {"ease", "linear"}}));
    advance(200);
    const Rml::Property* left = element("box")->GetLocalProperty(Rml::PropertyId::Left);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->unit, Rml::Unit::PERCENT);
    EXPECT_FLOAT_EQ(left->Get<float>(), 50.f);

    // utils.get with a unit converts
    AnimTargetList box = target("box");
    EXPECT_EQ(AnimUtils::get(box, "width", "px").toString(), "100px");
    EXPECT_EQ(AnimUtils::get(box, "width", "", true).number, 100);
}

// --- clocks through the engine frame ---

TEST_F(AnimeElementTest, UiClockRunsWhileTheGameIsFrozen){
    AnimEngine::instance().reset();
    HtmlUIClock::setFixedTime(std::nullopt);
    auto uiTarget = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    auto gameTarget = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    Engine::setFixedDeltatime(0.02);
    Engine::setTimeScale(0.0f);
    GpuEnvironment::drawFrames(1);
    auto ui = Anime::animate({uiTarget}, AnimParam::object({{"v", 1000}, {"duration", 1000}, {"ease", "linear"}}));
    auto game = Anime::animate({gameTarget}, AnimParam::object({{"v", 1000}, {"duration", 1000}, {"ease", "linear"}, {"clock", "game"}}));
    EXPECT_EQ(game->getClock(), AnimClock::GAME);

    GpuEnvironment::drawFrames(5);
    EXPECT_NEAR(uiTarget->getNumber("v"), 100, 1e-6);
    EXPECT_NEAR(gameTarget->getNumber("v"), 0, 1e-6);

    // half speed: the game clock advances 10 ms a frame
    Engine::setTimeScale(0.5f);
    GpuEnvironment::drawFrames(5);
    EXPECT_NEAR(uiTarget->getNumber("v"), 200, 1e-6);
    EXPECT_NEAR(gameTarget->getNumber("v"), 50, 1e-6);

    // a pinned UI clock pins anime too (and the RmlUi animations, the Lottie players)
    HtmlUIClock::setFixedTime(HtmlUIClock::now());
    GpuEnvironment::drawFrames(3);
    EXPECT_NEAR(uiTarget->getNumber("v"), 200, 1e-6);
    EXPECT_NEAR(gameTarget->getNumber("v"), 80, 1e-6);
    HtmlUIClock::setFixedTime(std::nullopt);

    // an invalid clock is an error naming the valid values
    TestSystem::get().clearLogs();
    Anime::createTimer(AnimParam::object({{"clock", "real"}}));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "\"ui\""), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "\"game\""), 1u);
}

TEST_F(AnimeElementTest, TimelineChildrenInheritTheGameClock){
    AnimEngine::instance().reset();
    HtmlUIClock::setFixedTime(std::nullopt);
    auto t = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    Engine::setFixedDeltatime(0.02);
    Engine::setTimeScale(0.0f);
    GpuEnvironment::drawFrames(1);
    auto tl = Anime::createTimeline(AnimParam::object({{"clock", "game"}, {"defaults", AnimParam::object({{"ease", "linear"}})}}));
    tl->add({t}, AnimParam::object({{"v", 100}, {"duration", 100}}));
    GpuEnvironment::drawFrames(5);
    EXPECT_EQ(t->getNumber("v"), 0);
    Engine::setTimeScale(1.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(t->getNumber("v"), 40, 1e-6);
    EXPECT_EQ(tl->childHead->getClock(), AnimClock::GAME);
}

TEST_F(AnimeElementTest, TickIsIdempotentPerTime){
    auto t = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    int updates = 0;
    AnimParam params = AnimParam::object({{"v", 100}, {"duration", 1000}, {"ease", "linear"}});
    params.set("onUpdate", AnimParam::callback([&](AnimTimer&){ updates++; }));
    Anime::animate({t}, params);
    advance(100);
    // every scene's UIDocumentSystem ticks: the same time ticks once
    AnimEngine::instance().tick(HtmlUIClock::now());
    updateDocuments(scene());
    AnimEngine::instance().tick(HtmlUIClock::now());
    EXPECT_EQ(updates, 1);
    EXPECT_NEAR(t->getNumber("v"), 10, 1e-9);
}

// Phase 3 (p3-animation; phase-3 spec 3.13.4, reconciliation decision 3): transitions start on
// any change of a computed value, so a property anime writes every frame that also has a CSS
// transition starts that transition again at each write, as Anime.js does in a browser: the
// computed value follows the written one with the transition's lag, and settles on the last one.
TEST_F(AnimeElementTest, CssTransitionRestartsOnEveryWrite){
    Rml::Element* box = element("box");
    box->SetProperty("transition", "opacity 0.5s linear");
    updateDocuments(scene());

    struct Counter : Rml::EventListener {
        int count = 0;
        void ProcessEvent(Rml::Event&) override{ count++; }
    } runs;
    box->AddEventListener(Rml::EventId::Transitionrun, &runs);

    auto animation = Anime::animate(target("box"), AnimParam::object({{"opacity", 0}, {"duration", 1000}, {"ease", "linear"}}));
    for (int i = 0; i < 10; i++){
        advance(50);
        updateDocuments(scene());
    }
    const Rml::Property* written = inlineProperty(box, Rml::PropertyId::Opacity);
    ASSERT_NE(written, nullptr);
    EXPECT_NEAR(written->Get<float>(), 0.5f, 0.01f);
    EXPECT_GT(box->GetComputedValues().opacity(), written->Get<float>() + 0.05f);
    EXPECT_GE(runs.count, 9);

    for (int i = 0; i < 30; i++){
        advance(50);
        updateDocuments(scene());
    }
    EXPECT_TRUE(animation->isCompleted());
    EXPECT_NEAR(box->GetComputedValues().opacity(), 0.0f, 1e-4f);
    box->RemoveEventListener(Rml::EventId::Transitionrun, &runs);
}
