// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TEXTEFFECT_H
#define TEXTEFFECT_H

#include "Scene.h"
#include "Log.h"
#include "component/TextEffectComponent.h"

#include <algorithm>
#include <memory>

namespace jasperengine{

    // One layer of a Text Effect for scripts (textEffect:getLayer(i)): reads and writes go to
    // the component, so `fx:getLayer(1).amplitude = 0.3` changes the running effect
    class JASPERENGINE_API TextEffectLayerRef{
    private:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        int index = 0;
        std::weak_ptr<char> lifetime;

        TextEffectLayer* layer() const {
            if (!scene || lifetime.expired())
                return nullptr;
            TextEffectComponent* effect = scene->findComponent<TextEffectComponent>(entity);
            return (effect && index >= 0 && index < MAX_TEXT_EFFECT_LAYERS) ? &effect->layers[index] : nullptr;
        }

        template<typename T, typename F>
        T read(F get, T fallback) const {
            TextEffectLayer* l = layer();
            return l ? get(*l) : fallback;
        }

        template<typename F>
        void write(F set) {
            TextEffectLayer* l = layer();
            if (!l){
                Log::warn("TextEffectLayer: its Text Effect is gone");
                return;
            }
            set(*l);
            if (TextEffectComponent* effect = scene->findComponent<TextEffectComponent>(entity)){
                effect->needApply = true;
                effect->editedTypes[index] = l->type;
            }
        }

    public:
        TextEffectLayerRef() = default;
        TextEffectLayerRef(Scene* scene, Entity entity, int index)
            : scene(scene), entity(entity), index(index), lifetime(scene ? scene->getLifetime() : std::weak_ptr<char>()) {}

        TextEffectType getType() const { return read([](const TextEffectLayer& l){ return l.type; }, TextEffectType::None); }
        // an amplitude left at the old type's default takes the new one's (1 for Grow, Unfold)
        void setType(TextEffectType type) { write([&](TextEffectLayer& l){ TextEffects::setType(l, type); }); }
        bool isEnabled() const { return read([](const TextEffectLayer& l){ return l.enabled; }, false); }
        void setEnabled(bool enabled) { write([&](TextEffectLayer& l){ l.enabled = enabled; }); }
        float getAmplitude() const { return read([](const TextEffectLayer& l){ return l.amplitude; }, 0.0f); }
        void setAmplitude(float value) { write([&](TextEffectLayer& l){ l.amplitude = value; }); }
        float getDuration() const { return read([](const TextEffectLayer& l){ return l.duration; }, 0.0f); }
        void setDuration(float value) { write([&](TextEffectLayer& l){ l.duration = value; }); }
        float getSpread() const { return read([](const TextEffectLayer& l){ return l.spread; }, 0.0f); }
        void setSpread(float value) { write([&](TextEffectLayer& l){ l.spread = value; }); }
        float getPhaseStep() const { return read([](const TextEffectLayer& l){ return l.phaseStep; }, 0.0f); }
        void setPhaseStep(float value) { write([&](TextEffectLayer& l){ l.phaseStep = value; }); }
        float getFrequency() const { return read([](const TextEffectLayer& l){ return l.frequency; }, 0.0f); }
        void setFrequency(float value) { write([&](TextEffectLayer& l){ l.frequency = value; }); }
        float getCharactersPerSecond() const { return read([](const TextEffectLayer& l){ return l.charactersPerSecond; }, 0.0f); }
        void setCharactersPerSecond(float value) { write([&](TextEffectLayer& l){ l.charactersPerSecond = value; }); }
        float getFadeTime() const { return read([](const TextEffectLayer& l){ return l.fadeTime; }, 0.0f); }
        void setFadeTime(float value) { write([&](TextEffectLayer& l){ l.fadeTime = value; }); }
        std::vector<TextEffectGradientStop> getGradient() const {
            return read([](const TextEffectLayer& l){ return l.gradient; }, std::vector<TextEffectGradientStop>());
        }
        void setGradient(std::vector<TextEffectGradientStop> gradient) { write([&](TextEffectLayer& l){ l.gradient = gradient; }); }
        TextEffectMix getMix() const { return read([](const TextEffectLayer& l){ return l.mix; }, TextEffectMix::Multiply); }
        void setMix(TextEffectMix mix) { write([&](TextEffectLayer& l){ l.mix = mix; }); }
        float getSaturation() const { return read([](const TextEffectLayer& l){ return l.saturation; }, 0.0f); }
        void setSaturation(float value) { write([&](TextEffectLayer& l){ l.saturation = value; }); }
        float getLightness() const { return read([](const TextEffectLayer& l){ return l.lightness; }, 0.0f); }
        void setLightness(float value) { write([&](TextEffectLayer& l){ l.lightness = value; }); }
    };

