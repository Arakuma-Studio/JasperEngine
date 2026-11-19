// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GpuQuirks.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "device/GpuTierDatabase.h"
#include "render/SystemRender.h"
#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using nlohmann::json;

bool GpuQuirks::activeFlags[(size_t)GpuQuirk::Count] = {};

namespace {

    struct QuirkInfo {
        const char* name;
        const char* description;
    };

    // R13: the levers, in enum order
    const QuirkInfo QUIRK_INFO[(size_t)GpuQuirk::Count] = {
        {"gl_no_worker_context", "GL: no shared worker context; programs are made on the render thread (parallel or deferred)"},
        {"gl_no_compile_threads", "GL: the driver's compile threads stay off in parallel mode"},
        {"gl_no_warm_draws", "GL: the worker context never draws a new program before the frame does"},
        {"test_tint", "tests only: the clear colour changes (the equivalence checks' red control)"},
    };

    struct QuirkRunState {
        bool resolved = false;
        GpuQuirkState states[(size_t)GpuQuirk::Count];
        std::vector<GpuQuirkOverride> commandLine;
    };

    QuirkRunState& quirkRunState(){
        static QuirkRunState* s = new QuirkRunState();
        return *s;
    }

    bool quirkKnownInBuild(GpuQuirk quirk){
#if JASPERENGINE_GPU_QUIRK_TEST_HOOKS
        return quirk < GpuQuirk::Count;
#else
        return quirk < GpuQuirk::TestTint;
#endif
    }

    std::string quirkLower(std::string text){
        for (char& c : text)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return text;
    }

    // a key's value: a string or a list of strings (lists match any element)
    bool quirkStrings(const json& rule, const char* key, std::vector<std::string>& out, std::string& error){
        auto it = rule.find(key);
        if (it == rule.end())
            return true;
        if (it->is_string()){
            out.push_back(it->get<std::string>());
            return true;
        }
        if (it->is_array()){
            for (const json& item : *it){
                if (!item.is_string()){
                    error = std::string("\"") + key + "\" lists a value that is not a string";
                    return false;
                }
                out.push_back(item.get<std::string>());
            }
            return true;
        }
        error = std::string("\"") + key + "\" must be a string or a list of strings";
        return false;
    }

    bool quirkString(const json& object, const char* key, std::string& out, std::string& error){
        auto it = object.find(key);
        if (it == object.end())
            return true;
        if (!it->is_string()){
            error = std::string("\"") + key + "\" must be a string";
            return false;
        }
        out = it->get<std::string>();
        return true;
    }

    bool quirkAnyEquals(const std::vector<std::string>& values, const std::string& value){
        if (values.empty())
            return true;
        for (const std::string& item : values)
            if (quirkLower(item) == value) return true;
        return false;
    }

    bool quirkAnyFamily(const std::vector<std::string>& patterns, const std::string& normalized){
        if (patterns.empty())
            return true;
        for (const std::string& pattern : patterns)
            if (GpuTierDatabase::familyMatches(pattern, normalized)) return true;
        return false;
    }

    std::vector<int> quirkVersionParts(const std::string& dotted){
        std::vector<int> parts;
        int value = 0;
        bool digit = false;
        for (char c : dotted){
            if (c >= '0' && c <= '9'){
                value = value * 10 + (c - '0');
                digit = true;
            }else{
                parts.push_back(value);
                value = 0;
                digit = false;
            }
        }
        if (digit)
            parts.push_back(value);
        return parts;
    }

    const char* quirkApiOfBackend(const std::string& backend){
        if (backend == "OpenGL 3.3 core") return "glcore";
        if (backend == "OpenGL ES 3 / WebGL2") return "gles3";
        if (backend == "Direct3D 11") return "d3d11";
        if (backend == "Metal") return "metal";
        if (backend == "Vulkan") return "vulkan";
        return "";
    }

    // GL_VERSION opens with the API's version ("OpenGL ES 3.2 V@415.0", "4.5 (Core Profile) Mesa
    // 23.2.1"): the driver's version is what follows it
    std::string quirkDriverVersionText(const std::string& driver){
        size_t i = 0;
        if (driver.compare(0, 10, "OpenGL ES ") == 0)
            i = 10;
        const size_t start = i;
        int dots = 0;
        while (i < driver.size() && ((driver[i] >= '0' && driver[i] <= '9') || driver[i] == '.')){
            dots += driver[i] == '.';
            i++;
        }
        if (i > start && dots >= 1 && dots <= 2 && i < driver.size() && driver[i] == ' ')
            return driver.substr(i + 1);
        return driver;
    }

    // the reason's first clause, for the one log line
    std::string quirkShortReason(const std::string& reason){
        std::string out = reason.substr(0, reason.find_first_of(";:("));
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (out.size() > 72)
            out = out.substr(0, 69) + "...";
        return out;
    }

}

