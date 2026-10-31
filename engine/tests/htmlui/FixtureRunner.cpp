// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FixtureRunner.h"

#include "TestRenderCommon.h"
#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "HtmlUIClock.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"
#include "media/UIVideoElement.h"

#include "json.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    bool readFile(const std::string& path, std::string& text){
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::ostringstream stream;
        stream << file.rdbuf();
        text = stream.str();
        return true;
    }

    bool fail(std::string* error, const std::string& message){
        if (error)
            *error = message;
        return false;
    }

    // "#id" -> "id"; "" when the target is not of that form
    std::string targetId(const nlohmann::json& value){
        if (!value.is_string())
            return std::string();
        const std::string text = value.get<std::string>();
        if (text.size() < 2 || text[0] != '#')
            return std::string();
        for (size_t i = 1; i < text.size(); i++){
            const char c = text[i];
            if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_'))
                return std::string();
        }
        return text.substr(1);
    }

}

// ---- sidecars -------------------------------------------------------------------------------------

std::vector<std::string> FixtureSidecar::captures() const{
    std::vector<std::string> suffixes;
    for (const FixtureStep& step : steps){
        if (step.kind == FixtureStep::Kind::Capture)
            suffixes.push_back(step.suffix);
    }
    if (suffixes.empty())
        suffixes.push_back(std::string());
    return suffixes;
}

