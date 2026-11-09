// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleModuleRows.h"

#include "particles/ParticleEffectCodec.h"

#include <cctype>

using namespace jasperengine;
using namespace jasperengine::particles;
using Json = nlohmann::ordered_json;

namespace {

    constexpr const char* kClipboardKey = "particleModule";
    constexpr const char* kDocsUrl = "https://github.com/JasperRaccoon/JasperEngine/blob/main/docs/editor/particle-effect.md#";

    // the keyed form's own keys that are not the effect's fields
    bool documentField(const std::string& key) {
        return key != "version" && key != "modules" && key != "subEmitters" && key != "exposed";
    }

    bool applyPatch(ParticleEffectDocument& document, const Json& patch) {
        std::vector<Diagnostic> diagnostics;
        ParticleEffectDocument patched = document;
        if (!ParticleEffectCodec::applyMergePatch(patched, patch, diagnostics)) return false;
        const bool changed = ParticleEffectCodec::write(patched) != ParticleEffectCodec::write(document);
        document = patched;
        return changed;
    }

    Json moduleBody(const ParticleEffectDocument& document, const ModuleDesc& module) {
        const Json keyed = ParticleEffectCodec::toKeyedJson(document);
        if (!keyed.contains("modules") || !keyed["modules"].contains(module.key)) return Json::object();
        return keyed["modules"][module.key];
    }

}

std::set<std::string> editor::particlerows::nonDefaultFields(const ParticleEffectDocument& document) {
    std::set<std::string> fields;
    // the canonical form leaves defaults out: what it writes is what differs
    const Json keyed = ParticleEffectCodec::toKeyedJson(document);
    for (auto it = keyed.begin(); it != keyed.end(); ++it) {
        if (documentField(it.key())) fields.insert("effect." + it.key());
    }
    if (keyed.contains("modules")) {
        for (auto module = keyed["modules"].begin(); module != keyed["modules"].end(); ++module) {
            if (!module->is_object()) continue;
            for (auto field = module->begin(); field != module->end(); ++field) {
                if (field.key() != "enabled") fields.insert(module.key() + "." + field.key());
            }
        }
    }
    return fields;
}

bool editor::particlerows::resetField(ParticleEffectDocument& document, const std::string& path) {
    const size_t dot = path.find('.');
    if (dot == std::string::npos) return false;
    const std::string owner = path.substr(0, dot);
    const std::string key = path.substr(dot + 1);
    if (key.empty() || key.find('.') != std::string::npos || key.find('[') != std::string::npos) return false;
    Json patch = Json::object();
    if (owner == "effect") {
        if (!documentField(key) || !ParticleModules::documentFields().find(key)) return false;
        patch[key] = nullptr;
    } else {
        const ModuleDesc* module = ParticleModules::find(owner);
        if (!module || !module->fields.find(key) || !module->isPresent(document)) return false;
        patch["modules"][owner][key] = nullptr;
    }
    return applyPatch(document, patch);
}

bool editor::particlerows::resetModule(ParticleEffectDocument& document, ModuleId id) {
    const ModuleDesc& module = ParticleModules::get(id);
    if (!module.isPresent(document)) return false;
    const Json body = moduleBody(document, module);
    Json reset = Json::object();
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (it.key() != "enabled") reset[it.key()] = nullptr;
    }
    // the Main header holds the effect's own fields too (Duration, Looping, ...)
    Json effectFields = Json::object();
    if (id == ModuleId::Main) {
        const Json keyed = ParticleEffectCodec::toKeyedJson(document);
        for (auto it = keyed.begin(); it != keyed.end(); ++it) {
            if (documentField(it.key())) effectFields[it.key()] = nullptr;
        }
    }
    // the sub-emitter list is the module's too
    bool changed = false;
    if (id == ModuleId::SubEmitters && !document.subEmitters.empty()) {
        document.subEmitters.clear();
        changed = true;
    }
    if (reset.empty() && effectFields.empty()) return changed;
    Json patch = effectFields;
    if (!reset.empty()) patch["modules"][module.key] = reset;
    return applyPatch(document, patch) || changed;
}