// ---------------------------------------------------------------- the catalogue

std::vector<GpuQuirk> GpuQuirks::known(){
    std::vector<GpuQuirk> out;
    for (size_t i = 0; i < (size_t)GpuQuirk::Count; i++)
        if (quirkKnownInBuild((GpuQuirk)i)) out.push_back((GpuQuirk)i);
    return out;
}

const char* GpuQuirks::name(GpuQuirk quirk){
    return quirk < GpuQuirk::Count ? QUIRK_INFO[(size_t)quirk].name : "";
}

const char* GpuQuirks::description(GpuQuirk quirk){
    return quirk < GpuQuirk::Count ? QUIRK_INFO[(size_t)quirk].description : "";
}

bool GpuQuirks::fromName(const std::string& text, GpuQuirk& quirk){
    for (GpuQuirk candidate : known()){
        if (text == name(candidate)){
            quirk = candidate;
            return true;
        }
    }
    return false;
}

std::string GpuQuirks::knownNames(){
    std::string out;
    for (GpuQuirk quirk : known())
        out += (out.empty() ? "" : ", ") + std::string(name(quirk));
    return out;
}

// ---------------------------------------------------------------- the rules

const std::vector<GpuQuirks::Rule>& GpuQuirks::builtInRules(){
    static const std::vector<Rule>* rules = [](){
        auto* parsed = new std::vector<Rule>();
        std::string error;
        if (!parseRules(embeddedJson(), *parsed, error)){
            Log::error("GPU quirk rules: %s (no rule applies)", error.c_str());
            parsed->clear();
        }
        return parsed;
    }();
    return *rules;
}

bool GpuQuirks::parseRules(const std::string& text, std::vector<Rule>& rules, std::string& error){
    rules.clear();
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        error = "not a JSON object";
        return false;
    }
    if (!root.contains("version") || !root["version"].is_number_integer() || root["version"].get<int>() != 1){
        error = "\"version\" must be 1";
        return false;
    }
    auto list = root.find("rules");
    if (list == root.end() || !list->is_array()){
        error = "\"rules\" must be a list";
        return false;
    }
    static const char* const KEYS[] = {"api", "platform", "vendor", "renderer", "driver", "driver_at_least", "driver_below",
                                       "os_at_least", "os_below"};
    for (size_t i = 0; i < list->size(); i++){
        const json& item = (*list)[i];
        const std::string where = "rule " + std::to_string(i + 1) + ": ";
        if (!item.is_object()){
            error = where + "not an object";
            return false;
        }
        Rule rule;
        std::string problem;
        if (!quirkString(item, "quirk", rule.quirkName, problem) || !quirkString(item, "reason", rule.reason, problem) ||
                !quirkString(item, "issue", rule.issue, problem) || !quirkStrings(item, "check", rule.checks, problem)){
            error = where + problem;
            return false;
        }
        GpuQuirk quirk;
        if (fromName(rule.quirkName, quirk))
            rule.quirk = quirk;
        auto match = item.find("match");
        if (match == item.end() || !match->is_object()){
            error = where + "\"match\" must be an object";
            return false;
        }
        for (auto it = match->begin(); it != match->end(); ++it){
            bool knownKey = false;
            for (const char* key : KEYS)
                knownKey = knownKey || it.key() == key;
            if (!knownKey){
                error = where + "unknown match key \"" + it.key() + "\"";
                return false;
            }
        }
        if (!quirkStrings(*match, "api", rule.api, problem) || !quirkStrings(*match, "platform", rule.platform, problem) ||
                !quirkStrings(*match, "vendor", rule.vendor, problem) || !quirkStrings(*match, "renderer", rule.renderer, problem) ||
                !quirkStrings(*match, "driver", rule.driver, problem) || !quirkString(*match, "driver_at_least", rule.driverAtLeast, problem) ||
                !quirkString(*match, "driver_below", rule.driverBelow, problem) || !quirkString(*match, "os_at_least", rule.osAtLeast, problem) ||
                !quirkString(*match, "os_below", rule.osBelow, problem)){
            error = where + problem;
            return false;
        }
        rules.push_back(std::move(rule));
    }
    return true;
}

std::string GpuQuirks::dottedNumber(const std::string& text){
    for (size_t i = 0; i < text.size(); i++){
        if (text[i] < '0' || text[i] > '9')
            continue;
        size_t end = i;
        int dots = 0;
        while (end < text.size()){
            if (text[end] >= '0' && text[end] <= '9'){
                end++;
            }else if (text[end] == '.' && end + 1 < text.size() && text[end + 1] >= '0' && text[end + 1] <= '9'){
                dots++;
                end++;
            }else{
                break;
            }
        }
        if (dots > 0)
            return text.substr(i, end - i);
        i = end;
    }
    return std::string();
}

