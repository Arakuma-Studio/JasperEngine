// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimPresets.h"

#include "AppSettings.h"
#include "Log.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "animation/AnimationClipIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "TitleAnimOps.h"
#include "titleart/fx/FxPresets.h"
#include "util/NumericExpression.h"

#include "title_anim_presets.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    std::string projectDirectory;
    std::string userDirectoryOverride;

    struct Library {
        std::vector<TitleAnimPreset> presets;
        bool loaded = false;
    };

    Library& library(){
        static Library l;
        return l;
    }

    void loadFolder(const std::string& dir, const std::string& scope, std::vector<TitleAnimPreset>& out){
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        std::vector<fs::path> files;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files){
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            TitleAnimPreset p;
            std::string error;
            if (!TitleAnimPresets::parse(text.str(), p, error)){
                Log::warn("Animation Preset %s: %s", file.string().c_str(), error.c_str());
                continue;
            }
            p.scope = scope;
            p.file = file.string();
            auto same = std::find_if(out.begin(), out.end(), [&](const TitleAnimPreset& x){ return x.id == p.id; });
            if (same != out.end()) *same = std::move(p);
            else out.push_back(std::move(p));
        }
    }

    void loadAll(){
        Library& lib = library();
        lib.presets.clear();
        for (const TitleAnimPresetEmbeddedFile* f = title_anim_preset_embedded_files; f->name; f++){
            TitleAnimPreset p;
            std::string error;
            const std::string text(reinterpret_cast<const char*>(f->data), f->size);
            if (!TitleAnimPresets::parse(text, p, error)){
                Log::error("the built-in Animation Preset %s does not load: %s", f->name, error.c_str());
                continue;
            }
            p.file = std::string("builtin:") + f->name;
            lib.presets.push_back(std::move(p));
        }
        loadFolder(TitleAnimPresets::folderOf("project"), "project", lib.presets);
        loadFolder(TitleAnimPresets::folderOf("user"), "user", lib.presets);
        lib.loaded = true;
    }

    // ---- evaluation (D13) ----

    struct Eval {
        std::vector<std::pair<std::string, double>> vars;
        std::map<std::string, TitleJson> raw;      // "$name": a parameter as written
        std::string error;

        void set(const std::string& name, double value){
            for (auto& v : vars) if (v.first == name){ v.second = value; return; }
            vars.push_back({name, value});
        }
        double get(const std::string& name) const{
            for (const auto& v : vars) if (v.first == name) return v.second;
            return 0.0;
        }
        bool number(const TitleJson& v, double& out){
            if (v.is_number()){
                out = v.get<double>();
                return true;
            }
            if (v.is_boolean()){
                out = v.get<bool>() ? 1.0 : 0.0;
                return true;
            }
            if (v.is_string()){
                const std::string text = v.get<std::string>();
                if (!text.empty() && text[0] == '$'){
                    auto it = raw.find(text.substr(1));
                    if (it != raw.end() && it->second.is_number()){
                        out = it->second.get<double>();
                        return true;
                    }
                    error = "'" + text + "' is not a number";
                    return false;
                }
                const NumericResult r = NumericExpression::evaluateWith(text, vars);
                if (!r.ok){
                    error = "'" + text + "': " + r.error;
                    return false;
                }
                out = r.value;
                return true;
            }
            error = "expected a number or an expression";
            return false;
        }
        // a value: numbers and expressions evaluated, "$p" its parameter's value, "#rrggbb" kept
        bool value(const TitleJson& v, TitleJson& out){
            if (v.is_array()){
                out = TitleJson::array();
                for (const TitleJson& e : v){
                    TitleJson x;
                    if (!value(e, x)) return false;
                    out.push_back(x);
                }
                return true;
            }
            if (v.is_string()){
                const std::string text = v.get<std::string>();
                if (!text.empty() && text[0] == '#'){
                    out = v;
                    return true;
                }
                if (!text.empty() && text[0] == '$' && text.find_first_of("+-*/() ") == std::string::npos){
                    auto it = raw.find(text.substr(1));
                    if (it == raw.end()){
                        error = "no parameter '" + text.substr(1) + "'";
                        return false;
                    }
                    out = it->second;
                    return true;
                }
            }
            if (v.is_object() || v.is_null()){
                out = v;
                return true;
            }
            double d = 0.0;
            if (!number(v, d)) return false;
            out = ta::roundToFloat(d);
            return true;
        }
        bool truthy(const TitleJson& when, bool& out){
            if (when.is_null()){
                out = true;
                return true;
            }
            double d = 0.0;
            if (!number(when, d)) return false;
            out = d > 0.5;
            return true;
        }
        // "${p}" in a field path: the parameter's text
        std::string substitute(const std::string& text) const{
            std::string out = text;
            size_t at = 0;
            while ((at = out.find("${", at)) != std::string::npos){
                const size_t end = out.find('}', at);
                if (end == std::string::npos) break;
                const std::string name = out.substr(at + 2, end - at - 2);
                auto it = raw.find(name);
                const std::string with = it != raw.end() && it->second.is_string() ? it->second.get<std::string>() : std::string();
                out.replace(at, end - at + 1, with);
                at += with.size();
            }
            return out;
        }
    };

    std::vector<std::string> textLayersOf(const tt::TitleArtDoc& doc){
        std::vector<std::string> out;
        tt::forEachLayer(doc.root, [&](const TitleJson& l, const TitleJson*, int){
            if (tt::layerTypeOf(l) == tt::LayerType::Text) out.push_back(l.value("id", std::string()));
            return true;
        });
        return out;
    }

    TitleJson presetMarkerTag(const std::string& id){
        return TitleJson{{"from_preset", id}};
    }

    bool fromPresetMarker(const ta::TitleMarker& m, const std::string& id){
        if (m.unknown.empty()) return false;
        const TitleJson u = TitleJson::parse(m.unknown, nullptr, false);
        return u.is_object() && u.value("from_preset", std::string()) == id;
    }

    // ---- key generators (D13) ----

    void shakeKeys(Eval& e, const TitleJson& g, int width, uint64_t docSeed, double at, std::vector<ta::TitleKey>& keys, bool& ok){
        double start = 0, length = 0.25, frequency = 25, amplitude = 10, decay = 1, seed = 1;
        ok = (!g.contains("start") || e.number(g["start"], start)) && (!g.contains("length") || e.number(g["length"], length)) &&
             (!g.contains("frequency") || e.number(g["frequency"], frequency)) && (!g.contains("amplitude") || e.number(g["amplitude"], amplitude)) &&
             (!g.contains("decay") || e.number(g["decay"], decay)) && (!g.contains("seed") || e.number(g["seed"], seed));
        if (!ok) return;
        frequency = std::max(1.0, frequency);
        length = std::max(0.01, length);
        const int n = std::max(2, (int)std::ceil(length * frequency));
        ta::SplitMix64 random(ta::mixSeeds(docSeed, (uint64_t)std::llround(seed) * 0x9E37u + 85u));
        for (int k = 0; k <= n; k++){
            const double u = (double)k / (double)n;
            ta::TitleKey key;
            key.key.time = (float)(at + start + length * u);
            key.key.ease = EaseType::SINE_IN_OUT;
            const double envelope = amplitude * std::pow(std::max(0.0, 1.0 - u), std::max(0.0, decay));
            for (int c = 0; c < width; c++){
                const double r = random.unit() * 2.0 - 1.0;
                key.key.value.push_back(k == 0 || k == n ? 0.0f : (float)(r * envelope));
            }
            keys.push_back(std::move(key));
        }
    }

    void flickerKeys(Eval& e, const TitleJson& g, uint64_t docSeed, double at, double duration, std::vector<ta::TitleKey>& keys, bool& ok){
        double count = 4, start = 0, length = duration, low = 0, high = 1, seed = 1;
        ok = (!g.contains("count") || e.number(g["count"], count)) && (!g.contains("start") || e.number(g["start"], start)) &&
             (!g.contains("length") || e.number(g["length"], length)) && (!g.contains("low") || e.number(g["low"], low)) &&
             (!g.contains("high") || e.number(g["high"], high)) && (!g.contains("seed") || e.number(g["seed"], seed));
        if (!ok) return;
        const int drops = std::max(0, std::min(64, (int)std::llround(count)));
        ta::SplitMix64 random(ta::mixSeeds(docSeed, (uint64_t)std::llround(seed) * 0x51EDu + 7u));
        // on at random times in the first 85 % of the length, each on stretch ended by a drop, then on for good
        std::vector<double> times;
        for (int i = 0; i < drops * 2 + 1; i++) times.push_back(random.unit() * 0.85 * std::max(0.01, length));
        std::sort(times.begin(), times.end());
        auto put = [&](double t, double v){
            ta::TitleKey k;
            k.key.time = (float)(at + start + t);
            k.key.value = {(float)v};
            k.key.ease = EaseType::STEP;
            keys.push_back(std::move(k));
        };
        put(0.0, low);
        for (size_t i = 0; i < times.size(); i++) put(times[i] + 1e-3 * (double)i, i % 2 == 0 ? high : low);
        put(0.9 * std::max(0.01, length), high);
    }

}