    // The Text Effect of a Text's entity for scripts: TextEffect(scene, entity) adds the
    // component when the entity has none. play/stop/finish/restart act on the next UI update.
    class JASPERENGINE_API TextEffect{
    private:
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        std::weak_ptr<char> lifetime;

        TextEffectComponent* component() const {
            if (!scene || lifetime.expired())
                return nullptr;
            return scene->findComponent<TextEffectComponent>(entity);
        }

        TextEffectComponent& get() const {
            static TextEffectComponent gone;
            TextEffectComponent* effect = component();
            if (!effect){
                Log::warn("TextEffect: the entity has no Text Effect any more");
                gone = TextEffectComponent();
                return gone;
            }
            return *effect;
        }

    public:
        TextEffect(Scene* scene, Entity entity)
            : scene(scene), entity(entity), lifetime(scene ? scene->getLifetime() : std::weak_ptr<char>()) {
            if (scene && scene->isEntityCreated(entity) && !scene->findComponent<TextEffectComponent>(entity))
                scene->addComponent<TextEffectComponent>(entity, {});
        }

        Scene* getScene() const { return scene; }
        Entity getEntity() const { return entity; }

        bool isEnabled() const { return get().enabled; }
        void setEnabled(bool enabled) { get().enabled = enabled; get().needApply = true; }
        bool isAutoPlay() const { return get().autoPlay; }
        void setAutoPlay(bool autoPlay) { get().autoPlay = autoPlay; }
        bool isLoop() const { return get().loop; }
        void setLoop(bool loop) { get().loop = loop; }
        bool isUseUnscaledTime() const { return get().useUnscaledTime; }
        void setUseUnscaledTime(bool useUnscaledTime) { get().useUnscaledTime = useUnscaledTime; }
        int getStartIndex() const { return get().startIndex; }
        void setStartIndex(int startIndex) { get().startIndex = std::max(startIndex, 0); get().needApply = true; }
        int getLength() const { return get().length; }
        void setLength(int length) { get().length = std::max(length, -1); get().needApply = true; }
        std::string getForWords() const { return get().forWords; }
        void setForWords(const std::string& forWords) { get().forWords = forWords; get().needApply = true; }

        // 1-based, like Lua arrays
        TextEffectLayerRef getLayer(int index) const {
            if (index < 1 || index > MAX_TEXT_EFFECT_LAYERS)
                Log::warn("TextEffect.getLayer: index %i is not 1..%i", index, MAX_TEXT_EFFECT_LAYERS);
            return TextEffectLayerRef(scene, entity, index - 1);
        }
        void setLayer(int index, const TextEffectLayer& layer) {
            if (index < 1 || index > MAX_TEXT_EFFECT_LAYERS){
                Log::warn("TextEffect.setLayer: index %i is not 1..%i", index, MAX_TEXT_EFFECT_LAYERS);
                return;
            }
            get().layers[index - 1] = layer;
            get().editedTypes[index - 1] = layer.type;
            get().needApply = true;
        }

        // from the start (TMP Play)
        void play() { TextEffectComponent& e = get(); e.started = true; e.playing = true; e.needRestart = true; }
        // back to the plain text
        void stop() { TextEffectComponent& e = get(); e.started = true; e.playing = false; e.finished = false; e.needApply = true; }
        // every one-shot layer at its end at once (onComplete fires)
        void finish() {
            TextEffectComponent& e = get();
            e.started = true;
            e.playing = true;
            e.cycleTime = 1e9;
            e.needApply = true;
        }
        void restart() { play(); }
        bool isFinished() const { return get().finished; }
        bool isPlaying() const { return get().playing; }

        FunctionSubscribe<void()>& getOnComplete() { return get().onComplete; }
        FunctionSubscribe<void(int)>& getOnCharacterRevealed() { return get().onCharacterRevealed; }
    };

}

#endif //TEXTEFFECT_H
