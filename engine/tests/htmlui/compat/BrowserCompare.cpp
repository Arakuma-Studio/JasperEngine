// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BrowserCompare.h"

#include "TestRenderCommon.h"
#include "harness/GpuEnvironment.h"
#include "harness/ImageCompare.h"
#include "harness/Readback.h"
#include "harness/TestSystem.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"
#include "RmlFontEngine.h"

#include "json.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
namespace hc = jasperengine::htmlui::compat;

// ---- images ---------------------------------------------------------------------------------

Image jasperengine::test::compat::premultiplied(int width, int height, const std::vector<uint8_t>& straight){
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(straight.size());
    for (size_t i = 0; i + 3 < straight.size(); i += 4){
        const int a = straight[i + 3];
        for (int c = 0; c < 3; c++)
            image.rgba[i + c] = uint8_t((straight[i + c] * a + 127) / 255);
        image.rgba[i + 3] = uint8_t(a);
    }
    return image;
}

std::vector<uint8_t> jasperengine::test::compat::unpremultiplied(const Image& image){
    std::vector<uint8_t> straight(image.rgba.size());
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4){
        const int a = image.rgba[i + 3];
        for (int c = 0; c < 3; c++)
            straight[i + c] = a == 0 ? 0 : uint8_t(std::min(255, (image.rgba[i + c] * 255 + a / 2) / a));
        straight[i + 3] = uint8_t(a);
    }
    return straight;
}

bool jasperengine::test::compat::loadPng(const std::string& path, Image& image){
    std::vector<uint8_t> straight;
    int width = 0, height = 0;
    if (!readPng(path, straight, width, height)){
        image = Image();
        return false;
    }
    image = premultiplied(width, height, straight);
    return true;
}

bool jasperengine::test::compat::savePng(const std::string& path, const Image& image){
    return writePng(path, unpremultiplied(image), image.width, image.height);
}

bool ImageMetrics::passes(const ImageGate& gate) const{
    return comparable && fuzzyBadPercent <= gate.maxFuzzyBadPercent && maxBlock <= gate.maxBlock;
}

std::string ImageMetrics::text() const{
    if (!comparable)
        return error;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "fuzzy-bad %.3f %% (%zu pixels, threshold %d), max-block %.1f at (%d, %d)",
                  fuzzyBadPercent, fuzzyBadPixels, channelThreshold, maxBlock, maxBlockX, maxBlockY);
    return buffer;
}

namespace {

    // the pixels of `a` with no pixel of the 3x3 neighbourhood in `b` within the threshold
    void markFuzzyBad(const Image& a, const Image& b, int threshold, std::vector<uint8_t>& bad){
        const int w = a.width, h = a.height;
        for (int y = 0; y < h; y++){
            for (int x = 0; x < w; x++){
                const uint8_t* p = &a.rgba[(size_t(y) * w + x) * 4];
                bool matched = false;
                for (int dy = -1; dy <= 1 && !matched; dy++){
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h)
                        continue;
                    for (int dx = -1; dx <= 1 && !matched; dx++){
                        const int xx = x + dx;
                        if (xx < 0 || xx >= w)
                            continue;
                        const uint8_t* q = &b.rgba[(size_t(yy) * w + xx) * 4];
                        matched = std::abs(p[0] - q[0]) <= threshold && std::abs(p[1] - q[1]) <= threshold &&
                                  std::abs(p[2] - q[2]) <= threshold && std::abs(p[3] - q[3]) <= threshold;
                    }
                }
                if (!matched)
                    bad[size_t(y) * w + x] = 1;
            }
        }
    }

    // the largest difference of block means over channels, for one block
    double blockDifference(const Image& a, const Image& b, int bx, int by, int size){
        double worst = 0.0;
        for (int c = 0; c < 4; c++){
            double sa = 0.0, sb = 0.0;
            for (int y = by; y < by + size; y++){
                for (int x = bx; x < bx + size; x++){
                    sa += a.rgba[(size_t(y) * a.width + x) * 4 + c];
                    sb += b.rgba[(size_t(y) * b.width + x) * 4 + c];
                }
            }
            worst = std::max(worst, std::fabs(sa - sb) / double(size * size));
        }
        return worst;
    }

    constexpr int kBlock = 8;

}