bool jasperengine::test::loadSidecar(const std::string& path, FixtureSidecar& sidecar, std::string* error){
    sidecar = FixtureSidecar();
    std::string text;
    if (!readFile(path, text))
        return fail(error, "cannot read " + path);
    try{
        const nlohmann::json json = nlohmann::json::parse(text);
        const nlohmann::json& viewport = json.at("viewport");
        if (!viewport.is_array() || viewport.size() != 2)
            return fail(error, path + ": \"viewport\" must be [width, height]");
        sidecar.width = viewport[0].get<int>();
        sidecar.height = viewport[1].get<int>();
        if (sidecar.width <= 0 || sidecar.height <= 0 || sidecar.width > 4096 || sidecar.height > 4096)
            return fail(error, path + ": bad viewport");
        sidecar.boxes = json.value("boxes", false);
        sidecar.noText = json.value("noText", false);
        sidecar.comment = json.value("comment", "");
        if (json.contains("tolerance")){
            // "hardware": what a hardware GPU needs on top (its measurements); the goldens'
            // renderer (llvmpipe, lavapipe) keeps the rest (GpuEnvironment::isReferenceRenderer)
            auto apply = [&sidecar](const nlohmann::json& tolerance){
                sidecar.gate.channelThreshold = tolerance.value("channel", sidecar.gate.channelThreshold);
                sidecar.gate.maxBadPercent = tolerance.value("badPercent", sidecar.gate.maxBadPercent);
                sidecar.gate.maxCluster = tolerance.value("cluster", sidecar.gate.maxCluster);
                sidecar.boxTolerance = tolerance.value("box", sidecar.boxTolerance);
            };
            const nlohmann::json& tolerance = json["tolerance"];
            const bool hardware = tolerance.contains("hardware") && !GpuEnvironment::isReferenceRenderer();
            sidecar.toleranceOverridden = hardware || tolerance.size() > (tolerance.contains("hardware") ? 1u : 0u);
            apply(tolerance);
            if (hardware)
                apply(tolerance["hardware"]);
        }
        if (json.contains("samples")){
            sidecar.sampleCount = json["samples"].get<int>();
            if (sidecar.sampleCount != 1 && sidecar.sampleCount != 4)
                return fail(error, path + ": \"samples\" is 1 or 4");
        }
        if (json.contains("selfTest")){
            const nlohmann::json& self = json["selfTest"];
            sidecar.expectMismatch = self.value("expect", "") == "mismatch";
            sidecar.referenceCss = self.value("referenceCss", "");
        }
        static const std::pair<const char*, FixtureStep::Kind> kinds[] = {
            {"hover", FixtureStep::Kind::Hover}, {"focus", FixtureStep::Kind::Focus}, {"press", FixtureStep::Kind::Press},
            {"release", FixtureStep::Kind::Release}, {"addClass", FixtureStep::Kind::AddClass},
            {"removeClass", FixtureStep::Kind::RemoveClass}, {"setStyle", FixtureStep::Kind::SetStyle},
            {"setAttr", FixtureStep::Kind::SetAttr}, {"scroll", FixtureStep::Kind::Scroll}, {"advance", FixtureStep::Kind::Advance},
            {"capture", FixtureStep::Kind::Capture}, {"seekVideo", FixtureStep::Kind::SeekVideo}};
        size_t index = 0;
        for (const nlohmann::json& item : json.value("steps", nlohmann::json::array())){
            index++;
            const std::string where = path + ": step " + std::to_string(index);
            if (!item.is_object())
                return fail(error, where + " is not an object");
            const nlohmann::json* value = nullptr;
            FixtureStep step;
            int found = 0;
            for (const auto& kind : kinds){
                if (item.contains(kind.first)){
                    step.kind = kind.second;
                    value = &item[kind.first];
                    found++;
                }
            }
            if (found != 1 || item.size() != size_t(1 + (item.contains("keyboard") ? 1 : 0)))
                return fail(error, where + ": one step kind per object (\"keyboard\" goes with \"focus\")");
            const nlohmann::json& v = *value;
            switch (step.kind){
            case FixtureStep::Kind::Hover:
            case FixtureStep::Kind::Focus:
            case FixtureStep::Kind::Press:
                step.id = targetId(v);
                step.keyboard = item.value("keyboard", false);
                if (step.id.empty())
                    return fail(error, where + ": the target is \"#id\"");
                break;
            case FixtureStep::Kind::Release:
                if (!(v.is_boolean() && v.get<bool>())){
                    step.id = targetId(v);
                    if (step.id.empty())
                        return fail(error, where + ": \"release\": true or \"#id\"");
                }
                break;
            case FixtureStep::Kind::AddClass:
            case FixtureStep::Kind::RemoveClass:
                if (!v.is_array() || v.size() != 2 || targetId(v[0]).empty() || !v[1].is_string())
                    return fail(error, where + ": [\"#id\", \"class\"]");
                step.id = targetId(v[0]);
                step.name = v[1].get<std::string>();
                break;
            case FixtureStep::Kind::SetStyle:
            case FixtureStep::Kind::SetAttr:
                if (!v.is_array() || v.size() != 3 || targetId(v[0]).empty() || !v[1].is_string() ||
                    !(v[2].is_string() || (step.kind == FixtureStep::Kind::SetAttr && v[2].is_null())))
                    return fail(error, where + ": [\"#id\", \"name\", \"value\"]");
                step.id = targetId(v[0]);
                step.name = v[1].get<std::string>();
                step.remove = v[2].is_null() || (step.kind == FixtureStep::Kind::SetStyle && v[2].get<std::string>().empty());
                if (v[2].is_string())
                    step.value = v[2].get<std::string>();
                break;
            case FixtureStep::Kind::Scroll:
                if (!v.is_array() || v.size() != 3 || targetId(v[0]).empty() || !v[1].is_number() || !v[2].is_number())
                    return fail(error, where + ": [\"#id\", x, y]");
                step.id = targetId(v[0]);
                step.x = v[1].get<float>();
                step.y = v[2].get<float>();
                break;
            case FixtureStep::Kind::Advance:
                if (!v.is_number() || v.get<double>() < 0.0)
                    return fail(error, where + ": seconds >= 0");
                step.seconds = v.get<double>();
                break;
            case FixtureStep::Kind::Capture:
                if (!v.is_string())
                    return fail(error, where + ": a file-name suffix");
                step.suffix = v.get<std::string>();
                break;
            case FixtureStep::Kind::SeekVideo:
                if (!v.is_array() || v.size() != 2 || targetId(v[0]).empty() || !v[1].is_number() || v[1].get<double>() < 0.0)
                    return fail(error, where + ": [\"#id\", seconds]");
                step.id = targetId(v[0]);
                step.seconds = v[1].get<double>();
                break;
            }
            sidecar.steps.push_back(std::move(step));
        }
    }catch (const std::exception& exception){
        return fail(error, path + ": " + exception.what());
    }
    return true;
}

// ---- directories and lists -----------------------------------------------------------------------

std::string jasperengine::test::htmluiTestSourceDirectory(){
    return JASPER_HTMLUI_TEST_SOURCE_DIR;
}

