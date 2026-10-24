// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HTML/CSS UI effects (phase-5 spec 3.13): list_ui_effects (the catalogue `jasperengine-editor
// ui-effects --json` prints, filtered, with the project's effects) and set_ui_effect_time (the
// effect clock pinned for deterministic images, or live again; for stateful effects also a
// scripted pointer path and splats on one element, replayed exactly at the pinned clock).
// inspect_ui reports the effects an element uses (HtmlUiTools.cpp).
#include "McpTools.h"
#include "HtmlUiShared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "htmlui/effects/EffectManifest.h"
#include "htmlui/effects/EffectRegistry.h"
#include "htmlui/effects/EffectRuntime.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElement.h"
#include "util/ProjectChecks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
namespace fx = ::jasperengine::htmlui;

// above this many effects a list leaves out the parameters (name one for them)
constexpr size_t FULL_ENTRIES_MAX = 12;

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string joined(const std::vector<std::string>& items, size_t limit = 8) {
    std::string text;
    for (size_t i = 0; i < items.size() && i < limit; i++) text += (i ? ", " : "") + items[i];
    if (items.size() > limit) text += ", ...";
    return text;
}

// "speed number 1 [0..10]: how fast", one parameter of a manifest as text
std::string paramLine(const Json& param) {
    std::string line = param.value("name", std::string()) + " " + param.value("type", std::string()) + " " +
                       param.value("default", std::string());
    const std::string range = param.value("range", std::string());
    if (!range.empty()) line += " [" + range + "]";
    if (param.contains("options")) {
        std::vector<std::string> options = param["options"].get<std::vector<std::string>>();
        line += " {" + joined(options, 12) + "}";
    }
    const std::string description = param.value("description", std::string());
    if (!description.empty()) line += ": " + description;
    return line;
}

// ------------------------------------------------------------------ list_ui_effects

void listUiEffectsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    fx::EffectRegistry& registry = fx::EffectRegistry::get();

    // the engine's effects and the project's the documents declared (@effect), then the
    // project's effect files no document declares yet
    std::vector<const fx::EffectManifest*> effects = registry.all();
    std::set<const fx::EffectManifest*> undeclared;
    std::vector<std::unique_ptr<fx::EffectManifest>> parsed;
    if (project && !project->getProjectPath().empty()) {
        for (const std::string& file : ProjectChecks::uiEffectFiles(project)) {
            std::string source;
            if (!ProjectChecks::readFile(project->getProjectPath() / fs::u8path(file), source)) continue;
            auto manifest = std::make_unique<fx::EffectManifest>();
            std::vector<fx::EffectDiagnostic> diags;
            if (!fx::EffectManifestParser::parse(source, file, false, *manifest, diags) || registry.find(manifest->name)) continue;
            undeclared.insert(manifest.get());
            effects.push_back(manifest.get());
            parsed.push_back(std::move(manifest));
        }
    }
    std::stable_sort(effects.begin(), effects.end(), [](const fx::EffectManifest* a, const fx::EffectManifest* b) { return a->name < b->name; });

    const std::string name = lowerCopy(args.value("name", std::string()));
    const std::string category = lowerCopy(args.value("category", std::string()));
    const std::string kind = lowerCopy(args.value("kind", std::string()));
    std::vector<const fx::EffectManifest*> matches;
    std::set<std::string> categories;
    for (const fx::EffectManifest* m : effects) {
        categories.insert(m->category);
        if (!name.empty() && m->name != name) continue;
        if (!category.empty() && m->category != category) continue;
        if (!kind.empty() && fx::effectKindName(m->kind) != kind) continue;
        matches.push_back(m);
    }
    if (!name.empty() && matches.empty()) {
        const std::vector<std::string> suggestions = registry.suggestions(name);
        call->fail("No UI effect '" + name + "'" + (suggestions.empty() ? std::string(".") : "; did you mean " + joined(suggestions, 5) + "?") +
                   " Call list_ui_effects without name for the list.");
        return;
    }
    if (!category.empty() && !categories.count(category)) {
        call->fail("No UI effect category '" + category + "'. Categories: " +
                   joined(std::vector<std::string>(categories.begin(), categories.end()), 20) + ".");
        return;
    }

    const bool full = !name.empty() || matches.size() <= FULL_ENTRIES_MAX;
    Json list = Json::array();
    std::string text = std::to_string(matches.size()) + " UI effect(s)";
    if (!full) text += " (name one for its parameters)";
    text += ". Use: background-image/filter/backdrop-filter: effect(<name>, <param> <value>, ...).";
    for (const fx::EffectManifest* m : matches) {
        Json entry = ProjectChecks::uiEffectJson(*m);
        if (!m->builtin) {
            entry["declared"] = !undeclared.count(m);
            if (project && !project->getProjectPath().empty()) {
                // the file as the project shows it
                entry["path"] = ProjectChecks::projectRelative(project->getProjectPath(), ProjectChecks::uiEffectFile(project, m->sourcePath));
            }
        }
        text += "\n" + m->name + " (" + fx::effectKindName(m->kind) + ", " + m->category + ", cost " + fx::effectCostName(m->cost) +
                (m->backdrop ? ", backdrop" : "") + (m->builtin ? "" : ", project " + entry.value("path", m->sourcePath)) + "): " + m->summary;
        if (full) {
            text += "\n  " + entry.value("example", std::string());
            for (const Json& param : entry["params"]) text += "\n  - " + paramLine(param);
            // phase 6: a simulation's images
            if (entry.contains("state")) {
                std::vector<std::string> images;
                for (const Json& st : entry["state"]["states"])
                    images.push_back(st.value("name", std::string()) + " " + st.value("format", std::string()) + " " + st.value("size", std::string()));
                text += "\n  state: " + joined(images, 8) + "; " + std::to_string(entry["state"]["passes"].size()) + " passes";
            }
        } else {
            std::vector<std::string> names;
            for (const Json& param : entry["params"]) names.push_back(param.value("name", std::string()));
            entry.erase("params");
            entry.erase("state");
            entry["param_names"] = names;
        }
        list.push_back(std::move(entry));
    }
    call->ok(text, Json{{"count", matches.size()}, {"effects", list}});
}

