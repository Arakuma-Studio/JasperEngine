// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The guide's effect examples (phase-5 spec 4.1 deliverable 4): every css block of the "Effects"
// section of docs/mcp/guide/html-ui.md is rendered on a page with one element per class it
// styles, over the test card (backdrop effects need something behind them). Each element shows
// its effects (getEffects) and paints something, and nothing logs a warning or an error. The
// project effect of "Writing an effect" is the fixture fixtures/effects/fx/my-glow.frag, which
// compiles and draws alongside the guide's CSS examples.

#include "EffectTestCommon.h"

#include "htmlui/effects/EffectInstancers.h"

#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    std::string effectsSection(){
        const std::string guide = readText(sourcePath("../../../docs/mcp/guide/html-ui.md"));
        const size_t start = guide.find("\n## Effects\n");
        if (start == std::string::npos)
            return std::string();
        const size_t end = guide.find("\n## ", start + 1);
        return guide.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // the fenced blocks of a language
    std::vector<std::string> blocks(const std::string& text, const std::string& lang){
        std::vector<std::string> out;
        const std::string fence = "```" + lang + "\n";
        for (size_t at = text.find(fence); at != std::string::npos; at = text.find(fence, at + 1)){
            const size_t begin = at + fence.size();
            const size_t end = text.find("```", begin);
            if (end == std::string::npos)
                break;
            out.push_back(text.substr(begin, end - begin));
        }
        return out;
    }

    // the classes a block styles, in order
    std::vector<std::string> classesOf(const std::string& css){
        std::vector<std::string> classes;
        std::set<std::string> seen;
        static const std::regex selector(R"(\.([a-z][a-z0-9-]*)\s*\{)");
        for (std::sregex_iterator it(css.begin(), css.end(), selector), end; it != end; ++it){
            const std::string name = (*it)[1];
            if (seen.insert(name).second)
                classes.push_back(name);
        }
        return classes;
    }

    // distinct colours in an element's box of the capture
    size_t colours(const TestImage& image, const Rml::Rectanglef& box){
        std::set<uint32_t> distinct;
        const int x0 = std::max(0, (int)box.Left()), y0 = std::max(0, (int)box.Top());
        const int x1 = std::min(image.width, (int)box.Right()), y1 = std::min(image.height, (int)box.Bottom());
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const uint8_t* p = &image.rgba[((size_t)y * image.width + x) * 4];
                distinct.insert((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
            }
        }
        return distinct.size();
    }

}

TEST(EffectGuideTest, EveryExampleRenders){
    const std::string section = effectsSection();
    const std::vector<std::string> css = blocks(section, "css");
    ASSERT_GE(css.size(), 5u) << "the guide's effect examples";
    EffectScene scene(640, 480, 1);
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    // the built-in effects the examples use, compiled
    for (const EffectManifest* m : EffectRegistry::get().builtins()){
        if (section.find("effect(" + m->name + ",") != std::string::npos || section.find("effect(" + m->name + ")") != std::string::npos){
            ASSERT_TRUE(warmUpEffect(*m)) << m->name;
        }
    }
    for (size_t index = 0; index < css.size(); index++){
        std::string sheet = css[index];
        // fx/my-glow.frag of the guide is the fixture's
        for (size_t at = sheet.find("url(\"fx/"); at != std::string::npos; at = sheet.find("url(\"fx/", at + 1))
            sheet.replace(at, 8, "url(\"/fixtures/effects/fx/");
        const std::vector<std::string> classes = classesOf(sheet);
        ASSERT_FALSE(classes.empty()) << "guide example " << index + 1 << " styles no class:\n" << css[index];
        std::string body = "<img id=\"card\" src=\"/effects/testcard.png\">\n";
        for (size_t i = 0; i < classes.size(); i++){
            const int left = 20 + (int)(i % 2) * 310, top = 20 + (int)(i / 2) * 150;
            body += "<div id=\"e" + std::to_string(i) + "\" class=\"" + classes[i] + "\" style=\"position: absolute; left: " + std::to_string(left) +
                    "px; top: " + std::to_string(top) + "px; width: 290px; height: 130px;\">FX 123</div>\n";
        }
        const std::string html = "<!DOCTYPE html><html><head><style>\n"
                                 "html, body { margin: 0; width: 640px; height: 480px; background: #101218; color: #fff; font-family: sans-serif; font-size: 32px; }\n"
                                 "#card { position: absolute; left: 0; top: 0; width: 640px; height: 480px; }\n" +
                                 sheet + "</style></head><body>\n" + body + "</body></html>\n";
        SCOPED_TRACE("guide example " + std::to_string(index + 1) + ":\n" + css[index]);
        TestSystem::get().clearLogs();
        std::string error;
        ASSERT_TRUE(scene.loadHtml(html, &error)) << error;
        for (const EffectManifest* m : EffectRegistry::get().all()){
            if (!m->builtin){
                ASSERT_TRUE(warmUpEffect(*m)) << m->name << " does not compile";
            }
        }
        TestImage image;
        {
            FixedEffectClock clock(1.5);
            EffectRuntime::get().setTestPointer(scene.context(), Rml::Vector2f(200.f, 80.f), false);
            ASSERT_TRUE(scene.capture(image));
            ASSERT_TRUE(scene.capture(image));
        }
        for (size_t i = 0; i < classes.size(); i++){
            Rml::Element* element = scene.element("e" + std::to_string(i));
            ASSERT_NE(element, nullptr);
            const std::vector<EffectUse> uses = collectEffectUses(element);
            EXPECT_FALSE(uses.empty()) << "." << classes[i] << " shows no effect";
            EXPECT_GT(colours(image, scene.box("e" + std::to_string(i))), 2u) << "." << classes[i] << " paints nothing";
        }
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u) << logText(D_LOG_WARN);
    }
    EffectRegistry::get().clearProjectEffects();
}