std::string jasperengine::test::browserFixturesDirectory(){
    return htmluiTestSourceDirectory() + "/fixtures/p3";
}

std::string jasperengine::test::taffyFixturesDirectory(){
    return htmluiTestSourceDirectory() + "/fixtures/taffy";
}

std::string jasperengine::test::browserReferenceDirectory(){
    return htmluiTestSourceDirectory() + "/golden/browser";
}

std::string jasperengine::test::browserArtifactDirectory(){
    return getOutputDirectory() + "/browser";
}

std::vector<BrowserFixture> jasperengine::test::listBrowserFixtures(){
    std::vector<BrowserFixture> fixtures;
    std::error_code ec;
    for (const fs::directory_entry& area : fs::directory_iterator(browserFixturesDirectory(), ec)){
        if (!area.is_directory())
            continue;
        for (const fs::directory_entry& file : fs::directory_iterator(area.path(), ec)){
            if (file.path().extension() != ".html")
                continue;
            fs::path sidecar = file.path();
            sidecar.replace_extension(".json");
            if (fs::exists(sidecar, ec))
                fixtures.push_back(BrowserFixture{area.path().filename().string(), file.path().stem().string()});
        }
    }
    std::sort(fixtures.begin(), fixtures.end(), [](const BrowserFixture& a, const BrowserFixture& b){ return a.id() < b.id(); });
    return fixtures;
}

std::vector<std::string> jasperengine::test::listTaffyFixtures(){
    std::vector<std::string> fixtures;
    std::error_code ec;
    for (const fs::directory_entry& suite : fs::directory_iterator(taffyFixturesDirectory(), ec)){
        if (!suite.is_directory())
            continue;
        for (const fs::directory_entry& file : fs::directory_iterator(suite.path(), ec)){
            if (file.path().extension() == ".html")
                fixtures.push_back(suite.path().filename().string() + "/" + file.path().stem().string());
        }
    }
    std::sort(fixtures.begin(), fixtures.end());
    return fixtures;
}

const ListedFixture* FixtureList::find(const std::string& id) const{
    for (const ListedFixture& entry : entries){
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

FixtureList jasperengine::test::loadFixtureList(const std::string& path){
    FixtureList list;
    std::string text;
    if (!readFile(path, text))
        return list;
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)){
        number++;
        ListedFixture entry;
        entry.line = number;
        const size_t hash = line.find('#');
        if (hash != std::string::npos){
            entry.note = line.substr(hash + 1);
            entry.note.erase(0, entry.note.find_first_not_of(' '));
            line.erase(hash);
        }
        std::istringstream words(line);
        std::string word;
        if (!(words >> entry.id))
            continue;
        while (words >> word){
            const size_t equals = word.find('=');
            const std::string key = word.substr(0, equals);
            const std::string value = equals == std::string::npos ? std::string() : word.substr(equals + 1);
            if (key == "owner" && !value.empty())
                entry.owner = value;
            else if (key == "reason" && !value.empty())
                entry.reason = value;
            else
                list.errors.push_back(path + ":" + std::to_string(number) + ": '" + word + "' is not owner=<package> or reason=<class>");
        }
        if (entry.reason.empty())
            list.errors.push_back(path + ":" + std::to_string(number) + ": " + entry.id + " has no reason=<class>");
        for (const ListedFixture& previous : list.entries){
            if (previous.id == entry.id)
                list.errors.push_back(path + ":" + std::to_string(number) + ": " + entry.id + " is listed twice");
        }
        list.entries.push_back(std::move(entry));
    }
    return list;
}

bool jasperengine::test::isPhase3Package(const std::string& name){
    // (and the phase-5 package whose fixtures are an area of this tree: property, p5-property)
    static const std::set<std::string> packages = {"p3-base", "p3-testkit", "p3-values", "p3-cascade", "p3-paint",
                                                    "p3-text", "p3-layout", "p3-animation", "p3-grid", "p3-integration",
                                                    "p5-property", "p6-video"};
    return packages.count(name) > 0;
}

// ---- Taffy references ---------------------------------------------------------------------------

std::string TaffyDifference::text() const{
    const std::string at = path.empty() ? std::string("#test-root") : "#test-root > " + path;
    if (field == 0)
        return at + ": " + detail;
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "%s %c: engine %.3f, Chromium %.3f (difference %.3f)", at.c_str(), field, engine, reference,
                  engine - reference);
    return buffer;
}

