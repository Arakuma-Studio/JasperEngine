// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtCheck.h"

#include "pixelart/PixelArtRules.h"

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
using Json = nlohmann::json;

namespace {

    struct RuleEntry {
        std::string id;
        RuleFn builtIn;
    };

    // R10's table: ids are stable API and give the order; cleanup_outputs has no built-in (S46
    // registers it)
    const std::vector<RuleEntry>& ruleTable(){
        static const std::vector<RuleEntry> table = {
            {"pixel_art_mode", rules::pixelArtMode},
            {"integer_scale", rules::integerScale},
            {"filtering", rules::filtering},
            {"mipmaps_compression", rules::mipmapsCompression},
            {"palette", rules::palette},
            {"texel_size", rules::texelSize},
            {"off_grid", rules::offGrid},
            {"rotation", rules::rotation},
            {"font_antialiasing", rules::fontAntialiasing},
            {"particles_grid", rules::particlesGrid},
            {"anti_aliasing", rules::antiAliasing},
            {"sprite_variants", rules::spriteVariants},
            {"cleanup_outputs", nullptr},
        };
        return table;
    }

    std::mutex& registryMutex(){
        static std::mutex mutex;
        return mutex;
    }
    std::map<std::string, RuleFn>& registered(){
        static std::map<std::string, RuleFn> rules;
        return rules;
    }
    FontAnalyzer& fontAnalyzer(){
        static FontAnalyzer analyzer;
        return analyzer;
    }

    RuleFn ruleFunction(const std::string& id){
        std::lock_guard<std::mutex> lock(registryMutex());
        auto it = registered().find(id);
        if (it != registered().end())
            return it->second;
        for (const RuleEntry& entry : ruleTable())
            if (entry.id == id)
                return entry.builtIn;
        return nullptr;
    }

    Json optionJson(const FixOption& option){
        Json changes = Json::array();
        for (const FixChange& change : option.changes)
            changes.push_back({{"target", change.target}, {"field", change.field}, {"from", change.from}, {"to", change.to}});
        return {{"id", option.id}, {"label", option.label}, {"changes", changes}};
    }

    Json findingJson(const Finding& finding){
        Json out = {
            {"id", finding.id},
            {"key", finding.key},
            {"rule", finding.rule},
            {"status", ruleStatusName(finding.status)},
            {"target", finding.target},
            {"measured", finding.measured},
            {"expected", finding.expected},
            {"message", finding.message},
        };
        if (finding.options.empty()){
            out["fix"] = nullptr;
            out["needs_person"] = finding.needsPerson;
        }else{
            Json options = Json::array();
            for (const FixOption& option : finding.options)
                options.push_back(optionJson(option));
            out["fix"] = {{"options", options}};
        }
        return out;
    }

}

const char* jasperengine::editor::pixelart::ruleStatusName(RuleStatus status){
    switch (status){
        case RuleStatus::Fail: return "FAIL";
        case RuleStatus::Warn: return "WARN";
        case RuleStatus::Pass: default: return "PASS";
    }
}

const char* jasperengine::editor::pixelart::drawableKindName(DrawableKind kind){
    switch (kind){
        case DrawableKind::Tilemap: return "tilemap";
        case DrawableKind::Image: return "image";
        case DrawableKind::Text: return "text";
        case DrawableKind::Particles: return "particles";
        case DrawableKind::Sprite: default: return "sprite";
    }
}

Json jasperengine::editor::pixelart::number(double value){
    if (!std::isfinite(value))
        return 0;
    const double rounded = std::round(value * 1000000.0) / 1000000.0;
    if (std::fabs(rounded - std::round(rounded)) < 1e-9 && std::fabs(rounded) < 9.0e15)
        return (int64_t)std::llround(rounded);
    // six decimals at most, as textureCanonicalNumber prints; the JSON writer prints the shortest
    // text that reads back as this double ("2.134375", "0.5")
    return rounded;
}

bool jasperengine::editor::pixelart::isPixelArtBase(int width, int height){
    return width > 0 && height > 0 && width <= 640 && height <= 360;
}