ImageMetrics jasperengine::test::compat::compareImages(const Image& actual, const Image& expected, int channelThreshold){
    ImageMetrics metrics;
    metrics.channelThreshold = channelThreshold;
    if (actual.empty() || expected.empty()){
        metrics.error = actual.empty() ? "no engine image" : "no reference image";
        return metrics;
    }
    if (actual.width != expected.width || actual.height != expected.height){
        metrics.error = "size " + std::to_string(actual.width) + "x" + std::to_string(actual.height) + " differs from the reference " +
                        std::to_string(expected.width) + "x" + std::to_string(expected.height);
        return metrics;
    }
    metrics.comparable = true;

    std::vector<uint8_t> bad(size_t(actual.width) * actual.height, 0);
    markFuzzyBad(actual, expected, channelThreshold, bad);
    markFuzzyBad(expected, actual, channelThreshold, bad);
    for (uint8_t flag : bad)
        metrics.fuzzyBadPixels += flag;
    metrics.fuzzyBadPercent = 100.0 * double(metrics.fuzzyBadPixels) / double(bad.size());

    for (int by = 0; by + kBlock <= actual.height; by += kBlock){
        for (int bx = 0; bx + kBlock <= actual.width; bx += kBlock){
            const double d = blockDifference(actual, expected, bx, by, kBlock);
            if (d > metrics.maxBlock){
                metrics.maxBlock = d;
                metrics.maxBlockX = bx;
                metrics.maxBlockY = by;
            }
        }
    }
    return metrics;
}

Image jasperengine::test::compat::heatMap(const Image& actual, const Image& expected, int channelThreshold){
    Image out;
    if (actual.empty() || expected.empty() || actual.width != expected.width || actual.height != expected.height)
        return out;
    out.width = actual.width;
    out.height = actual.height;
    out.rgba.assign(actual.rgba.size(), 255);

    std::vector<uint8_t> bad(size_t(actual.width) * actual.height, 0);
    markFuzzyBad(actual, expected, channelThreshold, bad);
    markFuzzyBad(expected, actual, channelThreshold, bad);

    for (size_t i = 0; i < bad.size(); i++){
        const uint8_t* e = &expected.rgba[i * 4];
        const uint8_t* a = &actual.rgba[i * 4];
        uint8_t* o = &out.rgba[i * 4];
        const int gray = int((0.299 * e[0] + 0.587 * e[1] + 0.114 * e[2]) * 0.3);
        if (bad[i]){
            int d = 0;
            for (int c = 0; c < 4; c++)
                d = std::max(d, std::abs(a[c] - e[c]));
            o[0] = uint8_t(std::min(255, 96 + d * 159 / 255));
            o[1] = uint8_t(gray / 2);
            o[2] = uint8_t(gray / 2);
        }else{
            o[0] = o[1] = o[2] = uint8_t(gray);
        }
    }

    // the worst block, outlined
    const ImageMetrics metrics = compareImages(actual, expected, channelThreshold);
    if (metrics.maxBlock > 0.0){
        for (int k = -1; k <= kBlock; k++){
            const int points[4][2] = {{metrics.maxBlockX + k, metrics.maxBlockY - 1}, {metrics.maxBlockX + k, metrics.maxBlockY + kBlock},
                                      {metrics.maxBlockX - 1, metrics.maxBlockY + k}, {metrics.maxBlockX + kBlock, metrics.maxBlockY + k}};
            for (const auto& p : points){
                if (p[0] < 0 || p[1] < 0 || p[0] >= out.width || p[1] >= out.height)
                    continue;
                uint8_t* o = &out.rgba[(size_t(p[1]) * out.width + p[0]) * 4];
                o[0] = 255;
                o[1] = 230;
                o[2] = 0;
            }
        }
    }
    return out;
}