bool jasperengine::test::loadTaffySuite(const std::string& path, std::vector<std::pair<std::string, std::vector<TaffyBox>>>& fixtures,
                                        std::string* error){
    fixtures.clear();
    std::string text;
    if (!readFile(path, text))
        return fail(error, "cannot read " + path);
    try{
        const nlohmann::json json = nlohmann::json::parse(text);
        for (const auto& item : json.at("fixtures").items()){
            std::vector<TaffyBox> boxes;
            for (const nlohmann::json& entry : item.value()){
                TaffyBox box;
                box.path = entry.at(0).get<std::string>();
                box.tag = entry.at(1).get<std::string>();
                box.x = entry.at(2).get<float>();
                box.y = entry.at(3).get<float>();
                box.w = entry.at(4).get<float>();
                box.h = entry.at(5).get<float>();
                boxes.push_back(std::move(box));
            }
            fixtures.emplace_back(item.key(), std::move(boxes));
        }
    }catch (const std::exception& exception){
        return fail(error, path + ": " + exception.what());
    }
    return true;
}

std::vector<TaffyDifference> jasperengine::test::compareTaffyBoxes(const std::vector<TaffyBox>& engine, const std::vector<TaffyBox>& reference,
                                                                    float tolerance){
    std::vector<TaffyDifference> differences;
    for (const TaffyBox& expected : reference){
        const auto found = std::find_if(engine.begin(), engine.end(), [&](const TaffyBox& box){ return box.path == expected.path; });
        if (found == engine.end()){
            TaffyDifference difference;
            difference.path = expected.path;
            difference.detail = "no such element in the engine (a <" + expected.tag + ">)";
            differences.push_back(difference);
            continue;
        }
        if (found->tag != expected.tag){
            TaffyDifference difference;
            difference.path = expected.path;
            difference.detail = "the engine has a <" + found->tag + ">, Chromium a <" + expected.tag + ">";
            differences.push_back(difference);
            continue;
        }
        const std::pair<char, std::pair<float, float>> fields[] = {
            {'x', {found->x, expected.x}}, {'y', {found->y, expected.y}}, {'w', {found->w, expected.w}}, {'h', {found->h, expected.h}}};
        for (const auto& field : fields){
            if (std::fabs(field.second.first - field.second.second) > tolerance){
                TaffyDifference difference;
                difference.path = expected.path;
                difference.field = field.first;
                difference.engine = field.second.first;
                difference.reference = field.second.second;
                differences.push_back(difference);
            }
        }
    }
    for (const TaffyBox& box : engine){
        const bool known = std::any_of(reference.begin(), reference.end(), [&](const TaffyBox& expected){ return expected.path == box.path; });
        if (!known){
            TaffyDifference difference;
            difference.path = box.path;
            difference.detail = "the engine has an extra <" + box.tag + ">";
            differences.push_back(difference);
        }
    }
    return differences;
}

// ---- the runner -------------------------------------------------------------------------------------

struct FixtureRunner::State {
    std::unique_ptr<RenderTestContext> render;
    Rml::ElementDocument* document = nullptr;
    std::shared_ptr<const hc::CompatDocument> compiled;
    int width = 0, height = 0;
    int sampleCount = 4;      // the engine's default MSAA (htmlUiMsaaSamples)
    double time = 0.0;
    bool gpu = false;
    bool pointerDown = false;
    RmlFontEngine::FrameCounters text;   // of the last frame drawn
    FixtureRunner::PhaseTimes phases;     // of the last frame
};

FixtureRunner::FixtureRunner(int width, int height, int sampleCount): state(std::make_unique<State>()){
    state->sampleCount = sampleCount;
    state->width = width;
    state->height = height;
    state->gpu = GpuEnvironment::ensure();
    if (!state->gpu)
        return;
    // engine/tests/htmlui is the assets directory: the fixtures' relative font URLs reach fonts/
    TestSystem::get().setAssetPath(htmluiTestSourceDirectory());
    hc::ImportManifest::reload();
    HtmlUIClock::setFixedTime(0.0);
    state->render = std::make_unique<RenderTestContext>(width, height, width, height, state->sampleCount);
}