bool GpuQuirks::versionLess(const std::string& a, const std::string& b){
    const std::string da = dottedNumber(a);
    const std::string db = dottedNumber(b);
    if (da.empty() || db.empty())
        return false;
    const std::vector<int> pa = quirkVersionParts(da);
    const std::vector<int> pb = quirkVersionParts(db);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); i++){
        const int x = i < pa.size() ? pa[i] : 0;
        const int y = i < pb.size() ? pb[i] : 0;
        if (x != y)
            return x < y;
    }
    return false;
}

bool GpuQuirks::ruleMatches(const Rule& rule, const GpuQuirkDevice& device){
    if (!quirkAnyEquals(rule.api, device.api))
        return false;
    if (!rule.platform.empty() && !quirkAnyEquals(rule.platform, device.platform))
        return false;
    const std::string renderer = GpuTierDatabase::normalize(device.renderer);
    if (!quirkAnyFamily(rule.renderer, renderer))
        return false;
    if (!rule.vendor.empty()){
        GpuQuery query;
        query.renderer = device.renderer;
        query.platform = device.platform.empty() ? std::string("pc") : device.platform;
        if (!quirkAnyEquals(rule.vendor, GpuTierDatabase::builtIn().match(query).vendor))
            return false;
    }
    if (!quirkAnyFamily(rule.driver, GpuTierDatabase::normalize(device.driver)))
        return false;
    // a range needs a number on both sides: a driver string without one never matches it
    auto inRange = [](const std::string& value, const std::string& atLeast, const std::string& below){
        if (atLeast.empty() && below.empty())
            return true;
        if (dottedNumber(value).empty() && quirkVersionParts(value).empty())
            return false;
        // a bare number ("29", an API level) counts as one component
        const std::string v = dottedNumber(value).empty() ? value + ".0" : value;
        auto number = [](const std::string& bound){ return dottedNumber(bound).empty() ? bound + ".0" : bound; };
        if (!atLeast.empty() && versionLess(v, number(atLeast)))
            return false;
        if (!below.empty() && !versionLess(v, number(below)))
            return false;
        return true;
    };
    const std::string driverVersion = quirkDriverVersionText(device.driver);
    if ((!rule.driverAtLeast.empty() || !rule.driverBelow.empty()) &&
            (dottedNumber(driverVersion).empty() || !inRange(driverVersion, rule.driverAtLeast, rule.driverBelow)))
        return false;
    if (!inRange(device.osVersion, rule.osAtLeast, rule.osBelow))
        return false;
    return true;
}

// ---------------------------------------------------------------- overrides

bool GpuQuirks::parseOverrides(const std::string& text, std::vector<GpuQuirkOverride>& out, std::string& error){
    size_t start = 0;
    while (start <= text.size()){
        size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        std::string item = text.substr(start, end - start);
        start = end + 1;
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (item.empty()){
            if (end == text.size()) break;
            continue;
        }
        const size_t colon = item.rfind(':');
        const std::string name = colon == std::string::npos ? item : item.substr(0, colon);
        const std::string value = colon == std::string::npos ? std::string() : item.substr(colon + 1);
        GpuQuirk quirk;
        if (!fromName(name, quirk)){
            error = "unknown quirk \"" + name + "\" (known: " + knownNames() + ")";
            return false;
        }
        if (value != "on" && value != "off"){
            error = "\"" + item + "\" needs :on or :off, for example " + name + ":off";
            return false;
        }
        out.push_back({quirk, value == "on"});
        if (end == text.size()) break;
    }
    return true;
}

void GpuQuirks::setCommandLineOverrides(const std::vector<GpuQuirkOverride>& overrides){
    quirkRunState().commandLine = overrides;
}