// ---- the strict metric (phase 3) ----------------------------------------------------------------

namespace {

    // the strict bad pixels of `a` against `b`: off by more than the threshold at the same place
    // and with no pixel of the 3x3 neighbourhood in `b` within it
    void markStrictBad(const Image& a, const Image& b, int threshold, std::vector<uint8_t>& bad){
        const int w = a.width, h = a.height;
        auto within = [threshold](const uint8_t* p, const uint8_t* q){
            return std::abs(p[0] - q[0]) <= threshold && std::abs(p[1] - q[1]) <= threshold &&
                   std::abs(p[2] - q[2]) <= threshold && std::abs(p[3] - q[3]) <= threshold;
        };
        for (int y = 0; y < h; y++){
            for (int x = 0; x < w; x++){
                const size_t index = size_t(y) * w + x;
                const uint8_t* p = &a.rgba[index * 4];
                if (within(p, &b.rgba[index * 4]))
                    continue;
                bool matched = false;
                for (int dy = -1; dy <= 1 && !matched; dy++){
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h)
                        continue;
                    for (int dx = -1; dx <= 1 && !matched; dx++){
                        const int xx = x + dx;
                        if (xx >= 0 && xx < w)
                            matched = within(p, &b.rgba[(size_t(yy) * w + xx) * 4]);
                    }
                }
                if (!matched)
                    bad[index] = 1;
            }
        }
    }

    std::vector<uint8_t> strictBadPixels(const Image& actual, const Image& expected, int threshold){
        std::vector<uint8_t> bad(size_t(actual.width) * actual.height, 0);
        markStrictBad(actual, expected, threshold, bad);
        markStrictBad(expected, actual, threshold, bad);
        return bad;
    }

}

bool StrictMetrics::passes() const{
    return comparable && badPercent <= gate.maxBadPercent && largestCluster <= gate.maxCluster;
}

std::string StrictMetrics::text() const{
    if (!comparable)
        return error;
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "bad %.3f %% (%zu pixels, threshold %d; limit %.3f %%), largest cluster %zu pixels (limit %zu) at (%d, %d) in [%d,%d]-[%d,%d]",
                  badPercent, badPixels, gate.channelThreshold, gate.maxBadPercent, largestCluster, gate.maxCluster, clusterX, clusterY,
                  clusterMinX, clusterMinY, clusterMaxX, clusterMaxY);
    return buffer;
}

StrictMetrics jasperengine::test::compat::compareStrict(const Image& actual, const Image& expected, const StrictGate& gate){
    StrictMetrics metrics;
    metrics.gate = gate;
    if (actual.empty() || expected.empty()){
        metrics.error = actual.empty() ? "no engine image" : "no reference image";
        return metrics;
    }
    if (actual.width != expected.width || actual.height != expected.height){
        metrics.error = "size " + std::to_string(actual.width) + "x" + std::to_string(actual.height) + " differs from the reference " +
                        std::to_string(expected.width) + "x" + std::to_string(expected.height);
        return metrics;
    }
    metrics.comparable = true;
    std::vector<uint8_t> bad = strictBadPixels(actual, expected, gate.channelThreshold);
    for (uint8_t flag : bad)
        metrics.badPixels += flag;
    metrics.badPercent = 100.0 * double(metrics.badPixels) / double(bad.size());

    // 4-connected clusters (flood fill; a visited pixel is cleared)
    const int w = actual.width, h = actual.height;
    std::vector<size_t> stack;
    for (size_t start = 0; start < bad.size(); start++){
        if (!bad[start])
            continue;
        size_t size = 0;
        int minX = w, minY = h, maxX = -1, maxY = -1;
        bad[start] = 0;
        stack.push_back(start);
        while (!stack.empty()){
            const size_t index = stack.back();
            stack.pop_back();
            size++;
            const int x = int(index % w), y = int(index / w);
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
            const int neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& n : neighbours){
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                    continue;
                const size_t next = size_t(n[1]) * w + n[0];
                if (bad[next]){
                    bad[next] = 0;
                    stack.push_back(next);
                }
            }
        }
        if (size > metrics.largestCluster){
            metrics.largestCluster = size;
            metrics.clusterX = int(start % w);
            metrics.clusterY = int(start / w);
            metrics.clusterMinX = minX;
            metrics.clusterMinY = minY;
            metrics.clusterMaxX = maxX;
            metrics.clusterMaxY = maxY;
        }
    }
    return metrics;
}