PixelRotation jasperengine::editor::pixelart::effectiveRotation(const PixelArtDrawable& drawable, const PixelArtCamera& camera){
    if (drawable.kind == DrawableKind::Sprite && drawable.pixelRotation != PixelRotation::CAMERA)
        return drawable.pixelRotation;
    if (camera.hasComponent && camera.component.rotationMode == PixelRotation::ON_GRID)
        return PixelRotation::ON_GRID;
    return PixelRotation::STEPS_90;
}

const std::vector<std::string>& jasperengine::editor::pixelart::ruleIds(){
    static const std::vector<std::string> ids = [](){
        std::vector<std::string> out;
        for (const RuleEntry& entry : ruleTable())
            out.push_back(entry.id);
        return out;
    }();
    return ids;
}

bool jasperengine::editor::pixelart::isKnownRule(const std::string& id){
    return (bool)ruleFunction(id);
}

void jasperengine::editor::pixelart::registerRule(std::string id, RuleFn rule){
    std::lock_guard<std::mutex> lock(registryMutex());
    registered()[std::move(id)] = std::move(rule);
}

void jasperengine::editor::pixelart::unregisterRule(const std::string& id){
    std::lock_guard<std::mutex> lock(registryMutex());
    registered().erase(id);
}

void jasperengine::editor::pixelart::setFontAnalyzer(FontAnalyzer analyzer){
    std::lock_guard<std::mutex> lock(registryMutex());
    fontAnalyzer() = std::move(analyzer);
}

PixelArtFontInfo jasperengine::editor::pixelart::rules::analyzeFont(const PixelArtCheckInput& input, const std::string& font, unsigned int size){
    FontAnalyzer analyzer;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        analyzer = fontAnalyzer();
    }
    if (analyzer)
        return analyzer(input.assetsRoot, font, size);
    PixelArtFontInfo info;
    info.font = font;
    info.size = size;
    return info;
}

CheckResult jasperengine::editor::pixelart::run(const PixelArtCheckInput& input, const std::vector<std::string>& selected){
    CheckResult result;
    // registered rules a later spec placed outside R10's table run after it, by id
    std::vector<std::string> order = ruleIds();
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const auto& [id, fn] : registered())
            if (std::find(order.begin(), order.end(), id) == order.end())
                order.push_back(id);
    }
    for (const std::string& id : order){
        if (!selected.empty() && std::find(selected.begin(), selected.end(), id) == selected.end())
            continue;
        RuleFn fn = ruleFunction(id);
        if (!fn)
            continue;
        std::vector<Finding> found;
        RuleResult rule = fn(input, found);
        rule.id = id;
        std::stable_sort(found.begin(), found.end(), [](const Finding& a, const Finding& b){ return a.key < b.key; });
        rule.findings = 0;
        rule.status = RuleStatus::Pass;
        for (Finding& finding : found){
            finding.rule = id;
            if (finding.status == RuleStatus::Pass)
                continue;
            rule.findings++;
            rule.status = std::max(rule.status, finding.status);
            result.findings.push_back(std::move(finding));
        }
        result.rules.push_back(rule);
    }
    Verdict verdict = Verdict::Clean;
    for (size_t i = 0; i < result.findings.size(); i++){
        Finding& finding = result.findings[i];
        finding.id = "F" + std::to_string(i + 1);
        verdict = worse(verdict, finding.options.empty() ? Verdict::NeedsPerson : Verdict::Fixable);
    }
    result.verdict = verdict;

    const PixelArtProjectInfo& p = input.project;
    Json screens = Json::array();
    for (const PixelArtScreen& screen : p.screens)
        screens.push_back(std::to_string(screen.width) + "x" + std::to_string(screen.height));
    result.project = {
        {"mode", p.modeEnabled},
        {"base_resolution", {p.canvasWidth, p.canvasHeight}},
        {"scaling", rules::scalingName(p.scaling)},
        {"pixels_per_unit", p.pixelsPerUnit},
        {"default_filter", textureFilterModeName(p.defaultFilterMode)},
        {"window", {p.windowWidth, p.windowHeight}},
        {"check_screens", screens},
    };
    result.cameras = Json::array();
    for (const PixelArtScene& scene : input.scenes)
        if (scene.pixelArt)
            result.cameras.push_back(rules::cameraJson(input, scene));
    return result;
}