std::vector<GpuQuirkOverride> GpuQuirks::environmentOverrides(){
    std::vector<GpuQuirkOverride> out;
    // the variables before the quirk names: aliases of an override, with a deprecation line
    const char* worker = std::getenv("JASPER_GL_SOFTWARE_WORKER");
    if (worker && std::strcmp(worker, "1") == 0){
        Log::warn("JASPER_GL_SOFTWARE_WORKER=1 is deprecated: use JASPERENGINE_GPU_QUIRKS=gl_no_worker_context:off");
        out.push_back({GpuQuirk::GlNoWorkerContext, false});
    }
    const char* warm = std::getenv("JASPER_GL_WARM_DRAWS");
    if (warm && std::strcmp(warm, "1") == 0){
        Log::warn("JASPER_GL_WARM_DRAWS=1 is deprecated: use JASPERENGINE_GPU_QUIRKS=gl_no_warm_draws:off");
        out.push_back({GpuQuirk::GlNoWarmDraws, false});
    }
    const char* value = std::getenv("JASPERENGINE_GPU_QUIRKS");
    if (value && value[0]){
        // each item alone, so one bad name drops only itself
        std::string text(value);
        size_t start = 0;
        while (start < text.size()){
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            std::string error;
            if (!parseOverrides(text.substr(start, end - start), out, error))
                Log::warn("JASPERENGINE_GPU_QUIRKS: %s; ignored", error.c_str());
            start = end + 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------- resolution

std::vector<GpuQuirkState> GpuQuirks::resolve(const GpuQuirkDevice& device, const std::vector<Rule>& rules,
                                              const std::vector<GpuQuirkOverride>& overrides){
    std::vector<GpuQuirkState> states((size_t)GpuQuirk::Count);
    for (const Rule& rule : rules){
        if (rule.quirk >= GpuQuirk::Count || states[(size_t)rule.quirk].active)
            continue;
        if (ruleMatches(rule, device)){
            GpuQuirkState& state = states[(size_t)rule.quirk];
            state.active = true;
            state.source = "rule";
            state.reason = rule.reason;
        }
    }
    for (const GpuQuirkOverride& item : overrides){
        if (item.quirk >= GpuQuirk::Count)
            continue;
        GpuQuirkState& state = states[(size_t)item.quirk];
        state.active = item.on;
        state.source = item.on ? "forced_on" : "forced_off";
        if (item.on && state.reason.empty()){
            // the rule that would apply it elsewhere says why the lever exists
            for (const Rule& rule : rules)
                if (rule.quirk == item.quirk && state.reason.empty()) state.reason = rule.reason;
        }
    }
    return states;
}

GpuQuirkDevice GpuQuirks::runningDevice(){
    GpuQuirkDevice device;
    device.api = quirkApiOfBackend(SystemRender::getBackendName());
    device.driver = SystemRender::getDriverVersion();
    const Platform platform = Engine::getPlatform();
    switch (platform){
        case Platform::Android: device.platform = "android"; break;
        case Platform::iOS:
        case Platform::MacOS: device.platform = "apple"; break;
        case Platform::Web: device.platform = "web"; break;
        default: device.platform = "pc"; break;
    }
    if (System::hasInstance()){
        System& system = System::instance();
        // the web's unmasked WebGL renderer (GL_RENDERER is the browser's generic one), as tier detection
        device.renderer = system.getGpuRendererName();
        device.osVersion = platform == Platform::Android ? std::to_string(system.getAndroidApiLevel()) : system.getOperatingSystemVersion();
    }
    if (device.renderer.empty())
        device.renderer = SystemRender::getDeviceName();
    return device;
}

void GpuQuirks::resolveForDevice(){
    QuirkRunState& s = quirkRunState();
    if (s.resolved)
        return;
    std::vector<GpuQuirkOverride> overrides = environmentOverrides();
    // the command line wins over the environment
    overrides.insert(overrides.end(), s.commandLine.begin(), s.commandLine.end());
    install(resolve(runningDevice(), builtInRules(), overrides));
    Log::print("GPU quirks: %s", describe().c_str());
}

void GpuQuirks::install(const std::vector<GpuQuirkState>& states){
    QuirkRunState& s = quirkRunState();
    for (size_t i = 0; i < (size_t)GpuQuirk::Count; i++){
        s.states[i] = i < states.size() ? states[i] : GpuQuirkState();
        activeFlags[i] = s.states[i].active;
    }
    s.resolved = true;
}

bool GpuQuirks::isResolved(){
    return quirkRunState().resolved;
}

// ---------------------------------------------------------------- reports

const GpuQuirkState& GpuQuirks::state(GpuQuirk quirk){
    static const GpuQuirkState none;
    return quirk < GpuQuirk::Count ? quirkRunState().states[(size_t)quirk] : none;
}

std::vector<std::string> GpuQuirks::activeNames(){
    std::vector<std::string> out;
    for (size_t i = 0; i < (size_t)GpuQuirk::Count; i++)
        if (activeFlags[i]) out.push_back(name((GpuQuirk)i));
    return out;
}

std::vector<std::string> GpuQuirks::forcedOffNames(){
    std::vector<std::string> out;
    for (size_t i = 0; i < (size_t)GpuQuirk::Count; i++)
        if (quirkRunState().states[i].source == "forced_off") out.push_back(name((GpuQuirk)i));
    return out;
}

std::string GpuQuirks::describe(){
    std::string out;
    for (size_t i = 0; i < (size_t)GpuQuirk::Count; i++){
        const GpuQuirkState& state = quirkRunState().states[i];
        if (state.source.empty())
            continue;
        std::string why;
        if (state.source == "rule")
            why = "rule: " + quirkShortReason(state.reason);
        else
            why = state.source == "forced_on" ? "forced on" : "forced off";
        out += (out.empty() ? "" : ", ") + std::string(name((GpuQuirk)i)) + " (" + why + ")";
    }
    return out.empty() ? std::string("none") : out;
}