Image jasperengine::test::compat::strictHeatMap(const Image& actual, const Image& expected, const StrictGate& gate){
    Image out;
    if (actual.empty() || expected.empty() || actual.width != expected.width || actual.height != expected.height)
        return out;
    out.width = actual.width;
    out.height = actual.height;
    out.rgba.assign(actual.rgba.size(), 255);
    const std::vector<uint8_t> bad = strictBadPixels(actual, expected, gate.channelThreshold);
    for (size_t i = 0; i < bad.size(); i++){
        const uint8_t* e = &expected.rgba[i * 4];
        const uint8_t* a = &actual.rgba[i * 4];
        uint8_t* o = &out.rgba[i * 4];
        const int gray = int((0.299 * e[0] + 0.587 * e[1] + 0.114 * e[2]) * 0.3);
        if (bad[i]){
            int d = 0;
            for (int c = 0; c < 4; c++)
                d = std::max(d, std::abs(a[c] - e[c]));
            o[0] = uint8_t(std::min(255, 96 + d * 159 / 255));
            o[1] = uint8_t(gray / 2);
            o[2] = uint8_t(gray / 2);
        }else{
            o[0] = o[1] = o[2] = uint8_t(gray);
        }
    }
    const StrictMetrics metrics = compareStrict(actual, expected, gate);
    if (metrics.largestCluster > 0){
        auto mark = [&](int x, int y){
            if (x < 0 || y < 0 || x >= out.width || y >= out.height)
                return;
            uint8_t* o = &out.rgba[(size_t(y) * out.width + x) * 4];
            o[0] = 255;
            o[1] = 230;
            o[2] = 0;
        };
        for (int x = metrics.clusterMinX - 1; x <= metrics.clusterMaxX + 1; x++){
            mark(x, metrics.clusterMinY - 1);
            mark(x, metrics.clusterMaxY + 1);
        }
        for (int y = metrics.clusterMinY - 1; y <= metrics.clusterMaxY + 1; y++){
            mark(metrics.clusterMinX - 1, y);
            mark(metrics.clusterMaxX + 1, y);
        }
    }
    return out;
}

// ---- layout -------------------------------------------------------------------------------------

bool jasperengine::test::compat::loadReferenceLayout(const std::string& path, ReferenceLayout& layout, std::string* error){
    layout = ReferenceLayout();
    std::ifstream file(path, std::ios::binary);
    if (!file){
        if (error)
            *error = "cannot read " + path;
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    try{
        const nlohmann::json json = nlohmann::json::parse(stream.str());
        layout.page = json.value("page", "");
        if (json.contains("viewport") && json["viewport"].is_array() && json["viewport"].size() == 2){
            layout.viewportWidth = json["viewport"][0].get<int>();
            layout.viewportHeight = json["viewport"][1].get<int>();
        }
        if (json.contains("states")){
            const nlohmann::json& states = json["states"];
            layout.hover = states.value("hover", "");
            layout.focus = states.value("focus", "");
            layout.active = states.value("active", "");
        }
        std::string requires = json.value("requires", "");
        std::replace(requires.begin(), requires.end(), ',', ' ');
        std::istringstream words(requires);
        for (std::string word; words >> word;)
            layout.requires.push_back(word);
        for (const nlohmann::json& element : json.at("elements")){
            LayoutBox box;
            box.id = element.at("id").get<std::string>();
            box.tag = element.value("tag", "");
            box.x = element.at("x").get<float>();
            box.y = element.at("y").get<float>();
            box.w = element.at("w").get<float>();
            box.h = element.at("h").get<float>();
            box.tolerance = element.value("tol", "");
            box.transformed = element.value("transformed", false);
            layout.elements.push_back(std::move(box));
        }
    }catch (const std::exception& exception){
        if (error)
            *error = path + ": " + exception.what();
        return false;
    }
    return true;
}

std::string LayoutDifference::text() const{
    if (field == 0)
        return "#" + id + ": no such element in the engine document";
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "#%s %c: engine %.2f, Chromium %.2f (difference %.2f, tolerance %.2f)", id.c_str(), field,
                  engine, reference, engine - reference, tolerance);
    return buffer;
}