int CheckResult::count(RuleStatus status) const{
    int n = 0;
    for (const Finding& finding : findings)
        n += finding.status == status ? 1 : 0;
    return n;
}

const Finding* CheckResult::find(const std::string& keyOrId) const{
    for (const Finding& finding : findings)
        if (finding.key == keyOrId || finding.id == keyOrId)
            return &finding;
    return nullptr;
}

Json CheckResult::toJson() const{
    Json rulesJson = Json::array();
    for (const RuleResult& rule : rules)
        rulesJson.push_back({{"id", rule.id}, {"status", ruleStatusName(rule.status)}, {"checked", rule.checked}, {"findings", rule.findings}});
    Json findingsJson = Json::array();
    int fixable = 0;
    for (const Finding& finding : findings){
        findingsJson.push_back(findingJson(finding));
        fixable += finding.options.empty() ? 0 : 1;
    }
    return {
        {"verdict", verdictName(verdict)},
        {"counts", {{"fail", count(RuleStatus::Fail)}, {"warn", count(RuleStatus::Warn)}, {"fixable", fixable},
                    {"needs_person", (int)findings.size() - fixable}}},
        {"rules", rulesJson},
        {"findings", findingsJson},
        {"project", project},
        {"camera", cameras},
    };
}

// ---- fixes

size_t FixPlan::changeCount() const{
    size_t n = 0;
    for (const auto& [key, option] : fixes)
        n += option.changes.size();
    return n;
}

Json FixPlan::toJson() const{
    Json out = Json::array();
    for (const auto& [key, option] : fixes){
        Json item = optionJson(option);
        item["key"] = key;
        item["option"] = option.id;
        item.erase("id");
        out.push_back(item);
    }
    return out;
}

FixPlan jasperengine::editor::pixelart::plan(const CheckResult& result, const std::vector<std::string>& selection){
    FixPlan out;
    // finding index -> chosen option id (empty: the default)
    std::map<size_t, std::string> chosen;
    std::set<std::string> needsPerson;
    for (const std::string& raw : selection){
        std::string selector = raw;
        std::string option;
        const size_t at = raw.rfind('@');
        if (at != std::string::npos){
            selector = raw.substr(0, at);
            option = raw.substr(at + 1);
        }
        std::vector<size_t> matched;
        bool explicitFinding = false;
        if (selector == "all"){
            for (size_t i = 0; i < result.findings.size(); i++)
                matched.push_back(i);
        }else if (isKnownRule(selector)){
            for (size_t i = 0; i < result.findings.size(); i++)
                if (result.findings[i].rule == selector)
                    matched.push_back(i);
        }else{
            for (size_t i = 0; i < result.findings.size(); i++){
                if (result.findings[i].key == selector || result.findings[i].id == selector){
                    matched.push_back(i);
                    explicitFinding = true;
                }
            }
            if (matched.empty()){
                out.unknown.push_back(raw);
                continue;
            }
        }
        for (size_t i : matched){
            const Finding& finding = result.findings[i];
            if (finding.options.empty()){
                if (explicitFinding)
                    needsPerson.insert(finding.key);
                continue;
            }
            if (!option.empty()){
                const bool has = std::any_of(finding.options.begin(), finding.options.end(), [&](const FixOption& o){ return o.id == option; });
                if (!has){
                    // a rule or "all" with an option applies it where it exists
                    if (explicitFinding)
                        out.badOptions.push_back(finding.key + "@" + option);
                    else if (!chosen.count(i))
                        chosen[i] = std::string();
                    continue;
                }
            }
            if (!option.empty() || !chosen.count(i))
                chosen[i] = option;
        }
    }
    for (const auto& [index, option] : chosen){
        const Finding& finding = result.findings[index];
        const FixOption* picked = &finding.options.front();
        for (const FixOption& candidate : finding.options)
            if (!option.empty() && candidate.id == option)
                picked = &candidate;
        out.fixes.push_back({finding.key, *picked});
    }
    out.needsPerson.assign(needsPerson.begin(), needsPerson.end());
    return out;
}
