// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_EFFECT_COMMON_H
#define HTMLUI_TEST_EFFECT_COMMON_H

// Shared helpers of the effects' rendering tests (engine/tests/htmlui/effects): a fixture
// template of this directory (paint.html, filter.html, backdrop.html, text.html, empty.html,
// sizes.html) with its declaration filled in, loaded through the compatibility layer as a
// virtual document into a FixtureRunner (the UI clock fixed), the effects' variants compiled
// before the capture, and image comparisons.

#include "FixtureRunner.h"
#include "TestRenderCommon.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/effects/EffectRegistry.h"
#include "htmlui/effects/EffectRuntime.h"

#include "Log.h"
#include "pool/ShaderPool.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::test::effects {

    using TestImage = jasperengine::test::compat::Image;

    inline std::string sourcePath(const std::string& relative){
        return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/" + relative;
    }

    inline std::string readText(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    inline bool fileExists(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        return (bool)file;
    }

    // every variant an effect may be drawn with, compiled (one variant a frame, as the game does)
    inline bool warmUpEffect(const htmlui::EffectManifest& m){
        std::vector<ShaderKey> keys;
        htmlui::EffectRegistry& registry = htmlui::EffectRegistry::get();
        if (m.kind == htmlui::EffectKind::Paint)
            keys.push_back(registry.shaderKey(m, false));
        keys.push_back(registry.shaderKey(m, true));
        if (!m.prepassThreshold.empty())
            keys.push_back(registry.brightPassKey());
        for (int i = 0; i < 400; i++){
            bool ready = true;
            for (ShaderKey key : keys){
                if (ShaderPool::getShaderState(key) == ResourceLoadState::Failed)
                    return false;
                ready = ShaderPool::warmUp(key) && ready;
            }
            if (ready)
                return true;
            beginTestFrame();
        }
        return false;
    }

    // the largest channel difference of two images (INT_MAX when their sizes differ)
    inline int maxDifference(const jasperengine::test::compat::Image& a, const jasperengine::test::compat::Image& b){
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size())
            return 1 << 30;
        int worst = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            worst = std::max(worst, std::abs((int)a.rgba[i] - (int)b.rgba[i]));
        return worst;
    }

    inline std::string logText(int type){
        std::string text;
        for (const auto& line : TestSystem::get().getLogs()){
            if (line.type == type)
                text += line.message + "\n";
        }
        return text;
    }

    // A document made of a fixture template, in a FixtureRunner
    class JASPERENGINE_INTERNAL EffectScene {
    public:
        explicit EffectScene(int width = 360, int height = 240, int samples = 4): runner(width, height, samples){}

        bool valid() const{ return runner.valid(); }

        // <fixture>.html of this directory with __DECL__ replaced
        bool load(const std::string& fixture, const std::string& declaration, std::string* error = nullptr){
            std::string html = readText(sourcePath("effects/" + fixture + ".html"));
            for (size_t at = html.find("__DECL__"); at != std::string::npos; at = html.find("__DECL__", at + declaration.size()))
                html.replace(at, 8, declaration);
            return loadHtml(html, error);
        }

        bool loadHtml(const std::string& html, std::string* error = nullptr){
            const std::string path = "@htmlui/test/effects/scene" + std::to_string(++counter) + ".html";
            htmlui::compat::VirtualFiles::add(path, html);
            const bool ok = runner.load(path, error);
            htmlui::compat::VirtualFiles::remove(path);
            return ok;
        }

        // a document of the asset root (engine/tests/htmlui), e.g. "fixtures/effects/lib-test.html"
        bool loadFile(const std::string& assetPath, std::string* error = nullptr){
            return runner.load(assetPath, error);
        }

        bool capture(jasperengine::test::compat::Image& image){
            FixtureRunner::Capture capture;
            if (!runner.capture(capture, true))
                return false;
            image = std::move(capture.image);
            return !image.empty();
        }

        Rml::Element* element(const std::string& id) const{
            return runner.document() ? runner.document()->GetElementById(id) : nullptr;
        }

        // the border box of an element in context px
        Rml::Rectanglef box(const std::string& id) const{
            Rml::Element* e = element(id);
            if (!e)
                return Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0.f), Rml::Vector2f(0.f));
            return Rml::Rectanglef::FromPositionSize(e->GetAbsoluteOffset(Rml::BoxArea::Border), e->GetBox().GetSize(Rml::BoxArea::Border));
        }

        Rml::Context* context() const{ return runner.context(); }
        FixtureRunner& fixture(){ return runner; }

    private:
        FixtureRunner runner;
        inline static int counter = 0;
    };

    // the golden harness's pointer for an element (70 %, 40 % of #fx, pressed 0.3 s before) and,
    // for trail effects, a 12-point arc over the last 0.5 s (newest first)
    inline void setTestPointer(EffectScene& scene, const htmlui::EffectManifest& m){
        const Rml::Rectanglef fx = scene.box("fx");
        const Rml::Vector2f pointer = fx.Position() + Rml::Vector2f(fx.Width() * 0.7f, fx.Height() * 0.4f);
        htmlui::EffectRuntime::get().setTestPointer(scene.context(), pointer, false, 0.3f);
        if (m.pointer == htmlui::EffectPointer::Trail){
            std::vector<Rml::Vector4f> trail;
            const Rml::Vector2f center = fx.Position() + fx.Size() * 0.5f;
            for (int k = 0; k < 12; k++){
                const float t = k / 11.f;
                const float angle = 3.14159265f * (0.1f + 0.8f * t);
                const Rml::Vector2f p = center + Rml::Vector2f(std::cos(angle) * fx.Width() * 0.32f, -std::sin(angle) * fx.Height() * 0.32f);
                trail.push_back(Rml::Vector4f(p.x, p.y, 0.5f * t, 1.f));
            }
            htmlui::EffectRuntime::get().setTestTrail(scene.context(), trail);
        }
    }

    // the effect's declaration in its fixture
    inline std::string declarationOf(const htmlui::EffectManifest& m, const std::string& arguments = std::string()){
        const std::string call = "effect(" + m.name + (arguments.empty() ? std::string() : ", " + arguments) + ")";
        if (m.kind == htmlui::EffectKind::Paint)
            return "background-image: " + call;
        return (m.backdrop ? "backdrop-filter: " : "filter: ") + call;
    }

    inline std::string noEffectDeclaration(const htmlui::EffectManifest& m){
        if (m.kind == htmlui::EffectKind::Paint)
            return "background-image: none";
        return m.backdrop ? "backdrop-filter: none" : "filter: none";
    }

    inline std::string fixtureOf(const htmlui::EffectManifest& m){
        if (m.kind == htmlui::EffectKind::Paint)
            return "paint";
        return m.backdrop ? "backdrop" : "filter";
    }

    // the fixed clocks of a deterministic capture, released by the destructor
    struct FixedEffectClock {
        explicit FixedEffectClock(double t){ htmlui::EffectClock::setFixedTime(t); }
        void set(double t){ htmlui::EffectClock::setFixedTime(t); }
        ~FixedEffectClock(){
            htmlui::EffectClock::setFixedTime(std::nullopt);
            htmlui::EffectRuntime::get().clearTestInput();
        }
    };

}

#endif //HTMLUI_TEST_EFFECT_COMMON_H