FixtureRunner::~FixtureRunner(){
    if (state->render){
        if (state->render->context)
            state->render->context->UnloadAllDocuments();
        Rml::Factory::ClearStyleSheetCache();
        state->render->destroy();
    }
    HtmlUIClock::reset();
    TestSystem::get().setAssetPath("");
    hc::ImportManifest::reload();
}

bool FixtureRunner::valid() const{
    return state->gpu && state->render && state->render->isValid();
}

int FixtureRunner::width() const{
    return state->width;
}

int FixtureRunner::height() const{
    return state->height;
}

double FixtureRunner::time() const{
    return state->time;
}

Rml::Context* FixtureRunner::context() const{
    return state->render ? state->render->context : nullptr;
}

RmlRenderer* FixtureRunner::renderer() const{
    return state->render ? &state->render->renderer : nullptr;
}

bool FixtureRunner::renderRequested() const{
    return state->render && state->render->renderRequested;
}

RmlFontEngine::FrameCounters FixtureRunner::textCounters() const{
    return state->text;
}

FixtureRunner::PhaseTimes FixtureRunner::phaseTimes() const{
    return state->phases;
}

Rml::ElementDocument* FixtureRunner::document() const{
    return state->document;
}

std::shared_ptr<const hc::CompatDocument> FixtureRunner::compiled() const{
    return state->compiled;
}

std::string FixtureRunner::diagnosticsText() const{
    if (!state->compiled)
        return std::string();
    std::vector<hc::Diagnostic> diagnostics = state->compiled->diagnostics;
    hc::sortDiagnostics(diagnostics);
    std::string text;
    for (const hc::Diagnostic& diagnostic : diagnostics)
        text += hc::toText(diagnostic) + "\n";
    return text;
}

// HtmlUIContext::update (+ render): the compat runtime before Context::Update, as each engine frame
void FixtureRunner::frame(bool draw, bool* drawn){
    RenderTestContext& r = *state->render;
    beginTestFrame();
    if (!r.target.isCreated())
        r.target.ensure(r.targetSize.x, r.targetSize.y, state->sampleCount);
    const bool begun = draw && r.renderer.beginFrame(r.target, r.contextSize);
    // as HtmlUIContext: every edge of backgrounds and borders feathered on a single-sampled target,
    // only curves on a multisampled one (phase-3 spec 3.7.3); the runner draws one target pixel per px
    Rml::SetGeometryFeathering(state->sampleCount <= 1);
    Rml::SetGeometryPixelSize(1.0f);
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b){ return std::chrono::duration<double, std::milli>(b - a).count(); };
    state->phases = PhaseTimes();
    const Clock::time_point updateStart = Clock::now();
    hc::updateRuntime(r.context);
    r.context->Update();
    state->phases.updateMs = ms(updateStart, Clock::now());
    // <video> frames chosen in the update (HtmlUIContext converts them before beginFrame)
    r.renderer.runVideoConversions();
    if (begun){
        // the font engine's counters of the frame, as HtmlUIContext::render takes them
        RmlFontEngine& fonts = HtmlUI::getFontEngine();
        fonts.beginContextFrame(r.context->GetRenderManager());
        const Clock::time_point renderStart = Clock::now();
        r.context->Render();
        state->phases.renderMs = ms(renderStart, Clock::now());
        state->text = fonts.takeContextFrame(r.context->GetRenderManager());
        r.renderer.addTextStats(state->text.glyphsRasterized, state->text.textRegenerations, state->text.textEffectFallbacks,
                                state->text.atlasEvictions);
    }
    // text settling at its on-screen scale asks for another frame (S16 R15)
    r.renderRequested = r.context->GetRenderManager().ConsumeRenderRequest();
    const Clock::time_point endStart = Clock::now();
    const bool result = draw && r.renderer.endFrame();
    state->phases.endFrameMs = ms(endStart, Clock::now());
    SystemRender::commit();
    if (drawn)
        *drawn = result;
}

void FixtureRunner::frameAt(double seconds, bool draw){
    if (!valid())
        return;
    state->time = std::max(state->time, seconds);
    HtmlUIClock::setFixedTime(state->time);
    frame(draw);
}

