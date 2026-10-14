// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effect registry (phase-5 spec 3.6, 3.10): built-in manifests, project effects, the RmlUi
// registrations and the shader resolvers.

#include "EffectRegistry.h"
#include "EffectCompat.h"
#include "EffectInstancers.h"
#include "EffectManifest.h"
#include "EffectRuntime.h"
#include "EffectSyntax.h"

#include "htmlui/compat/VirtualFiles.h"
#include "pool/ShaderPool.h"

#include "Log.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace jasperengine::htmlui {

    struct JASPERENGINE_INTERNAL EffectRmlRegistrations {
        std::vector<std::unique_ptr<EffectPropertyParser>> parsers;
        std::unique_ptr<RawEffectDecoratorInstancer> rawDecorator;
        std::unique_ptr<RawEffectFilterInstancer> rawFilter;
        std::map<std::string, std::unique_ptr<EffectDecoratorInstancer>> decorators;
        std::map<std::string, std::unique_ptr<EffectFilterInstancer>> filters;
        // replaced instancers: RmlUi's style sheets may still point at them
        std::vector<std::unique_ptr<EffectDecoratorInstancer>> oldDecorators;
        std::vector<std::unique_ptr<EffectFilterInstancer>> oldFilters;
    };

}

namespace {

    const char* const kBrightPass = "@effects/_bright";

    // the RmlUi side of the registry while RmlUi runs (the registry is a singleton)
    std::unique_ptr<EffectRmlRegistrations>& rmlState(){
        static std::unique_ptr<EffectRmlRegistrations>* state = new std::unique_ptr<EffectRmlRegistrations>();
        return *state;
    }

}

EffectRegistry& EffectRegistry::get(){
    // used from the HTML UI's lifetime hooks and by worker threads (shader names): never destroyed
    static EffectRegistry* instance = new EffectRegistry();
    return *instance;
}

EffectRegistry::EffectRegistry(){
    // UI effect variants name their effect (getShaderStr): ShaderPool maps the names back
    installShaderResolvers();
}
EffectRegistry::~EffectRegistry() = default;

void EffectRegistry::ensureBuiltins() const{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (builtinsLoaded)
        return;
    builtinsLoaded = true;
    size_t count = 0;
    const BuiltinEffectSource* sources = builtinEffectSources(count);
    for (size_t i = 0; i < count; i++){
        auto manifest = std::make_unique<EffectManifest>();
        std::vector<EffectDiagnostic> diags;
        const std::string source = sources[i].manifest;
        if (EffectManifestParser::parse(source, sources[i].path, true, *manifest, diags) &&
            EffectManifestParser::validate(*manifest, source, diags)){
            builtinList.push_back(std::move(manifest));
        }
        for (EffectDiagnostic& d : diags){
            Log::error("[HTML UI] built-in effect %s:%u: %s", d.file.c_str(), d.line, d.message.c_str());
            builtinDiags.push_back(std::move(d));
        }
    }
    std::sort(builtinList.begin(), builtinList.end(), [](const auto& a, const auto& b){ return a->name < b->name; });
}

const std::vector<EffectDiagnostic>& EffectRegistry::builtinDiagnostics() const{
    ensureBuiltins();
    return builtinDiags;
}

const EffectManifest* EffectRegistry::find(std::string_view name) const{
    ensureBuiltins();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (const auto& m : builtinList){
        if (m->name == name)
            return m.get();
    }
    auto it = projects.find(std::string(name));
    return it != projects.end() ? it->second.manifest.get() : nullptr;
}

std::vector<const EffectManifest*> EffectRegistry::builtins() const{
    ensureBuiltins();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<const EffectManifest*> list;
    for (const auto& m : builtinList)
        list.push_back(m.get());
    return list;
}

std::vector<const EffectManifest*> EffectRegistry::all() const{
    std::vector<const EffectManifest*> list = builtins();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (const auto& entry : projects)
        list.push_back(entry.second.manifest.get());
    std::sort(list.begin(), list.end(), [](const EffectManifest* a, const EffectManifest* b){ return a->name < b->name; });
    return list;
}

const EffectManifest* EffectRegistry::findByCustomShader(const std::string& customShaderName) const{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (const auto& entry : projects){
        if (entry.second.fragPath == customShaderName)
            return entry.second.manifest.get();
    }
    return nullptr;
}

size_t EffectRegistry::graveyardSize() const{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return graveyard.size() + (rmlState() ? rmlState()->oldDecorators.size() + rmlState()->oldFilters.size() : 0);
}

ShaderKey EffectRegistry::shaderKey(const EffectManifest& m, bool filterForm) const{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const uint32_t generation = ShaderPool::getCustomShaderGeneration();
    if (generation != customIdGeneration){
        customIds.clear();
        customIdGeneration = generation;
    }
    const std::string custom = m.customShaderName();
    auto it = customIds.find(custom);
    if (it == customIds.end())
        it = customIds.emplace(custom, ShaderPool::registerCustomShader(custom)).first;
    return ShaderPool::getShaderKey(ShaderType::UI_EFFECT, filterForm ? 1u : 0u, it->second);
}