// ==================================================================== the library

const std::vector<TitleAnimPreset>& TitleAnimPresets::all(){
    if (!library().loaded) loadAll();
    return library().presets;
}

const TitleAnimPreset* TitleAnimPresets::find(const std::string& id){
    for (const TitleAnimPreset& p : all()) if (p.id == id) return &p;
    return nullptr;
}

std::vector<std::string> TitleAnimPresets::ids(){
    std::vector<std::string> out;
    for (const TitleAnimPreset& p : all()) out.push_back(p.id);
    return out;
}

void TitleAnimPresets::reload(){
    library().loaded = false;
}

void TitleAnimPresets::setProjectDir(const std::string& dir){
    if (dir == projectDirectory) return;
    projectDirectory = dir;
    reload();
}

void TitleAnimPresets::setUserDirForTesting(const std::string& dir){
    userDirectoryOverride = dir;
    reload();
}

std::string TitleAnimPresets::folderOf(const std::string& scope){
    if (scope == "project") return projectDirectory.empty() ? std::string() : (fs::path(projectDirectory) / "EditorSettings" / "TitleLibrary" / "AnimationPresets").string();
    if (!userDirectoryOverride.empty()) return userDirectoryOverride;
    return (AppSettings::getConfigDirectory() / "title-library" / "animation-presets").string();
}

bool TitleAnimPresets::parse(const std::string& text, TitleAnimPreset& out, std::string& error){
    out = TitleAnimPreset();
    const TitleJson root = TitleJson::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        error = "not a JSON object";
        return false;
    }
    if (root.value("format", std::string()) != TITLE_ANIM_PRESET_FORMAT){
        error = std::string("format is not ") + TITLE_ANIM_PRESET_FORMAT;
        return false;
    }
    if (root.value("version", 0) > TITLE_ANIM_PRESET_VERSION){
        error = "needs a newer JasperEngine (version " + std::to_string(root.value("version", 0)) + ")";
        return false;
    }
    out.id = root.value("id", std::string());
    out.name = root.value("name", out.id);
    out.group = root.value("group", std::string("in"));
    out.role = root.value("role", std::string("custom"));
    out.description = root.value("description", std::string());
    out.wrap = root.value("wrap", std::string("once"));
    out.overlay = root.value("overlay", false);
    if (out.id.empty() || out.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos){
        error = "id '" + out.id + "' is not snake_case";
        return false;
    }
    if (out.group != "in" && out.group != "emphasis" && out.group != "loop" && out.group != "out" && out.group != "custom"){
        error = "group is in, emphasis, loop, out or custom";
        return false;
    }
    const TitleJson duration = root.value("duration", TitleJson(1.0));
    out.defaultDuration = duration.is_number() ? duration.get<double>() : -2.0;    // -2: an expression
    for (const TitleJson& p : root.value("params", TitleJson::array())){
        TitleAnimPresetParam param;
        param.name = p.value("name", std::string());
        param.label = p.value("label", param.name);
        param.type = p.value("type", std::string("number"));
        param.defaultValue = p.value("default", TitleJson());
        param.min = p.value("min", 0.0);
        param.max = p.value("max", 0.0);
        for (const TitleJson& o : p.value("options", TitleJson::array())) if (o.is_string()) param.options.push_back(o.get<std::string>());
        if (param.name.empty() || (param.type != "number" && param.type != "bool" && param.type != "enum" && param.type != "color" && param.type != "sound")){
            error = "param '" + param.name + "': type is number, bool, enum, color or sound";
            return false;
        }
        out.params.push_back(std::move(param));
    }
    out.data = root;
    return true;
}