bool FixtureRunner::load(const std::string& assetPath, std::string* error){
    if (!valid())
        return fail(error, "no GPU or HtmlUI");
    Rml::Context* context = state->render->context;
    if (state->pointerDown){
        context->ProcessMouseButtonUp(0, 0);
        state->pointerDown = false;
    }
    context->ProcessMouseLeave();
    if (state->document){
        state->document->Close();
        state->document = nullptr;
        state->compiled.reset();
        context->Update();
    }
    hc::CompatOptions options;
    options.viewport = hc::CompatSize{state->width, state->height};
    options.dpRatio = 1.0f;
    state->time = 0.0;
    HtmlUIClock::setFixedTime(0.0);
    state->document = hc::DocumentLoader::load(context, assetPath, options, &state->compiled);
    if (!state->document)
        return fail(error, "DocumentLoader::load failed for " + assetPath);
    if (!state->document->IsVisible())
        state->document->Show();
    frame(false);
    return true;
}

Rml::Element* FixtureRunner::element(const std::string& id, std::string* error) const{
    Rml::Element* found = state->document ? state->document->GetElementById(id) : nullptr;
    // the id of <html> is kept as data-html-id on the document root (its id is reserved): "#id" names the root, as in Chromium
    if (!found && state->document && state->document->GetAttribute<Rml::String>("data-html-id", "") == id)
        found = state->document;
    if (!found && error)
        *error = "no element #" + id;
    return found;
}

// the pointer to the element's border-box centre, rounded as the reference script rounds it; on screen, through the
// transforms of the element and its ancestors (the reference takes getBoundingClientRect())
bool FixtureRunner::pointerTo(const std::string& id, std::string* error){
    Rml::Element* target = element(id, error);
    if (!target)
        return false;
    Rml::Rectanglef box;
    if (!Rml::ElementUtilities::GetBoundingBox(box, target, Rml::BoxArea::Border))
        box = Rml::Rectanglef::FromPositionSize(target->GetAbsoluteOffset(Rml::BoxArea::Border), target->GetBox().GetSize(Rml::BoxArea::Border));
    const Rml::Vector2f centre = box.Center();
    state->render->context->ProcessMouseMove(int(std::lround(centre.x)), int(std::lround(centre.y)), 0);
    return true;
}

bool FixtureRunner::step(const FixtureStep& step, std::string* error){
    if (!state->document)
        return fail(error, "no document");
    Rml::Context* context = state->render->context;
    using Kind = FixtureStep::Kind;
    switch (step.kind){
    case Kind::Hover:
        if (!pointerTo(step.id, error))
            return false;
        break;
    case Kind::Focus:
        if (step.keyboard){
            // keyboard navigation, as the reference presses Tab (:focus-visible applies)
            Rml::Element* target = element(step.id, error);
            if (!target)
                return false;
            bool reached = false;
            for (int i = 0; i < 64 && !reached; i++){
                context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
                context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
                frame(false);
                reached = context->GetFocusElement() == target;
            }
            if (!reached)
                return fail(error, "Tab navigation never reached #" + step.id);
        }else{
            if (!pointerTo(step.id, error))
                return false;
            context->ProcessMouseButtonDown(0, 0);
            context->ProcessMouseButtonUp(0, 0);
        }
        break;
    case Kind::Press:
        if (!pointerTo(step.id, error))
            return false;
        context->ProcessMouseButtonDown(0, 0);
        state->pointerDown = true;
        break;
    case Kind::Release:
        if (!step.id.empty() && !pointerTo(step.id, error))
            return false;
        context->ProcessMouseButtonUp(0, 0);
        state->pointerDown = false;
        break;
    case Kind::AddClass:
    case Kind::RemoveClass:
    case Kind::SetStyle:
    case Kind::SetAttr:
    case Kind::Scroll:{
        // through the element API, as a game script would
        Rml::Element* target = element(step.id, error);
        if (!target)
            return false;
        UIElement handle = UIElementAccess::wrap(target);
        if (step.kind == Kind::AddClass)
            handle.addClass(step.name);
        else if (step.kind == Kind::RemoveClass)
            handle.removeClass(step.name);
        else if (step.kind == Kind::SetStyle)
            handle.setStyle(step.name, step.remove ? std::string() : step.value);
        else if (step.kind == Kind::SetAttr && step.remove)
            handle.removeAttribute(step.name);
        else if (step.kind == Kind::SetAttr)
            handle.setAttribute(step.name, step.value);
        else{
            handle.setScrollLeft(step.x);
            handle.setScrollTop(step.y);
        }
        break;
    }
    case Kind::Advance:{
        // RmlUi steps an animation by at most 0.1 s per update: the clock moves in 1/20 s steps
        // and ends exactly on the target, as the reference seeks every animation to it
        const double end = state->time + step.seconds;
        while (state->time < end){
            state->time = std::min(end, state->time + 0.05);
            HtmlUIClock::setFixedTime(state->time);
            frame(false);
        }
        return true;
    }
    case Kind::Capture:
        return true;
    case Kind::SeekVideo:{
        // the UI clock is fixed: the element decodes the frame at once (frame-exact mode), and the
        // frames after the seek upload and convert it
        UIVideoElement* video = rmlui_dynamic_cast<UIVideoElement*>(element(step.id, error));
        if (!video)
            return fail(error, "#" + step.id + " is not a <video>");
        video->setCurrentTime(step.seconds);
        for (int i = 0; i < 4 && video->isSeeking(); i++)
            frame(false);
        if (video->isSeeking())
            return fail(error, "#" + step.id + " did not finish seeking to " + std::to_string(step.seconds));
        break;
    }
    }
    // the change is styled and laid out (transitions start) at the step's time
    frame(false);
    return true;
}