ShaderKey EffectRegistry::brightPassKey() const{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const uint32_t generation = ShaderPool::getCustomShaderGeneration();
    if (generation != customIdGeneration){
        customIds.clear();
        customIdGeneration = generation;
    }
    auto it = customIds.find(kBrightPass);
    if (it == customIds.end())
        it = customIds.emplace(kBrightPass, ShaderPool::registerCustomShader(kBrightPass)).first;
    return ShaderPool::getShaderKey(ShaderType::UI_EFFECT, 1u, it->second);
}

std::vector<std::string> EffectRegistry::suggestions(std::string_view unknownName) const{
    const std::string name(unknownName);
    std::vector<std::pair<size_t, std::string>> scored;
    std::set<std::string> seen;
    for (const EffectManifest* m : all()){
        // made for what the name describes (@suggest-for), first
        for (const std::string& word : m->suggestFor){
            if (word == name && seen.insert(m->name).second)
                scored.push_back({0, m->name});
        }
    }
    for (const EffectManifest* m : all()){
        const size_t distance = EffectSyntax::editDistance(name, m->name);
        if (distance <= 3 && seen.insert(m->name).second)
            scored.push_back({distance + 1, m->name});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    std::vector<std::string> list;
    for (const auto& entry : scored)
        list.push_back(entry.second);
    return list;
}

std::string EffectRegistry::counterpart(const EffectManifest& m) const{
    for (const EffectManifest* other : all()){
        if (other->kind == m.kind)
            continue;
        if (std::find(other->suggestFor.begin(), other->suggestFor.end(), m.name) != other->suggestFor.end())
            return other->name;
        if (std::find(m.suggestFor.begin(), m.suggestFor.end(), other->name) != m.suggestFor.end())
            return other->name;
    }
    return std::string();
}

bool EffectRegistry::loadProjectManifest(const std::string& name, const std::string& fragPath, const std::string& source,
                                         std::vector<EffectDiagnostic>& diags, std::unique_ptr<EffectManifest>& out) const{
    auto manifest = std::make_unique<EffectManifest>();
    const size_t before = diags.size();
    if (!EffectManifestParser::parse(source, fragPath, false, *manifest, diags))
        return false;
    EffectManifestParser::validate(*manifest, source, diags);
    if (manifest->name != name){
        EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectManifest,
                                                  "the manifest of " + fragPath + " declares '@effect " + manifest->name + "', not '" + name + "'",
                                                  "// @effect " + name);
        d.file = fragPath;
        d.line = (uint32_t)manifest->line;
        d.column = 1;
        d.length = 0;
        diags.push_back(d);
    }
    if (diags.size() != before)
        return false;
    out = std::move(manifest);
    return true;
}

bool EffectRegistry::declareProjectEffect(const std::string& name, const std::string& fragPath, const std::string& declaredIn, int line,
                                          std::vector<EffectDiagnostic>& diags, const FileReader& read){
    ensureBuiltins();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto conflict = [&](const std::string& message, const std::string& suggestion){
        EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectDeclareConflict, message, suggestion);
        diags.push_back(d);
        return false;
    };
    for (const auto& m : builtinList){
        if (m->name == name)
            return conflict("'" + name + "' is a built-in effect; @effect cannot declare it", "rename the effect");
    }
    if (isReservedEffectName(name))
        return conflict("'" + name + "' is the name of a CSS function; @effect cannot declare it", "rename the effect");
    auto existing = projects.find(name);
    if (existing != projects.end()){
        if (existing->second.fragPath == fragPath)
            return true;
        return conflict("'" + name + "' is already declared with src " + existing->second.fragPath + " (" + existing->second.declaredIn + ":" +
                        std::to_string(existing->second.line) + "); the first declaration is used", "rename the effect");
    }
    std::string source;
    const bool found = read ? read(fragPath, source) : hc::readAssetFile(hc::FileAccess(), fragPath, source);
    if (!found){
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectFileMissing, "@effect " + name + ": no file at " + fragPath,
                                             "src resolves against the stylesheet, like url()"));
        return false;
    }
    std::unique_ptr<EffectManifest> manifest;
    if (!loadProjectManifest(name, fragPath, source, diags, manifest))
        return false;
    ProjectEffect& effect = projects[name];
    effect.manifest = std::move(manifest);
    effect.fragPath = fragPath;
    effect.declaredIn = declaredIn;
    effect.line = line;
    effect.source = source;
    ShaderPool::registerCustomShader(fragPath);
    if (initialised)
        registerRml(*effect.manifest);
    return true;
}