float jasperengine::test::compat::layoutTolerance(const LayoutBox& reference, char field){
    // a box CSS sizes to its content where RmlUi fills the container (an auto-width table: RmlUi's
    // table model, kept by phase 3 §2.2; a fieldset's legend): its width is not compared, its
    // other edges are text-sized
    if (reference.tolerance == "auto-width" && field == 'w')
        return std::numeric_limits<float>::infinity();
    if (reference.tolerance == "text" || reference.tolerance == "auto-width"){
        const float extent = (field == 'x' || field == 'w') ? reference.w : reference.h;
        return std::max(6.0f, 0.04f * extent);
    }
    return 3.0f;
}

std::vector<LayoutDifference> jasperengine::test::compat::compareLayout(const std::vector<LayoutBox>& engine, const ReferenceLayout& reference){
    std::vector<LayoutDifference> differences;
    for (const LayoutBox& expected : reference.elements){
        if (expected.transformed)
            continue;
        const auto found = std::find_if(engine.begin(), engine.end(), [&](const LayoutBox& box){ return box.id == expected.id; });
        if (found == engine.end()){
            LayoutDifference missing;
            missing.id = expected.id;
            differences.push_back(missing);
            continue;
        }
        const std::pair<char, std::pair<float, float>> fields[] = {
            {'x', {found->x, expected.x}}, {'y', {found->y, expected.y}}, {'w', {found->w, expected.w}}, {'h', {found->h, expected.h}}};
        for (const auto& field : fields){
            const float tolerance = layoutTolerance(expected, field.first);
            if (std::fabs(field.second.first - field.second.second) > tolerance){
                LayoutDifference difference;
                difference.id = expected.id;
                difference.field = field.first;
                difference.engine = field.second.first;
                difference.reference = field.second.second;
                difference.tolerance = tolerance;
                differences.push_back(difference);
            }
        }
    }
    return differences;
}

std::vector<std::string> jasperengine::test::compat::missingForkFeatures(const std::vector<std::string>& requires, const hc::ForkFeatures& fork){
    std::vector<std::string> missing;
    for (const std::string& name : requires){
        // "pseudo-elements" -> PseudoElements
        std::string camel;
        bool upper = true;
        for (char c : name){
            if (c == '-'){
                upper = true;
                continue;
            }
            camel += upper ? char(std::toupper((unsigned char)c)) : c;
            upper = false;
        }
        const hc::ForkFeature feature = hc::findForkFeature(camel);
        if (feature == hc::ForkFeature::None || !fork.has(feature))
            missing.push_back(name);
    }
    return missing;
}

std::string jasperengine::test::compat::pagesDirectory(){
    return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/compat/pages";
}

std::string jasperengine::test::compat::referenceDirectory(){
    return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/golden/compat/reference";
}

std::string jasperengine::test::compat::engineGoldenDirectory(){
    return std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/golden/compat/engine";
}

std::string jasperengine::test::compat::artifactDirectory(){
    return getOutputDirectory() + "/compat";
}

// ---- the engine side ------------------------------------------------------------------------

struct BrowserPage::State {
    Options options;
    std::unique_ptr<RenderTestContext> render;
    Rml::ElementDocument* document = nullptr;
    std::shared_ptr<const hc::CompatDocument> compiled;
    bool gpu = false;
};