std::vector<compat::LayoutBox> FixtureRunner::idBoxes() const{
    std::vector<compat::LayoutBox> boxes;
    if (!state->document)
        return boxes;
    // (element, inside a 'display: none' subtree)
    std::vector<std::pair<Rml::Element*, bool>> stack = {{state->document, false}};
    while (!stack.empty()){
        Rml::Element* element = stack.back().first;
        // an element that is not displayed has no box: all zeros, as getBoundingClientRect() gives
        const bool hidden = stack.back().second || element->GetDisplay() == Rml::Style::Display::None;
        stack.pop_back();
        const std::string id = element->GetId();
        if (!id.empty() && element != state->document){
            compat::LayoutBox box;
            box.id = id;
            box.tag = element->GetTagName();
            if (!hidden){
                // the union of the element's boxes, as getBoundingClientRect: an inline element split over
                // several lines has one box per line (Element::GetBox(index, offset))
                const Rml::Vector2f origin = element->GetAbsoluteOffset(Rml::BoxArea::Border);
                Rml::Vector2f low = origin, high = origin + element->GetBox().GetSize(Rml::BoxArea::Border);
                for (int i = 1; i < element->GetNumBoxes(); i++){
                    Rml::Vector2f offset;
                    const Rml::Box& part = element->GetBox(i, offset);
                    const Rml::Vector2f partLow = origin + offset;
                    const Rml::Vector2f partHigh = partLow + part.GetSize(Rml::BoxArea::Border);
                    low = Rml::Vector2f(std::min(low.x, partLow.x), std::min(low.y, partLow.y));
                    high = Rml::Vector2f(std::max(high.x, partHigh.x), std::max(high.y, partHigh.y));
                }
                box.x = low.x;
                box.y = low.y;
                box.w = high.x - low.x;
                box.h = high.y - low.y;
            }
            boxes.push_back(std::move(box));
        }
        for (int i = element->GetNumChildren() - 1; i >= 0; i--)
            stack.push_back({element->GetChild(i), hidden});
    }
    return boxes;
}

namespace {

    // The fork's symbol markers (disc, circle, square, disclosure triangles: shapes, not glyphs) are text in CSS,
    // painted with the text's colour: a capture without text hides them, as the reference's transparent text fill
    // does (BrowserCompare's no-text render of the phase-2 golden pages does the same). An image marker stays.
    void showSymbolMarkers(Rml::Element* element, bool show){
        if (!element)
            return;
        if (Rml::Element* marker = element->GetPseudoElement(Rml::PseudoElementType::Marker)){
            const bool image = marker->GetNumChildren() > 0 && marker->GetChild(0)->GetTagName() == "img";
            if (show || image)
                marker->RemoveProperty(Rml::PropertyId::Visibility);
            else
                marker->SetProperty(Rml::PropertyId::Visibility, Rml::Property(Rml::Style::Visibility::Hidden));
        }
        for (int i = 0; i < element->GetNumChildren(); i++)
            showSymbolMarkers(element->GetChild(i), show);
    }

}

