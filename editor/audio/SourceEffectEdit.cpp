// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SourceEffectEdit.h"

#include "Project.h"
#include "Scene.h"
#include "audio/AudioEffects.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/AudioSourceComponent.h"

#include <cctype>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string sourceEffectKey(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c == ' ' || c == '_' || c == '-') continue;
            out += (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    std::string sourceEffectTypeError(const std::string& where, const std::string& type) {
        return "audiosource." + where + ": '" + type + "' is not an effect an Audio Source can hold. Valid types: " +
               AudioEffects::perSourceNames() + " (duck and limiter work on mixer groups: audio_mixer).";
    }

    // the parameters of `params` an effect of `type` does not have
    bool checkSourceEffectParams(const std::string& where, const AudioEffectInfo& info, const nlohmann::json& params, std::string& error) {
        if (params.is_null()) return true;
        if (!params.is_object()) {
            error = "audiosource." + where + " must be an object of parameter names to numbers ({\"cutoff\": 500}).";
            return false;
        }
        std::string valid;
        for (const AudioEffectParamInfo& param : info.params) valid += (valid.empty() ? "" : ", ") + std::string(param.name);
        for (auto it = params.begin(); it != params.end(); ++it) {
            const AudioEffectParamInfo* param = it.key() == "wet" ? nullptr : AudioEffects::findParam(info.type, it.key());
            if (!param) {
                error = "audiosource." + where + ": " + info.name + " has no parameter '" + it.key() + "'. Its parameters: " + valid +
                        (it.key() == "wet" ? " (wet is the effect's own key, beside type)." : ".");
                return false;
            }
            if (!it.value().is_number()) {
                error = "audiosource." + where + "." + it.key() + " must be a number.";
                return false;
            }
        }
        return true;
    }

    // one effect object {type, bypass, wet, params}
    bool checkSourceEffect(const std::string& where, const nlohmann::json& effect, std::string& error) {
        if (!effect.is_object() || !effect.contains("type") || !effect["type"].is_string()) {
            error = "audiosource." + where + " must be an object with a type ({\"type\": \"lowpass\", \"params\": {\"cutoff\": 500}}).";
            return false;
        }
        const AudioEffectInfo* info = AudioEffects::findPerSource(effect["type"].get<std::string>());
        if (!info) {
            error = sourceEffectTypeError(where + ".type", effect["type"].get<std::string>());
            return false;
        }
        return checkSourceEffectParams(where + ".params", *info, effect.contains("params") ? effect["params"] : nlohmann::json(), error);
    }

    std::string sourceEffectLimit() {
        return "an Audio Source holds up to " + std::to_string(AudioSourceComponent::maxEffects) +
               " effects; route the sources through a mixer group for more.";
    }

}

const std::vector<SourceEffectEdit::FilterAlias>& SourceEffectEdit::filterAliases() {
    static const std::vector<FilterAlias> aliases = {
        {"Audio Low Pass Filter", "lowpass", "Adds a Low Pass to this GameObject's Audio Source: cuts the frequencies above the cutoff"},
        {"Audio High Pass Filter", "highpass", "Adds a High Pass to this GameObject's Audio Source: cuts the frequencies below the cutoff"},
        {"Audio Echo Filter", "echo", "Adds an Echo to this GameObject's Audio Source"},
        {"Audio Distortion Filter", "distortion", "Adds a Distortion to this GameObject's Audio Source"},
        {"Audio Reverb Filter", "reverb", "Adds a Reverb to this GameObject's Audio Source (cost: high; a mixer group shares one)"},
    };
    return aliases;
}

const SourceEffectEdit::FilterAlias* SourceEffectEdit::findFilterAlias(const std::string& name) {
    const std::string key = sourceEffectKey(name);
    for (const FilterAlias& alias : filterAliases()) {
        if (sourceEffectKey(alias.name) == key) return &alias;
    }
    return nullptr;
}

Command* SourceEffectEdit::addEffectCommand(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                            const std::string& effect, std::string& why) {
    SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
    const AudioEffectInfo* info = AudioEffects::findPerSource(effect);
    if (!scene || !scene->scene || !info) {
        why = info ? "The scene is not loaded." : sourceEffectTypeError("effects", effect);
        return nullptr;
    }
    auto* multi = new MultiPropertyCmd();
    for (Entity entity : entities) {
        std::vector<AudioSourceEffectDef> effects;
        if (const AudioSourceComponent* audio = scene->scene->findComponent<AudioSourceComponent>(entity)) {
            effects = audio->effects;
        } else {
            multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, ComponentType::AudioSourceComponent));
        }
        if ((int)effects.size() >= AudioSourceComponent::maxEffects) {
            why = "\"" + scene->scene->getEntityName(entity) + "\": " + sourceEffectLimit();
            delete multi;
            return nullptr;
        }
        AudioSourceEffectDef added;
        added.type = info->name;
        effects.push_back(added);
        multi->addPropertyCmd<std::vector<AudioSourceEffectDef>>(project, sceneId, entity, ComponentType::AudioSourceComponent, "effects", effects);
    }
    return multi;
}

bool SourceEffectEdit::checkChange(const AudioSourceComponent& current, const std::string& property, const std::string& op,
                                   const nlohmann::json& value, std::string& error) {
    if (property.compare(0, 7, "effects") != 0) return true;
    if (property == "effects") {
        if (op == "insert") {
            if ((int)current.effects.size() >= AudioSourceComponent::maxEffects) {
                error = "audiosource.effects: " + sourceEffectLimit();
                return false;
            }
            return checkSourceEffect("effects[]", value, error);
        }
        if (op != "set") return true;
        if (!value.is_array()) {
            error = "audiosource.effects must be an array of effects ([{\"type\": \"lowpass\", \"params\": {\"cutoff\": 500}}]).";
            return false;
        }
        if ((int)value.size() > AudioSourceComponent::maxEffects) {
            error = "audiosource.effects: " + sourceEffectLimit();
            return false;
        }
        for (size_t i = 0; i < value.size(); i++) {
            if (!checkSourceEffect("effects[" + std::to_string(i) + "]", value[i], error)) return false;
        }
        return true;
    }
    if (op != "set" || property.size() < 10 || property[7] != '[') return true;
    const size_t close = property.find(']');
    if (close == std::string::npos) return true;
    const size_t index = (size_t)std::strtoul(property.c_str() + 8, nullptr, 10);
    const std::string rest = property.substr(close + 1);
    const std::string where = property;
    if (index >= (size_t)AudioSourceComponent::maxEffects) {
        error = "audiosource." + where + ": " + sourceEffectLimit();
        return false;
    }
    if (rest.empty()) return checkSourceEffect(where, value, error);
    if (rest == ".type") {
        if (!value.is_string() || !AudioEffects::findPerSource(value.get<std::string>())) {
            error = sourceEffectTypeError(where, value.is_string() ? value.get<std::string>() : value.dump());
            return false;
        }
        return true;
    }
    if (rest == ".params") {
        const AudioEffectInfo* info = index < current.effects.size() ? AudioEffects::findPerSource(current.effects[index].type) : nullptr;
        // an effect being added in the same call is checked by its own change
        if (!info) return true;
        return checkSourceEffectParams(where, *info, value, error);
    }
    return true;
}