bool EffectRegistry::onProjectFileChanged(const std::string& assetPath){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto& entry : projects){
        ProjectEffect& effect = entry.second;
        if (effect.fragPath != assetPath)
            continue;
        std::string source;
        if (!hc::readAssetFile(hc::FileAccess(), assetPath, source))
            source.clear();
        std::vector<EffectDiagnostic> diags;
        std::unique_ptr<EffectManifest> manifest;
        if (!source.empty() && loadProjectManifest(entry.first, assetPath, source, diags, manifest)){
            // the old manifest stays alive: decorators made from it may still be drawn
            graveyard.push_back(std::move(effect.manifest));
            effect.manifest = std::move(manifest);
            effect.source = source;
            if (initialised)
                registerRml(*effect.manifest);
        }else{
            for (const EffectDiagnostic& d : diags)
                Log::error("[HTML UI] %s:%u: %s", d.file.c_str(), d.line, d.message.c_str());
        }
        // the variants compile again from the new source
        const uint16_t customId = ShaderPool::registerCustomShader(assetPath);
        ShaderPool::remove(ShaderType::UI_EFFECT, 0, customId);
        ShaderPool::remove(ShaderType::UI_EFFECT, 1, customId);
        ShaderPool::destroyCustomShaders();
        return true;
    }
    return false;
}

void EffectRegistry::clearProjectEffects(){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto& entry : projects)
        graveyard.push_back(std::move(entry.second.manifest));
    projects.clear();
}

void EffectRegistry::registerRml(const EffectManifest& m){
    if (!rmlState())
        return;
    const std::string type = "jfx-" + m.name;
    if (m.kind == EffectKind::Paint){
        auto decorator = std::make_unique<EffectDecoratorInstancer>(m);
        Rml::Factory::RegisterDecoratorInstancer(type, decorator.get());
        auto& slot = rmlState()->decorators[m.name];
        if (slot)
            rmlState()->oldDecorators.push_back(std::move(slot));
        slot = std::move(decorator);
    }
    auto filter = std::make_unique<EffectFilterInstancer>(m);
    Rml::Factory::RegisterFilterInstancer(type, filter.get());
    auto& slot = rmlState()->filters[m.name];
    if (slot)
        rmlState()->oldFilters.push_back(std::move(slot));
    slot = std::move(filter);
}

void EffectRegistry::installShaderResolvers(){
    // a project effect's variants carry its .frag, which their basenames do not say back
    ShaderPool::setCustomShaderNameResolver([](const std::string& basename) -> std::string {
        EffectRegistry& registry = EffectRegistry::get();
        std::lock_guard<std::recursive_mutex> lock(registry.mutex);
        for (const auto& entry : registry.projects){
            std::string sanitized = entry.second.fragPath;
            for (char& c : sanitized){
                if (!(std::isalnum((unsigned char)c) || c == '_'))
                    c = '_';
            }
            if (sanitized == basename)
                return entry.second.fragPath;
        }
        return std::string();
    });
    ShaderPool::setUiEffectResolver([](const std::string& effectName) -> std::string {
        const EffectManifest* m = EffectRegistry::get().find(effectName);
        return m ? m->customShaderName() : std::string();
    });
}

void EffectRegistry::initialise(){
    ensureBuiltins();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    installShaderResolvers();
    registerEffectCompatHandlers();
    if (initialised)
        return;
    initialised = true;
    rmlState() = std::make_unique<EffectRmlRegistrations>();

    // the slots' parsers, before any instancer registers a property with them
    const std::pair<EffectParamType, char> parsers[] = {
        {EffectParamType::Number, 0}, {EffectParamType::Integer, 0}, {EffectParamType::Percentage, 0}, {EffectParamType::Length, 0},
        {EffectParamType::Angle, 0}, {EffectParamType::Time, 0}, {EffectParamType::Color, 0}, {EffectParamType::Position, 'x'},
        {EffectParamType::Position, 'y'}, {EffectParamType::Enum, 0}, {EffectParamType::Boolean, 0}, {EffectParamType::Palette, 0}};
    for (const auto& entry : parsers){
        auto parser = std::make_unique<EffectPropertyParser>(entry.first, entry.second);
        Rml::StyleSheetSpecification::RegisterParser(EffectPropertyParser::parserName(entry.first, entry.second), parser.get());
        rmlState()->parsers.push_back(std::move(parser));
    }
    rmlState()->rawDecorator = std::make_unique<RawEffectDecoratorInstancer>();
    rmlState()->rawFilter = std::make_unique<RawEffectFilterInstancer>();
    Rml::Factory::RegisterDecoratorInstancer("effect", rmlState()->rawDecorator.get());
    Rml::Factory::RegisterFilterInstancer("effect", rmlState()->rawFilter.get());
    for (const auto& m : builtinList)
        registerRml(*m);
    for (const auto& entry : projects)
        registerRml(*entry.second.manifest);
    Rml::RegisterPlugin(&EffectRuntime::get());
}

void EffectRegistry::shutdown(){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialised)
        return;
    initialised = false;
    // RmlUi is gone (Rml::Shutdown ran first): nothing refers to the instancers any more
    rmlState().reset();
    graveyard.clear();
}

bool EffectRegistry::isInitialised() const{
    return initialised;
}