// ------------------------------------------------------------------ set_ui_effect_time

void setUiEffectTimeTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("time")) {
        call->fail("time is required: seconds to pin the effect clock at, or null for the live clock.");
        return;
    }
    const Json& value = args["time"];
    std::optional<double> time;
    if (!value.is_null()) {
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0.0 || value.get<double>() > 1.0e7) {
            call->fail("time must be seconds from 0 to 10000000, or null for the live clock.");
            return;
        }
        time = value.get<double>();
    }
    // stateful effects' scripted input: a pointer path and splats on one element (pinned clock only).
    // Everything is checked before anything changes: a refused call leaves the clock as it was
    const bool scripted = args.contains("pointer_path") || args.contains("splats");
    const std::string selector = args.value("selector", std::string());
    std::vector<Vector3> path;
    std::vector<UIEffectSplat> splats;
    auto finite = [](const Json& v) { return v.is_number() && std::isfinite(v.get<double>()) && std::fabs(v.get<double>()) < 1.0e30; };
    if (scripted) {
        if (!time) {
            call->fail("pointer_path and splats replay at a pinned clock: give time in seconds.");
            return;
        }
        if (selector.empty()) {
            call->fail("selector is required with pointer_path or splats: the element whose effect takes them.");
            return;
        }
        if (args.contains("pointer_path")) {
            const Json& points = args["pointer_path"];
            if (!points.is_array() || points.size() > 512) {
                call->fail("pointer_path must be an array of at most 512 [x, y, t] (element CSS px, effect seconds), or [] to clear it.");
                return;
            }
            for (const Json& point : points) {
                if (!point.is_array() || point.size() != 3 || !finite(point[0]) || !finite(point[1]) || !finite(point[2])) {
                    call->fail("pointer_path points are [x, y, t]: x, y in the element's CSS px (border box), t in effect seconds.");
                    return;
                }
                path.emplace_back(point[0].get<float>(), point[1].get<float>(), point[2].get<float>());
            }
        }
        if (args.contains("splats")) {
            const Json& list = args["splats"];
            if (!list.is_array() || list.size() > 64) {
                call->fail("splats must be an array of at most 64 objects {x, y, dx, dy, radius, color, strength, t}.");
                return;
            }
            for (const Json& item : list) {
                if (!item.is_object() || !finite(item.value("x", Json())) || !finite(item.value("y", Json()))) {
                    call->fail("each splat needs x and y (the effect area's CSS px); dx, dy px/s, radius px, color CSS, strength, t s optional.");
                    return;
                }
                for (const char* key : {"dx", "dy", "radius", "strength", "t"}) {
                    if (item.contains(key) && !finite(item[key])) {
                        call->fail(std::string("splat ") + key + " must be a finite number.");
                        return;
                    }
                }
                if (item.contains("color") && !item["color"].is_string()) {
                    call->fail("splat color must be a CSS colour string.");
                    return;
                }
                UIEffectSplat splat;
                splat.x = item["x"].get<float>();
                splat.y = item["y"].get<float>();
                // the engine clamps these too (half-float state images must stay finite)
                splat.dx = std::clamp(item.value("dx", 0.0f), -100000.0f, 100000.0f);
                splat.dy = std::clamp(item.value("dy", 0.0f), -100000.0f, 100000.0f);
                splat.radius = std::clamp(item.value("radius", 24.0f), 0.0f, 4096.0f);
                splat.strength = std::clamp(item.value("strength", 1.0f), -100.0f, 100.0f);
                splat.color = item.value("color", std::string("#ffffff"));
                splat.time = item.contains("t") ? std::max(0.0, item["t"].get<double>()) : *time;
                splats.push_back(splat);
            }
        }
    }
    Project* project = call->ctx().project();
    Json data;
    std::string scriptText;
    std::optional<UIElement> element;
    SceneProject* elementScene = nullptr;
    if (scripted) {
        double x = 0, y = 0;
        std::string error;
        Json found;
        if (!htmlui_mcp::elementCenter(call->ctx(), args.value("entity", Json()), selector, x, y, error, &found)) {
            call->fail(error + ".");
            return;
        }
        elementScene = project ? project->getScene(found["scene"].get<uint32_t>()) : nullptr;
        if (!elementScene || !elementScene->scene) {
            call->fail("the document's scene is not loaded.");
            return;
        }
        UIDocument document(elementScene->scene, found["entity"]["id"].get<Entity>());
        element = document.querySelector(found.value("ref", selector));
        // a step takes at most 8 splats (the step block): splats in one step interval of an effect's
        // clock, (T - (n + 1) h, T - n h], beyond that are refused, not dropped
        bool stateful = false;
        for (const UIEffectInfo& effect : element->getEffects()) {
            if (!effect.stateful) continue;
            stateful = true;
            const fx::EffectManifest* manifest = fx::EffectRegistry::get().find(effect.name);
            const double h = 1.0 / std::max(1.0, manifest ? (double)manifest->rateHz : 60.0);
            std::map<long long, int> perStep;
            for (const UIEffectSplat& splat : splats) {
                if (splat.time > *time + 1e-9) continue;   // future input: not replayed yet
                const long long step = static_cast<long long>(std::floor((*time - splat.time) / h + 1e-9));
                if (++perStep[step] > static_cast<int>(fx::EffectRuntime::kMaxSplatsPerStep)) {
                    std::ostringstream message;
                    message << "more than " << fx::EffectRuntime::kMaxSplatsPerStep << " splats fall in one step of '" << effect.name
                            << "' (" << h << " s ending at " << (*time - static_cast<double>(step) * h)
                            << " s): a step takes 8; spread their t over several steps.";
                    call->fail(message.str());
                    return;
                }
            }
        }
        if (!stateful) {
            call->fail("\"" + selector + "\" uses no stateful effect (pointer_path and splats drive simulations, e.g. effect(fluid)).");
            return;
        }
        int simulations = 0;
        if (args.contains("pointer_path")) simulations = std::max(simulations, element->effectPointerPath(path));
        if (args.contains("splats")) {
            int cleared = element->effectClearSplats();
            for (const UIEffectSplat& splat : splats) cleared = std::max(cleared, element->effectSplat(splat));
            simulations = std::max(simulations, cleared);
        }
        if (simulations == 0) {
            call->fail("\"" + selector + "\" has no stateful effect simulation yet: a stateful effect starts at its first draw (show "
                       "the element, take a screenshot), then call again.");
            return;
        }
        // what the replay at the pinned time takes: input before T - 30 s is outside any replay
        // (the window reaches back to the earliest input, at most 30 s), input after T waits
        double earliest = *time;
        size_t future = 0, tooOld = 0;
        for (const UIEffectSplat& splat : splats) {
            if (splat.time > *time + 1e-9) future++;
            else if (splat.time < *time - fx::EffectRuntime::kMaxScriptedReplay) tooOld++;
            earliest = std::min(earliest, splat.time);
        }
        if (!path.empty()) earliest = std::min(earliest, static_cast<double>(path.front().z));
        data["element"] = found.value("ref", selector);
        data["simulations"] = simulations;
        data["path"] = path.size();
        data["splats"] = splats.size();
        data["splats_applied"] = splats.size() - future - tooOld;
        data["splats_future"] = future;
        data["replay_from"] = std::max(earliest, *time - fx::EffectRuntime::kMaxScriptedReplay);
        std::ostringstream text;
        text << " " << data["element"].get<std::string>() << ": ";
        if (args.contains("pointer_path")) text << path.size() << "-point pointer path";
        if (args.contains("pointer_path") && args.contains("splats")) text << ", ";
        if (args.contains("splats")) {
            text << splats.size() << " splat(s)";
            if (future) text << " (" << future << " after the pinned time: drawn once the time reaches them)";
        }
        text << " on " << simulations << " simulation(s), replayed from the effect's reset at every pinned time (kept until "
             << "replaced; [] clears).";
        if (tooOld || (!path.empty() && path.front().z < *time - fx::EffectRuntime::kMaxScriptedReplay))
            text << " Warning: input more than " << fx::EffectRuntime::kMaxScriptedReplay
                 << " s before the pinned time is outside the replay and has no effect.";
        scriptText = text.str();
    }
    UIDocument::setEffectTime(time);
    // the edit and play views draw again with it (an idle view draws only when flagged)
    if (project) {
        for (SceneProject* sceneProject : project->getHtmlUIScenes()) sceneProject->needUpdateRender = true;
    }
    if (elementScene) elementScene->needUpdateRender = true;
    const double clock = UIDocument::getEffectTime();
    data["time"] = time ? Json(*time) : Json(nullptr);
    data["clock"] = clock;
    std::ostringstream text;
    if (time) {
        text << "UI effect clock pinned at " << *time << " s in every view: renders are deterministic (pointer state aside). "
             << "set_ui_effect_time {time: null} makes it live again." << scriptText;
    } else {
        text << "UI effect clock live again (now " << clock << " s).";
    }
    call->ok(text.str(), data);
}

} // namespace

void registerUiEffectTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "list_ui_effects";
        tool.title = "List UI Effects";
        tool.description =
            "HTML/CSS UI GPU effects for CSS effect(<name>, ...): name, kind (paint: background-image/mask-image; filter: "
            "filter/backdrop-filter), category, cost, summary, example; params (type, default, range) when named or few "
            "match. Project .frag effects too.";
        tool.inputSchema = object({
            {"name", string("All its parameters.")},
            {"category", string("e.g. glass, blur, light.")},
            {"kind", stringEnum("", {"paint", "filter"})}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Read;
        tool.handler = listUiEffectsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "set_ui_effect_time";
        tool.title = "Set UI Effect Time";
        tool.description = "Pin the HTML/CSS UI effect clock (edit/play views) for identical screenshots.";
        tool.inputSchema = object({
            {"time", Json{{"type", Json::array({"number", "null"})}, {"description", "Seconds; null: live."}}},
            {"entity", entityRef("UI Document.")},
            {"selector", string("Element whose effects take pointer_path/splats.")},
            {"pointer_path", array(Json{{"type", "array"}, {"items", {{"type", "number"}}}}, "Scripted drag [[x,y,t]] (element px, s); [] clears.")},
            {"splats", array(Json{{"type", "object"}, {"required", {"x", "y"}}},
                             "[{x,y,dx,dy,radius,color,strength,t}] (area px, px/s; 8 a step); replaces.")}
        }, {"time"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Control;
        tool.handler = setUiEffectTimeTool;
        registry.addTool(std::move(tool));
    }
}

} // namespace jasperengine::editor::mcp