BrowserPage::BrowserPage(): BrowserPage(Options()) {}

BrowserPage::BrowserPage(const Options& options): state(std::make_unique<State>()){
    state->options = options;
    state->gpu = GpuEnvironment::ensure();
    if (!state->gpu)
        return;
    // the pages directory is the project's assets directory: its htmlui-imports/manifest.json
    // maps the pages' Google Fonts URL to the committed fonts, as in a project
    TestSystem::get().setAssetPath(pagesDirectory());
    hc::ImportManifest::reload();
    HtmlUIClock::setFixedTime(0.0);
    state->render = std::make_unique<RenderTestContext>(options.width, options.height, options.width, options.height, options.sampleCount);
}

BrowserPage::~BrowserPage(){
    if (state->render){
        HtmlUI::getFontEngine().setRenderTextForTesting(true);
        // the cache holds the pages' sheets by path: dropped while RmlUi is still up (destroy
        // releases HtmlUI, which may shut RmlUi down)
        if (state->render->context)
            state->render->context->UnloadAllDocuments();
        Rml::Factory::ClearStyleSheetCache();
        state->render->destroy();
    }
    HtmlUIClock::reset();
    TestSystem::get().setAssetPath("");
    hc::ImportManifest::reload();
}

bool BrowserPage::valid() const{
    return state->gpu && state->render && state->render->isValid();
}

Rml::ElementDocument* BrowserPage::document() const{
    return state->document;
}

Rml::Context* BrowserPage::context() const{
    return state->render ? state->render->context : nullptr;
}

std::shared_ptr<const hc::CompatDocument> BrowserPage::compiled() const{
    return state->compiled;
}

// HtmlUIContext::update + render: the compat runtime (focus tracking, animation fill modes)
// before Context::Update, as the engine does each frame
bool BrowserPage::frame(bool draw){
    RenderTestContext& r = *state->render;
    beginTestFrame();
    if (!r.target.isCreated())
        r.target.ensure(r.targetSize.x, r.targetSize.y, state->options.sampleCount);
    const bool begun = draw && r.renderer.beginFrame(r.target, r.contextSize);
    hc::updateRuntime(r.context);
    r.context->Update();
    if (begun)
        r.context->Render();
    const bool drawn = draw && r.renderer.endFrame();
    SystemRender::commit();
    return drawn;
}

// RmlUi advances an animation by at most 0.1 s per update (ElementAnimation.cpp), so the
// fixed UI clock steps by 0.1 s: 30 s of updates end every finite animation of the pages, and
// the compat runtime applies fill modes on animationend, as the reference script's
// Animation.finish()
void BrowserPage::settleAnimations(){
    const double start = HtmlUIClock::now();
    for (int step = 1; step <= 300; step++){
        HtmlUIClock::setFixedTime(start + 0.1 * step);
        frame(false);
    }
}

bool BrowserPage::load(const std::string& page, std::string* error){
    if (!valid()){
        if (error)
            *error = "no GPU or HtmlUI";
        return false;
    }
    hc::CompatOptions options;
    options.fork = state->options.fork;
    options.viewport = hc::CompatSize{state->options.width, state->options.height};
    options.dpRatio = 1.0f;
    options.dropTransitionsForTesting = true;   // the reference script disables transitions

    HtmlUIClock::setFixedTime(0.0);
    state->document = hc::DocumentLoader::load(state->render->context, page, options, &state->compiled);
    if (!state->document){
        if (error)
            *error = "DocumentLoader::load failed for " + page;
        return false;
    }
    if (!state->document->IsVisible())
        state->document->Show();
    settleAnimations();
    return true;
}

namespace {

    Rml::Vector2f centre(Rml::Element* element){
        const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        return offset + size * 0.5f;
    }

}

