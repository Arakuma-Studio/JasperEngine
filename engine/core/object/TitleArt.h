// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_H
#define TITLE_ART_H

// The script API of a Title Art Renderer (docs/specs/S81-title-art-runtime.md R64-R68, §6.6):
// a GameObject over an entity that has the component, for C++ scripts, the render gate and
// the Lua class TitleArt (TitleArtClassesLua.cpp, which raises the errors these return).
//
// Always built: naming TitleArt never pulls the `titleart` module (R52). `title` and the Render
// Mode are read-only and nothing here adds the component (R67), so every Realtime and Glyph
// Atlas title exists in shipped content. Parameters are the document's Exposed names only
// (R66). The calls that can fail return false with `error` (one sentence naming the title,
// what failed, why and the fix, R65); with a null `error` the sentence goes to the Console.

#include "GameObject.h"
#include "component/TitleArtRendererComponent.h"
#include "math/Vector4.h"
#include "util/FunctionSubscribe.h"

#include <string>
#include <vector>

namespace jasperengine::titleanim{
    struct TitleAnimPlayOptions;
}

namespace jasperengine{

    struct TitleArtValue;
    struct AnimationEvent;

    class JASPERENGINE_API TitleArt: public GameObject{
    public:
        TitleArt(Scene* scene, Entity entity);
        virtual ~TitleArt();

        // the entity has a Title Art Renderer
        bool isValid() const;

        std::string getTitle() const;
        TitleArtRenderMode getRenderMode() const;

        std::string getText() const;
        bool setText(const std::string& text, std::string* error = nullptr);
        std::string getColorScheme() const;
        bool setColorScheme(const std::string& id, std::string* error = nullptr);
        std::vector<std::string> getColorSchemes() const;
        std::vector<std::string> getParameterNames() const;

        bool setParameter(const std::string& name, const TitleArtValue& value, std::string* error = nullptr);
        bool setParameter(const std::string& name, double value, std::string* error = nullptr);
        bool setParameter(const std::string& name, bool value, std::string* error = nullptr);
        bool setParameter(const std::string& name, const std::string& value, std::string* error = nullptr);
        bool setParameter(const std::string& name, const Vector4& linearColor, std::string* error = nullptr);
        // the effective value (R5); false with `error` for a name the document does not expose
        bool getParameter(const std::string& name, TitleArtValue& out, std::string* error = nullptr) const;
        bool hasParameter(const std::string& name) const;
        bool resetParameter(const std::string& name, std::string* error = nullptr);

        // R18, R19, R21
        bool isReady() const;
        std::string getPending() const;
        bool isFailed() const;
        std::string getError() const;
        std::string getPixelPath() const;

        int getPriority() const;
        void setPriority(int priority);

        // re-render now (Realtime, Glyph Atlas); nothing on Baked
        void refresh();
        bool setNativeSize(std::string* error = nullptr);

        // Live Effects (R33-R38)
        float getLiveTime() const;
        void setLiveTime(float seconds);
        float getSweepPosition() const;
        void setSweepPosition(float position);
        float getSweepWidth() const;
        void setSweepWidth(float width);
        float getSweepAngle() const;
        void setSweepAngle(float degrees);
        float getSweepSoftness() const;
        void setSweepSoftness(float softness);
        Vector4 getSweepColor() const;
        void setSweepColor(const Vector4& color);
        bool isSweepAuto() const;
        void setSweepAuto(bool automatic);
        float getGlowPulse() const;
        void setGlowPulse(float pulse);
        Vector4 getGlowColor() const;
        void setGlowColor(const Vector4& color);
        Vector4 getTintColor() const;
        void setTintColor(const Vector4& color);
        float getTintAmount() const;
        void setTintAmount(float amount);
        float getGlintAmount() const;
        void setGlintAmount(float amount);
        float getGlintSize() const;
        void setGlintSize(float size);
        float getGlintRate() const;
        void setGlintRate(float rate);
        unsigned int getGlintSeed() const;
        void setGlintSeed(unsigned int seed);
        Vector4 getGlintColor() const;
        void setGlintColor(const Vector4& color);

        // S85's animations (docs/specs/S85-title-animation.md §6.5): false and one warning per
        // name for an unknown animation (R30)
        std::vector<std::string> getAnimations() const;
        bool isPlaying() const;
        bool play(const std::string& name);
        bool play(const std::string& name, const titleanim::TitleAnimPlayOptions& options);
        bool playOverlay(const std::string& name);
        bool playOverlay(const std::string& name, const titleanim::TitleAnimPlayOptions& options);
        // every animation (the pose stays), or one by name
        void stop();
        bool stop(const std::string& name);
        // back to the still: the title is idle again
        void reset();
        bool isPaused() const;
        void setPaused(bool paused);
        bool isAnimationPlaying(const std::string& name) const;
        bool prepare(const std::string& name);
        bool isAnimationReady(const std::string& name) const;
        // shows `name` at `time` without playing (pauses)
        bool sample(const std::string& name, double time);
        // seconds; -1 for an unknown name
        double getDuration(const std::string& name) const;
        // the base animation ("" none), its time (a write seeks, no markers) and the speed of all
        std::string getAnimation() const;
        float getAnimationTime() const;
        void setAnimationTime(float seconds);
        float getAnimationSpeed() const;
        void setAnimationSpeed(float speed);
        FunctionSubscribe<void(const AnimationEvent&)>& getOnAnimationEvent();

        // events (Lua: t.onReady:add("tag", fn) or t.onReady = fn)
        FunctionSubscribe<void()>& getOnReady();
        FunctionSubscribe<void(const std::string&)>& getOnFailed();
        FunctionSubscribe<void(const std::string&)>& getOnMissingCharacters();
        FunctionSubscribe<void(const std::string&)>& getOnAnimationFinished();

        // the frame budget of every title (R15): 0.25..16 ms
        static void setFrameBudget(double ms);
        static double getFrameBudget();

    private:
        TitleArtRendererComponent* component() const;
        bool fail(const std::string& message, std::string* error) const;
    };

}

#endif //TITLE_ART_H