std::string editor::mainTextLayerOf(const tt::TitleArtDoc& doc){
    for (const TitleJson& e : doc.root.value("exposed", TitleJson::array())){
        if (e.value("name", std::string()) != "text") continue;
        for (const TitleJson& t : e.value("targets", TitleJson::array())){
            const std::string id = t.value("layer", std::string());
            const TitleJson* layer = tt::findLayer(doc.root, id);
            if (layer && tt::layerTypeOf(*layer) == tt::LayerType::Text) return id;
        }
    }
    // the topmost text layer: the last in draw order
    const std::vector<std::string> texts = textLayersOf(doc);
    return texts.empty() ? std::string() : texts.back();
}

// ==================================================================== apply (R22)

bool editor::applyTitleAnimationPresetOp(TitleOpContext& ctx, const TitleJson& op){
    std::string presetId;
    if (!ctx.requireString(op, "preset", presetId)) return false;
    const TitleAnimPreset* preset = TitleAnimPresets::find(presetId);
    if (!preset){
        const std::vector<std::string> ids = TitleAnimPresets::ids();
        return ctx.fail("unknown_preset", "no Animation Preset '" + presetId + "'", "preset", ids, tt::nearestName(presetId, ids));
    }
    std::string animationName;
    if (!ctx.requireString(op, "animation", animationName)) return false;
    ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
    const int animationIndex = findTitleAnimation(set, animationName);
    if (animationIndex < 0){
        std::vector<std::string> names;
        for (const ta::TitleAnimation& a : set.animations) names.push_back(a.name);
        return ctx.fail("unknown_animation", "no animation '" + animationName + "' (add_animation {name, preset} makes one)", "animation", names,
                        tt::nearestName(animationName, names));
    }
    const std::string mode = op.value("mode", std::string("merge"));
    if (mode != "merge" && mode != "replace") return ctx.fail("bad_value", "mode is merge or replace", "mode", {"merge", "replace"});
    double at = 0.0;
    if (op.contains("at")){
        if (!op["at"].is_number() || op["at"].get<double>() < 0.0) return ctx.fail("bad_value", "`at` is seconds >= 0", "at");
        at = op["at"].get<double>();
    }

    // parameters: the defaults, then the op's (checked)
    Eval e;
    TitleJson params = TitleJson::object();
    std::vector<std::string> paramNames;
    for (const TitleAnimPresetParam& p : preset->params) paramNames.push_back(p.name);
    if (op.contains("params")){
        if (!op["params"].is_object()) return ctx.fail("bad_value", "`params` is {name: value}", "params");
        for (auto it = op["params"].begin(); it != op["params"].end(); ++it){
            if (it.key() == "duration") continue;
            if (std::find(paramNames.begin(), paramNames.end(), it.key()) == paramNames.end())
                return ctx.fail("bad_path", preset->name + " has no parameter '" + it.key() + "'", "params", paramNames, tt::nearestName(it.key(), paramNames));
        }
    }
    const TitleJson given = op.value("params", TitleJson::object());
    for (const TitleAnimPresetParam& p : preset->params){
        TitleJson v = given.contains(p.name) ? given[p.name] : p.defaultValue;
        if (p.type == "number"){
            if (!v.is_number()) return ctx.fail("bad_value", p.name + " is a number", "params");
            double x = v.get<double>();
            if (p.max > p.min) x = std::min(p.max, std::max(p.min, x));
            v = x;
            e.set(p.name, x);
        }else if (p.type == "bool"){
            if (!v.is_boolean()) return ctx.fail("bad_value", p.name + " is true or false", "params");
            e.set(p.name, v.get<bool>() ? 1.0 : 0.0);
        }else if (p.type == "enum"){
            if (!v.is_string() || std::find(p.options.begin(), p.options.end(), v.get<std::string>()) == p.options.end())
                return ctx.fail("bad_value", p.name + " is one of its options", "params", p.options);
            for (const std::string& o : p.options) e.set(p.name + "_" + o, v.get<std::string>() == o ? 1.0 : 0.0);
        }else if (p.type == "color"){
            float rgb[3];
            if (!v.is_string() || !ta::parseHexColor(v.get<std::string>(), rgb)) return ctx.fail("bad_value", p.name + " is \"#rrggbb\"", "params");
        }else if (p.type == "sound"){
            if (!v.is_null() && !v.is_object() && !v.is_string()) return ctx.fail("bad_value", p.name + " is a sound asset ({path, guid}) or null", "params");
            if (v.is_string()) v = TitleJson{{"path", v.get<std::string>()}, {"guid", ""}};
            e.set(p.name, v.is_null() ? 0.0 : 1.0);
        }
        e.raw[p.name] = v;
        params[p.name] = v;
    }
    // the variables: the canvas, the main text, the duration
    const std::string mainText = mainTextLayerOf(ctx.doc);
    const TitleJson* mainLayer = mainText.empty() ? nullptr : tt::findLayer(ctx.doc.root, mainText);
    e.set("W", ctx.doc.canvasWidth());
    e.set("H", ctx.doc.canvasHeight());
    e.set("S", mainLayer ? mainLayer->value("size", 200.0) : 200.0);
    e.set("n", mainLayer ? (double)tt::glyphUnitsOf(*mainLayer, ta::StaggerUnit::Glyph, nullptr).count : 0.0);
    e.set("fps", set.fps);
    e.set("is3d", mainLayer && mainLayer->value("dimension", std::string("2d")) == "3d" ? 1.0 : 0.0);
    double duration = preset->defaultDuration;
    if (given.contains("duration") && given["duration"].is_number()) duration = given["duration"].get<double>();
    else if (duration == -2.0 && !e.number(preset->data["duration"], duration)) return ctx.fail("bad_preset", preset->id + ": duration: " + e.error, "preset");
    const bool automatic = duration < 0.0;
    e.set("D", automatic ? 1.0 : std::max(ta::TITLE_ANIM_MIN_DURATION, duration));
    for (auto it = preset->data.value("vars", TitleJson::object()).begin(); it != preset->data.value("vars", TitleJson::object()).end(); ++it){
        double v = 0.0;
        if (!e.number(it.value(), v)) return ctx.fail("bad_preset", preset->id + ": vars." + it.key() + ": " + e.error, "preset");
        e.set(it.key(), v);
    }
    const double D = e.get("D");
    const uint64_t docSeed = (uint64_t)ctx.doc.root.value("seed", 1);

    // the parts it creates when missing (S83 effects, masks and generator layers)
    std::map<std::string, std::pair<std::string, std::string>> added;     // add id -> (layer id, field prefix "mask#i" / "fx:type")
    std::set<std::string> unavailable;
    for (const TitleJson& add : preset->data.value("adds", TitleJson::array())){
        const std::string id = add.value("id", std::string());
        bool when = true;
        if (!e.truthy(add.value("when", TitleJson()), when)) return ctx.fail("bad_preset", preset->id + ": adds." + id + ": " + e.error, "preset");
        if (!when) continue;
        const std::string kind = add.value("kind", std::string("effect"));
        const std::string type = add.value("type", std::string());
        if (kind == "effect"){
            if (!tt::findEffect(type) || !mainLayer){
                unavailable.insert(id);
                ctx.note("preset_part_unavailable", preset->name + ": " + (mainLayer ? "the effect '" + type + "' is not in this build" : std::string("no text layer")) +
                                                        ": that part is left out");
                continue;
            }
            TitleJson* layer = tt::findLayer(ctx.doc.root, mainText);
            bool present = false;
            for (const TitleJson& fx : layer->value("effects", TitleJson::array())) present = present || fx.value("type", std::string()) == type;
            if (!present){
                TitleJson addOp = {{"op", "add_effect"}, {"layer", "id:" + mainText}, {"type", type}};
                // an S83 preset of the effect, when this build has it
                if (add.contains("preset") && add["preset"].is_string() && TitleFxPresets::find(type, add["preset"].get<std::string>()))
                    addOp["preset"] = add["preset"];
                if (add.contains("params")){
                    TitleJson p = TitleJson::object();
                    for (auto it = add["params"].begin(); it != add["params"].end(); ++it){
                        TitleJson v;
                        if (!e.value(it.value(), v)) return ctx.fail("bad_preset", preset->id + ": adds." + id + ".params." + it.key() + ": " + e.error, "preset");
                        p[it.key()] = v;
                    }
                    addOp["params"] = p;
                }
                if (!TitleArtOps::runCoreOp(ctx, addOp)) return false;
            }
            added[id] = {mainText, "fx:" + type};
        }else if (kind == "mask"){
            if (!mainLayer){
                unavailable.insert(id);
                ctx.note("preset_part_unavailable", preset->name + ": no text layer: the mask is left out");
                continue;
            }
            TitleJson* layer = tt::findLayer(ctx.doc.root, mainText);
            const size_t index = layer->value("masks", TitleJson::array()).size();
            TitleJson addOp = {{"op", "add_mask"}, {"layer", "id:" + mainText}, {"type", type.empty() ? std::string("gradient") : type}};
            if (add.contains("values")){
                TitleJson v = TitleJson::object();
                for (auto it = add["values"].begin(); it != add["values"].end(); ++it){
                    TitleJson x;
                    if (!e.value(it.value(), x)) return ctx.fail("bad_preset", preset->id + ": adds." + id + ".values." + it.key() + ": " + e.error, "preset");
                    v[it.key()] = x;
                }
                addOp["values"] = v;
            }
            // a mask this preset added before is reused (Edit Preset Parameters)
            bool reuse = false;
            const TitleJson& masks = layer->value("masks", TitleJson::array());
            for (size_t m = 0; m < masks.size(); m++){
                if (masks[m].value("name", std::string()) == preset->id){
                    added[id] = {mainText, "mask#" + std::to_string(m)};
                    reuse = true;
                }
            }
            if (reuse) continue;
            if (!TitleArtOps::runCoreOp(ctx, addOp)) return false;
            TitleJson* l = tt::findLayer(ctx.doc.root, mainText);
            TitleJson& made = (*l)["masks"][index];
            made["name"] = preset->id;
            added[id] = {mainText, "mask#" + std::to_string(index)};
        }else if (kind == "generator"){
            if (!tt::findGenerator(type)){
                unavailable.insert(id);
                ctx.note("preset_part_unavailable", preset->name + ": the generator '" + type + "' is not in this build: that part is left out");
                continue;
            }
            std::string existing;
            tt::forEachLayer(ctx.doc.root, [&](const TitleJson& l, const TitleJson*, int){
                if (existing.empty() && tt::layerTypeOf(l) == tt::LayerType::Generator && l.value("generator", TitleJson::object()).value("type", std::string()) == type)
                    existing = l.value("id", std::string());
                return existing.empty();
            });
            if (existing.empty()){
                TitleJson addOp = {{"op", "add_layer"}, {"type", "generator"}, {"generator", type}, {"name", add.value("name", type)}};
                if (mainLayer){
                    addOp[add.value("place", std::string("below")) == "above" ? "after" : "before"] = "id:" + mainText;
                }
                if (add.contains("values")){
                    TitleJson v = TitleJson::object();
                    for (auto it = add["values"].begin(); it != add["values"].end(); ++it){
                        TitleJson x;
                        if (!e.value(it.value(), x)) return ctx.fail("bad_preset", preset->id + ": adds." + id + ".values." + it.key() + ": " + e.error, "preset");
                        v[it.key()] = x;
                    }
                    addOp["values"] = v;
                }
                if (!TitleArtOps::runCoreOp(ctx, addOp)) return false;
                // the one generator layer of that type now
                tt::forEachLayer(ctx.doc.root, [&](const TitleJson& l, const TitleJson*, int){
                    if (tt::layerTypeOf(l) == tt::LayerType::Generator && l.value("generator", TitleJson::object()).value("type", std::string()) == type)
                        existing = l.value("id", std::string());
                    return true;
                });
            }
            added[id] = {existing, ""};
        }
    }
    // the set again: the adds changed the layers
    set = tt::readAnimationSet(ctx.doc);
    ta::TitleAnimation& a = set.animations[(size_t)animationIndex];

    // the targets of each track
    std::vector<std::string> callerLayers;
    if (op.contains("layers")){
        if (!op["layers"].is_array()) return ctx.fail("bad_value", "`layers` is an array of layer paths", "layers");
        for (const TitleJson& ref : op["layers"]){
            if (!ref.is_string()) return ctx.fail("bad_value", "`layers` is an array of layer paths", "layers");
            TitleJson* l = ctx.resolveLayer(ref.get<std::string>(), "layers");
            if (!l) return false;
            callerLayers.push_back(l->value("id", std::string()));
        }
    }
    auto targetsOf = [&](const std::string& target, std::string& prefix) -> std::vector<std::string>{
        prefix.clear();
        if (target == "title") return {""};
        if (target == "main_text") return mainText.empty() ? std::vector<std::string>() : std::vector<std::string>{mainText};
        if (target == "text_layers") return textLayersOf(ctx.doc);
        if (target == "layers") return callerLayers.empty() ? (mainText.empty() ? std::vector<std::string>() : std::vector<std::string>{mainText}) : callerLayers;
        if (target.rfind("add:", 0) == 0){
            auto it = added.find(target.substr(4));
            if (it == added.end()) return {};
            prefix = it->second.second;
            return {it->second.first};
        }
        if (target.rfind("generator:", 0) == 0){
            std::vector<std::string> out;
            const std::string type = target.substr(10);
            tt::forEachLayer(ctx.doc.root, [&](const TitleJson& l, const TitleJson*, int){
                if (tt::layerTypeOf(l) == tt::LayerType::Generator && l.value("generator", TitleJson::object()).value("type", std::string()) == type)
                    out.push_back(l.value("id", std::string()));
                return true;
            });
            return out;
        }
        return {};
    };

    // Edit Preset Parameters (and a second apply): this preset's tracks and markers go first;
    // replace also clears every track on its targets
    a.tracks.erase(std::remove_if(a.tracks.begin(), a.tracks.end(), [&](const ta::TitleTrack& t){ return t.fromPreset == preset->id; }), a.tracks.end());
    a.markers.erase(std::remove_if(a.markers.begin(), a.markers.end(), [&](const ta::TitleMarker& m){ return fromPresetMarker(m, preset->id); }), a.markers.end());
    if (mode == "replace"){
        std::set<std::string> targets;
        for (const TitleJson& spec : preset->data.value("tracks", TitleJson::array())){
            std::string prefix;
            for (const std::string& l : targetsOf(spec.value("target", std::string("title")), prefix)) targets.insert(l);
        }
        a.tracks.erase(std::remove_if(a.tracks.begin(), a.tracks.end(), [&](const ta::TitleTrack& t){ return targets.count(t.layer) > 0; }), a.tracks.end());
    }

    // the tracks
    int written = 0;
    for (const TitleJson& spec : preset->data.value("tracks", TitleJson::array())){
        bool when = true;
        if (!e.truthy(spec.value("when", TitleJson()), when)) return ctx.fail("bad_preset", preset->id + ": a track's when: " + e.error, "preset");
        if (!when) continue;
        const std::string target = spec.value("target", std::string("title"));
        if (target.rfind("add:", 0) == 0 && unavailable.count(target.substr(4))) continue;
        std::string prefix;
        const std::vector<std::string> layers = targetsOf(target, prefix);
        if (layers.empty()){
            if (target != "title" && target.rfind("generator:", 0) != 0)
                ctx.note("preset_part_unavailable", preset->name + ": no " + target + " for " + spec.value("field", std::string()) + ": that part is left out");
            continue;
        }
        std::string field = e.substitute(spec.value("field", std::string()));
        if (!prefix.empty() && !field.empty() && field[0] == '@') field = prefix + field.substr(1);
        for (const std::string& layerId : layers){
            const tt::TrackFieldInfo info = tt::resolveTrackField(ctx.doc, layerId, field);
            if (!info.ok){
                ctx.note("preset_part_unavailable", preset->name + ": " + field + ": " + info.error + ": that part is left out");
                continue;
            }
            ta::TitleTrack t;
            t.layer = layerId;
            t.field = info.longPath;
            t.fromPreset = preset->id;
            t.relative = spec.value("relative", false) && !info.stepped;
            if (spec.contains("pivot")){
                ta::TrackPivot p;
                if (ta::trackPivotFromName(spec["pivot"].get<std::string>(), p)) t.pivot = p;
            }else if (info.target == ta::TrackTarget::Glyph){
                t.pivot = ta::TrackPivot::Center;
            }
            if (spec.contains("stagger") && (info.target == ta::TrackTarget::Glyph || info.target == ta::TrackTarget::Child)){
                const TitleJson& s = spec["stagger"];
                ta::TitleStagger st;
                double x = 0.0;
                if (s.contains("each")){
                    if (!e.number(s["each"], x)) return ctx.fail("bad_preset", preset->id + ": stagger.each: " + e.error, "preset");
                    st.each = (float)std::min(ta::TITLE_ANIM_MAX_STAGGER_EACH, std::max(0.0, x));
                }
                if (s.contains("over")){
                    if (!e.number(s["over"], x)) return ctx.fail("bad_preset", preset->id + ": stagger.over: " + e.error, "preset");
                    st.over = (float)std::min(ta::TITLE_ANIM_MAX_STAGGER_OVER, std::max(0.0, x));
                }
                TitleJson order, unit;
                if (s.contains("order") && e.value(s["order"], order) && order.is_string()) ta::staggerOrderFromName(order.get<std::string>(), st.order);
                if (s.contains("unit") && e.value(s["unit"], unit) && unit.is_string()) ta::staggerUnitFromName(unit.get<std::string>(), st.unit);
                if (info.target == ta::TrackTarget::Child) st.unit = ta::StaggerUnit::Child;
                if (s.contains("seed") && e.number(s["seed"], x)) st.seed = (uint32_t)std::max(0.0, x);
                if (s.contains("ease") && s["ease"].is_string()) anim::easeFromName(s["ease"].get<std::string>(), st.ease);
                t.stagger = st;
            }
            if (spec.contains("drive")){
                const TitleJson& d = spec["drive"];
                TitleJson resolved = TitleJson::object();
                for (auto it = d.begin(); it != d.end(); ++it){
                    TitleJson v;
                    if (it.key() == "ease" || it.key() == "wrap") v = it.value();
                    else if (!e.value(it.value(), v)) return ctx.fail("bad_preset", preset->id + ": drive." + it.key() + ": " + e.error, "preset");
                    resolved[it.key()] = v;
                }
                ta::TitleDrive drive;
                std::vector<ta::TitleAnimWarning> w;
                if (!ta::decodeDrive(resolved, "drive", drive, &w)) return ctx.fail("bad_preset", preset->id + ": " + (w.empty() ? "a bad drive" : w[0].toString()), "preset");
                if ((int)drive.from.size() == 1 && info.width > 1){
                    drive.from.assign((size_t)info.width, drive.from[0]);
                    drive.to.assign((size_t)info.width, drive.to[0]);
                }
                t.drive = drive;
            }else if (spec.contains("reseed")){
                double rate = 12.0, offset = 0.0;
                const TitleJson& r = spec["reseed"];
                if ((r.contains("rate") && !e.number(r["rate"], rate)) || (r.contains("offset") && !e.number(r["offset"], offset)))
                    return ctx.fail("bad_preset", preset->id + ": reseed: " + e.error, "preset");
                t.reseed = ta::TitleReseed{(float)rate, (float)offset};
            }else if (spec.contains("generate")){
                const TitleJson& g = spec["generate"];
                const std::string kind = g.value("kind", std::string());
                bool ok = true;
                if (kind == "shake") shakeKeys(e, g, info.width, docSeed, at, t.keys, ok);
                else if (kind == "flicker") flickerKeys(e, g, docSeed, at, D, t.keys, ok);
                else return ctx.fail("bad_preset", preset->id + ": key generators are shake and flicker (beats and ticks make markers)", "preset");
                if (!ok) return ctx.fail("bad_preset", preset->id + ": generate: " + e.error, "preset");
            }else{
                // keys, repeated `repeat.count` times every `repeat.period`
                double count = 1.0, period = 0.0;
                if (spec.contains("repeat")){
                    const TitleJson& r = spec["repeat"];
                    if (!e.number(r.value("count", TitleJson(1)), count) || !e.number(r.value("period", TitleJson(0)), period))
                        return ctx.fail("bad_preset", preset->id + ": repeat: " + e.error, "preset");
                }
                const int repeats = std::max(1, std::min(256, (int)std::floor(count + 1e-6)));
                for (int k = 0; k < repeats; k++){
                    for (const TitleJson& key : spec.value("keys", TitleJson::array())){
                        if (!key.is_array() || key.size() < 2) return ctx.fail("bad_preset", preset->id + ": a key is [time, value] or [time, value, ease]", "preset");
                        double time = 0.0;
                        TitleJson value;
                        if (!e.number(key[0], time) || !e.value(key[1], value))
                            return ctx.fail("bad_preset", preset->id + ": " + spec.value("field", std::string()) + ": " + e.error, "preset");
                        ta::TitleKey tk;
                        std::string error;
                        if (!titleKeyFromValue(value, info.width, info.stepped, info.value == ta::TrackValue::Color, tk, error))
                            return ctx.fail("bad_preset", preset->id + ": " + field + ": " + error, "preset");
                        tk.key.time = (float)std::max(0.0, at + time + period * k);
                        if (key.size() > 2 && key[2].is_string()) anim::easeFromName(key[2].get<std::string>(), tk.key.ease);
                        // a repeat's closing key is the next repeat's opening one
                        if (k > 0 && !t.keys.empty() && std::fabs(t.keys.back().key.time - tk.key.time) < 1e-5f) t.keys.pop_back();
                        t.keys.push_back(std::move(tk));
                    }
                }
            }
            if (!t.drive && !t.reseed && t.keys.empty()) continue;
            std::stable_sort(t.keys.begin(), t.keys.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
            // merge: the preset's track replaces one on the same field
            const int same = findTitleTrack(a, t.layer, t.field);
            if (same >= 0) a.tracks.erase(a.tracks.begin() + same);
            const int conflict = conflictingTitleTrack(a, t.layer, t.field);
            if (conflict >= 0) a.tracks.erase(a.tracks.begin() + conflict);
            a.tracks.push_back(std::move(t));
            written++;
        }
    }

    // the markers (and the beats and ticks generators)
    auto addMarker = [&](double time, const std::string& name, const TitleJson& sound){
        ta::TitleMarker m;
        m.time = (float)std::max(0.0, at + time);
        m.name = name;
        if (sound.is_object()) m.sound = AssetRef(sound.value("path", std::string()), Guid::parse(sound.value("guid", std::string())));
        m.unknown = presetMarkerTag(preset->id).dump();
        if (a.markers.size() < ta::TITLE_ANIM_MAX_MARKERS) a.markers.push_back(std::move(m));
    };
    for (const TitleJson& spec : preset->data.value("markers", TitleJson::array())){
        bool when = true;
        if (!e.truthy(spec.value("when", TitleJson()), when)) return ctx.fail("bad_preset", preset->id + ": a marker's when: " + e.error, "preset");
        if (!when) continue;
        TitleJson sound;
        if (spec.contains("sound") && !e.value(spec["sound"], sound)) return ctx.fail("bad_preset", preset->id + ": a marker's sound: " + e.error, "preset");
        const std::string name = spec.value("name", std::string("onMarker"));
        if (spec.contains("generate")){
            const TitleJson& g = spec["generate"];
            const std::string kind = g.value("kind", std::string());
            double period = 0.5, start = 0.0;
            if ((g.contains("start") && !e.number(g["start"], start))) return ctx.fail("bad_preset", preset->id + ": " + e.error, "preset");
            if (kind == "beats"){
                if (!e.number(g.value("period", TitleJson(0.5)), period)) return ctx.fail("bad_preset", preset->id + ": beats: " + e.error, "preset");
                period = std::max(0.02, period);
                for (double t = start; t < D - 1e-6 && a.markers.size() < ta::TITLE_ANIM_MAX_MARKERS; t += period) addMarker(t, name, sound);
            }else if (kind == "ticks"){
                double each = 0.05;
                if (!e.number(g.value("each", TitleJson(0.05)), each)) return ctx.fail("bad_preset", preset->id + ": ticks: " + e.error, "preset");
                const int n = (int)e.get("n");
                for (int i = 0; i < n && a.markers.size() < ta::TITLE_ANIM_MAX_MARKERS; i++) addMarker(start + i * each, name, sound);
            }else{
                return ctx.fail("bad_preset", preset->id + ": marker generators are beats and ticks", "preset");
            }
            continue;
        }
        double time = 0.0;
        if (!e.number(spec.value("time", TitleJson(0)), time)) return ctx.fail("bad_preset", preset->id + ": a marker's time: " + e.error, "preset");
        addMarker(time, name, sound);
    }
    ta::sortMarkers(a.markers);

    // the animation's settings: a fresh animation (or this preset's again) takes the preset's
    const bool fresh = at == 0.0 && (a.tracks.size() == (size_t)written || (a.hasPresetSource && a.presetId == preset->id));
    if (fresh){
        a.duration = automatic ? (float)ta::TITLE_ANIM_AUTO_DURATION : (float)D;
        ta::AnimWrap wrap;
        if (ta::animWrapFromName(preset->wrap, wrap)) a.wrap = wrap;
        double loopStart = 0.0;
        if (preset->data.contains("loop_start") && e.number(preset->data["loop_start"], loopStart)) a.loopStart = (float)loopStart;
        if (preset->overlay) a.overlay = true;
        ta::AnimRole role;
        if (a.role == ta::AnimRole::Custom && ta::animRoleFromName(preset->role, role)) a.role = role;
    }else if (a.duration > 0.0f && !automatic && a.duration < at + D){
        a.duration = (float)std::min(ta::TITLE_ANIM_MAX_DURATION, at + D);
    }
    a.hasPresetSource = true;
    a.presetId = preset->id;
    a.presetParams = params.dump();
    if (written == 0 && preset->data.value("tracks", TitleJson::array()).size() > 0)
        ctx.note("preset_part_unavailable", preset->name + " wrote no tracks on this title");
    tt::writeAnimationSet(ctx.doc, set);
    TitleOpChange c;
    c.path = "animation:" + a.name;
    c.to = TitleJson{{"preset", preset->id}, {"tracks", written}, {"params", params}};
    ctx.change(std::move(c));
    ctx.undoName = "Apply Animation Preset " + preset->name;
    return true;
}

void editor::registerTitleAnimPresetOp(){
    static bool done = false;
    if (done) return;
    done = true;
    TitleArtOps::registerOp("apply_animation_preset", applyTitleAnimationPresetOp);
}

// ==================================================================== Save as Preset (R23)

bool editor::titleAnimationToPreset(const tt::TitleArtDoc& doc, const ta::TitleAnimation& animation, const std::string& id, const std::string& name,
                                    TitleJson& out, std::string& error){
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos){
        error = "a preset id is snake_case (letters, digits, _)";
        return false;
    }
    const std::string mainText = mainTextLayerOf(doc);
    const ta::CompiledTitleAnimation compiled = tt::compileAnimation(doc, animation, nullptr, nullptr);
    const double D = std::max(ta::TITLE_ANIM_MIN_DURATION, compiled.duration);
    // a time as an expression over D (2 decimals of the fraction are exact enough for a design)
    auto timeOf = [&](double t) -> TitleJson{
        if (t <= 0.0) return 0;
        if (std::fabs(t - D) < 1e-6) return "D";
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.6g*D", t / D);
        return std::string(buf);
    };
    TitleJson tracks = TitleJson::array();
    for (const ta::TitleTrack& t : animation.tracks){
        const tt::TrackFieldInfo info = tt::resolveTrackField(doc, t.layer, t.field);
        if (!info.ok){
            error = "the track on '" + t.field + "' does not resolve (" + info.error + "): remove it first";
            return false;
        }
        TitleJson spec = TitleJson::object();
        if (t.layer.empty()) spec["target"] = "title";
        else if (t.layer == mainText) spec["target"] = "main_text";
        else{
            error = "the track on '" + t.field + "' targets layer '" + (info.layer ? info.layer->value("name", t.layer) : t.layer) +
                    "': a preset targets the title or the main text";
            return false;
        }
        spec["field"] = t.field;
        // absolute tracks become relative against the rest (R23)
        const bool virtualField = info.target == ta::TrackTarget::Title || info.target == ta::TrackTarget::Glyph;
        bool relative = t.relative || virtualField;
        std::vector<float> rest;
        if (!t.relative && !virtualField && !info.stepped){
            float v[3];
            size_t w = 0;
            if (!ta::numericFromJson(info.rest, v, w) || (int)w != info.width){
                error = "'" + t.field + "' has no numeric rest to be relative to";
                return false;
            }
            rest.assign(v, v + w);
            relative = true;
        }
        if (info.stepped) relative = false;
        spec["relative"] = relative;
        spec["pivot"] = ta::trackPivotName(t.pivot);
        if (t.stagger) spec["stagger"] = ta::encodeStagger(*t.stagger);
        const bool multiply = info.combine == tt::ParamCombine::Multiply || t.field.find("scale") != std::string::npos || t.field.find("stretch") != std::string::npos ||
                              t.field.find("opacity") != std::string::npos;
        auto relativeValue = [&](const std::vector<float>& v) -> TitleJson{
            if (rest.empty() || info.value == ta::TrackValue::Color) {
                if (info.value == ta::TrackValue::Color) return ta::formatHexColor(v.data());
                if (v.size() == 1) return ta::roundToFloat(v[0]);
                TitleJson a = TitleJson::array();
                for (float x : v) a.push_back(ta::roundToFloat(x));
                return a;
            }
            TitleJson a = TitleJson::array();
            for (size_t i = 0; i < v.size(); i++){
                const double r = i < rest.size() ? rest[i] : 0.0;
                if (multiply && r == 0.0){
                    a.push_back(1.0);
                    continue;
                }
                a.push_back(ta::roundToFloat(multiply ? v[i] / r : v[i] - r));
            }
            return a.size() == 1 ? a[0] : a;
        };
        if (t.drive){
            TitleJson d = ta::encodeDrive(*t.drive);
            if (!rest.empty()){
                d["from"] = relativeValue(t.drive->from);
                d["to"] = relativeValue(t.drive->to);
            }
            spec["drive"] = d;
        }else if (t.reseed){
            spec["reseed"] = TitleJson{{"rate", t.reseed->rate}, {"offset", t.reseed->offset}};
        }else{
            TitleJson keys = TitleJson::array();
            for (const ta::TitleKey& k : t.keys){
                TitleJson key = TitleJson::array({timeOf(k.key.time), info.stepped ? ta::keyValueJson(k) : relativeValue(k.key.value)});
                if (k.key.ease != EaseType::LINEAR) key.push_back(anim::easeName(k.key.ease));
                keys.push_back(key);
            }
            spec["keys"] = keys;
        }
        tracks.push_back(spec);
    }
    TitleJson markers = TitleJson::array();
    for (const ta::TitleMarker& m : animation.markers){
        TitleJson spec = {{"time", timeOf(m.time)}, {"name", m.name}};
        if (!m.sound.empty()) spec["sound"] = TitleJson{{"path", m.sound.path}, {"guid", m.sound.guid.str()}};
        markers.push_back(spec);
    }
    const char* group = animation.role == ta::AnimRole::Intro ? "in" : animation.role == ta::AnimRole::Outro ? "out" :
                        animation.role == ta::AnimRole::Emphasis ? "emphasis" : (animation.role == ta::AnimRole::Idle || animation.role == ta::AnimRole::Win) ? "loop" : "custom";
    out = TitleJson::object();
    out["format"] = TITLE_ANIM_PRESET_FORMAT;
    out["version"] = TITLE_ANIM_PRESET_VERSION;
    out["id"] = id;
    out["name"] = name.empty() ? id : name;
    out["group"] = group;
    out["role"] = ta::animRoleName(animation.role);
    out["duration"] = animation.duration > 0.0f ? TitleJson(ta::roundToFloat(animation.duration)) : TitleJson(-1);
    out["wrap"] = ta::animWrapName(animation.wrap);
    out["loop_start"] = animation.wrap == ta::AnimWrap::Once ? TitleJson(0) : timeOf(animation.loopStart);
    out["overlay"] = animation.overlay;
    out["description"] = "Saved from '" + animation.name + "'";
    out["params"] = TitleJson::array();
    out["tracks"] = tracks;
    out["markers"] = markers;
    return true;
}

bool editor::saveTitleAnimPreset(const TitleJson& preset, const std::string& scope, std::string& path, std::string& error){
    const std::string dir = TitleAnimPresets::folderOf(scope);
    if (dir.empty()){
        error = "no " + scope + " folder (open a project first)";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec){
        error = "cannot make " + dir + ": " + ec.message();
        return false;
    }
    path = (fs::path(dir) / (preset.value("id", std::string("preset")) + ".json")).string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out){
        error = "cannot write " + path;
        return false;
    }
    out << preset.dump(2) << "\n";
    out.close();
    TitleAnimPresets::reload();
    return true;
}