std::string editor::particlerows::copyModule(const ParticleEffectDocument& document, ModuleId id) {
    const ModuleDesc& module = ParticleModules::get(id);
    Json clip = Json::object();
    clip[kClipboardKey] = module.key;
    clip["values"] = moduleBody(document, module);
    // the sub-emitter list lives beside the modules, but it is this module's
    if (id == ModuleId::SubEmitters) {
        const Json keyed = ParticleEffectCodec::toKeyedJson(document);
        clip["list"] = keyed.contains("subEmitters") ? keyed["subEmitters"] : Json::array();
    }
    return clip.dump();
}

const ModuleDesc* editor::particlerows::clipboardModule(const std::string& text) {
    if (text.empty() || text[0] != '{') return nullptr;
    const Json clip = Json::parse(text, nullptr, false);
    if (clip.is_discarded() || !clip.is_object() || !clip.contains(kClipboardKey) || !clip[kClipboardKey].is_string()) return nullptr;
    if (!clip.contains("values") || !clip["values"].is_object()) return nullptr;
    return ParticleModules::find(clip[kClipboardKey].get<std::string>());
}

bool editor::particlerows::pasteModule(ParticleEffectDocument& document, const std::string& text, bool valuesOnly, std::string* error) {
    const ModuleDesc* module = clipboardModule(text);
    if (!module) {
        if (error) *error = "the clipboard holds no particle module (Copy Module first)";
        return false;
    }
    if (valuesOnly && !module->isPresent(document)) {
        if (error) *error = std::string("Paste Values needs the ") + module->label + " module in the effect";
        return false;
    }
    Json values = Json::parse(text)["values"];
    const Json current = module->isPresent(document) ? moduleBody(document, *module) : Json::object();
    Json body = Json::object();
    // the current values not in the clipboard go back to their defaults
    for (auto it = current.begin(); it != current.end(); ++it) {
        if (valuesOnly && it.key() == "enabled") continue;
        if (!values.contains(it.key())) body[it.key()] = nullptr;
    }
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (valuesOnly && it.key() == "enabled") continue;
        body[it.key()] = *it;
    }
    Json patch = Json::object();
    patch["modules"][module->key] = body;
    const Json clip = Json::parse(text);
    if (module->id == ModuleId::SubEmitters && clip.contains("list") && clip["list"].is_array()) patch["subEmitters"] = clip["list"];
    std::vector<Diagnostic> diagnostics;
    ParticleEffectDocument patched = document;
    if (!ParticleEffectCodec::applyMergePatch(patched, patch, diagnostics)) {
        if (error) *error = ParticleEffectCodec::format(diagnostics);
        return false;
    }
    // Paste Module on an absent module adds it, as the clipboard had it
    if (!valuesOnly && !module->isPresent(patched)) module->setPresent(patched, true);
    const bool changed = ParticleEffectCodec::write(patched) != ParticleEffectCodec::write(document);
    document = patched;
    return changed;
}

bool editor::particlerows::curveOverEmitterTime(const std::string& fieldPath, CurveDriver driver) {
    if (driver == CurveDriver::EmitterTime) return true;
    // the fields the kernels read at the emitter's time (their tooltips say so), whatever their driver
    static const char* const kFields[] = {"emission.rateOverTime", "noise.scrollSpeed", "shape.radiusSpeed", "shape.arcSpeed"};
    for (const char* field : kFields) {
        if (fieldPath == field) return true;
    }
    return fieldPath.rfind("emission.bursts[", 0) == 0 && fieldPath.size() > 6 && fieldPath.compare(fieldPath.size() - 6, 6, ".count") == 0;
}

std::string editor::particlerows::moduleHelpUrl(const ModuleDesc& module) {
    // GitHub's heading anchor: lower case, spaces as hyphens, other punctuation dropped
    std::string anchor;
    for (const char* c = module.label; *c; c++) {
        const unsigned char ch = (unsigned char)*c;
        if (std::isalnum(ch)) anchor += (char)std::tolower(ch);
        else if (ch == ' ' || ch == '-') anchor += '-';
    }
    return std::string(kDocsUrl) + anchor;
}