bool FixtureRunner::capture(Capture& capture, bool image, bool noText){
    capture.image = compat::Image();
    capture.noTextImage = compat::Image();
    auto read = [this](compat::Image& out){
        bool drawn = false;
        // the UI shader variants compile one per frame on the first frames; then one more frame,
        // so what the first drawn frame loaded (images, SVG rasters) is in place
        for (int i = 0; i < 40 && !drawn; i++)
            frame(true, &drawn);
        if (!drawn)
            return false;
        frame(true, &drawn);
        std::vector<uint8_t> rgba;
        if (!drawn || !state->render->readLayer(rgba))
            return false;
        out.width = state->render->targetSize.x;
        out.height = state->render->targetSize.y;
        out.rgba = std::move(rgba);
        return true;
    };
    // the reference's screenshots hide the text caret (`caret: 'hide'`); its blink here follows the wall clock,
    // not the fixture's: the focused text field draws none while the images are taken
    Rml::Element* caretElement = nullptr;
    if (image){
        Rml::Element* focus = state->render->context->GetFocusElement();
        const std::string tag = focus ? focus->GetTagName() : std::string();
        if ((tag == "input" || tag == "textarea") && focus->GetAttribute<Rml::String>("style", "").find("caret-color") == Rml::String::npos){
            caretElement = focus;
            caretElement->SetProperty("caret-color", "transparent");
        }
    }
    struct CaretRestore {
        Rml::Element* element;
        ~CaretRestore(){ if (element) element->RemoveProperty("caret-color"); }
    } caretRestore{caretElement};
    if (image && !read(capture.image))
        return false;
    capture.boxes = idBoxes();
    if (image && noText){
        // the same frame (the clock is fixed) without glyphs: the layout does not change
        HtmlUI::getFontEngine().setRenderTextForTesting(false);
        showSymbolMarkers(state->document, false);
        const bool drawn = read(capture.noTextImage);
        HtmlUI::getFontEngine().setRenderTextForTesting(true);
        showSymbolMarkers(state->document, true);
        frame(true, nullptr);
        if (!drawn)
            return false;
    }
    return true;
}

bool FixtureRunner::run(const FixtureSidecar& sidecar, std::vector<Capture>& captures, std::string* error, bool images){
    captures.clear();
    for (const FixtureStep& s : sidecar.steps){
        if (!step(s, error))
            return false;
        if (s.kind == FixtureStep::Kind::Capture){
            Capture capture;
            capture.suffix = s.suffix;
            if (!this->capture(capture, images, sidecar.noText))
                return fail(error, "capture '" + s.suffix + "' did not draw");
            captures.push_back(std::move(capture));
        }
    }
    if (captures.empty()){
        Capture capture;
        if (!this->capture(capture, images, sidecar.noText))
            return fail(error, "the capture did not draw");
        captures.push_back(std::move(capture));
    }
    return true;
}

bool FixtureRunner::taffyBoxes(std::vector<TaffyBox>& boxes, std::string* error) const{
    boxes.clear();
    Rml::Element* root = element("test-root", error);
    if (!root)
        return false;
    const Rml::Vector2f origin = root->GetAbsoluteOffset(Rml::BoxArea::Border);
    struct Walker {
        std::vector<TaffyBox>& out;
        Rml::Vector2f origin;
        Rml::Element* root;
        void walk(Rml::Element* element, const std::string& path){
            TaffyBox box;
            box.path = path;
            box.tag = element->GetTagName();
            const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            // #test-root in the viewport, the rest relative to it (the reference's convention)
            const Rml::Vector2f base = element == root ? Rml::Vector2f(0.0f, 0.0f) : origin;
            box.x = offset.x - base.x;
            box.y = offset.y - base.y;
            box.w = size.x;
            box.h = size.y;
            out.push_back(std::move(box));
            int index = 0;
            for (int i = 0; i < element->GetNumChildren(); i++){
                Rml::Element* child = element->GetChild(i);
                if (child->GetTagName() == "#text")
                    continue;
                walk(child, path.empty() ? std::to_string(index) : path + "." + std::to_string(index));
                index++;
            }
        }
    };
    Walker{boxes, origin, root}.walk(root, "");
    return true;
}