bool BrowserPage::hover(const std::string& id){
    Rml::Element* element = state->document ? state->document->GetElementById(id) : nullptr;
    if (!element)
        return false;
    const Rml::Vector2f point = centre(element);
    state->render->context->ProcessMouseMove(int(std::lround(point.x)), int(std::lround(point.y)), 0);
    frame(false);
    return true;
}

bool BrowserPage::press(const std::string& id){
    if (!hover(id))
        return false;
    state->render->context->ProcessMouseButtonDown(0, 0);
    frame(false);
    return true;
}

bool BrowserPage::focusByKeyboard(const std::string& id, int maxTabs){
    Rml::Element* element = state->document ? state->document->GetElementById(id) : nullptr;
    if (!element)
        return false;
    for (int i = 0; i < maxTabs; i++){
        state->render->context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
        state->render->context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
        frame(false);
        if (state->render->context->GetFocusElement() == element)
            return true;
    }
    return false;
}

bool BrowserPage::applyStates(const ReferenceLayout& reference, std::string* error){
    // the reference script's order: focus, then hover, then active
    if (!reference.focus.empty() && !focusByKeyboard(reference.focus.substr(reference.focus[0] == '#' ? 1 : 0))){
        if (error)
            *error = "Tab navigation never reached " + reference.focus;
        return false;
    }
    if (!reference.hover.empty() && !hover(reference.hover.substr(reference.hover[0] == '#' ? 1 : 0))){
        if (error)
            *error = "no element " + reference.hover + " to hover";
        return false;
    }
    if (!reference.active.empty() && !press(reference.active.substr(reference.active[0] == '#' ? 1 : 0))){
        if (error)
            *error = "no element " + reference.active + " to press";
        return false;
    }
    return true;
}

bool BrowserPage::render(Image& image, bool withText){
    image = Image();
    if (!valid())
        return false;
    HtmlUI::getFontEngine().setRenderTextForTesting(withText);
    bool drawn = false;
    // the UI shader variants compile one per frame on the first frames
    for (int i = 0; i < 40 && !drawn; i++)
        drawn = frame(true);
    // one more, so what the first drawn frame loaded (images, SVG rasters) is in place
    drawn = frame(true) && drawn;
    std::vector<uint8_t> rgba;
    if (!state->render->readLayer(rgba))
        return false;
    image.width = state->render->targetSize.x;
    image.height = state->render->targetSize.y;
    image.rgba = std::move(rgba);
    return drawn;
}

std::vector<LayoutBox> BrowserPage::layout(const ReferenceLayout& reference) const{
    std::vector<LayoutBox> boxes;
    if (!state->document)
        return boxes;
    for (const LayoutBox& expected : reference.elements){
        Rml::Element* element = state->document->GetElementById(expected.id);
        if (!element)
            continue;
        LayoutBox box;
        box.id = expected.id;
        box.tag = element->GetTagName();
        const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        box.x = offset.x;
        box.y = offset.y;
        box.w = size.x;
        box.h = size.y;
        // a table's <caption> is laid out right before it (compat: RmlUi's tables hold rows
        // only): Chromium's table rect includes the caption
        Rml::Element* previous = element->GetPreviousSibling();
        while (previous && previous->GetTagName() == "#text")
            previous = previous->GetPreviousSibling();
        if (box.tag == "table" && previous && previous->GetTagName() == "caption"){
            const Rml::Vector2f captionOffset = previous->GetAbsoluteOffset(Rml::BoxArea::Border);
            const float top = std::min(box.y, captionOffset.y);
            box.h = box.y + box.h - top;
            box.y = top;
        }
        boxes.push_back(std::move(box));
    }
    return boxes;
}

std::string BrowserPage::diagnosticsText() const{
    if (!state->compiled)
        return std::string();
    std::vector<hc::Diagnostic> diagnostics = state->compiled->diagnostics;
    hc::sortDiagnostics(diagnostics);
    std::string text;
    for (const hc::Diagnostic& diagnostic : diagnostics)
        text += hc::toText(diagnostic) + "\n";
    return text;
}
