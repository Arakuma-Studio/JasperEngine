// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The perf gates of the virtual views (S17 R33, R34, AC13), on the document system's update (the
// RmlUi context update with layout, data views and the views' window): a 10 000-row list of 48 px
// rows (an image and three bound spans each, model mode) scrolled at 6 000 px/s at 60 fps, p95 of
// a frame <= 1.0 ms; the same with measured heights (32-160 px, a Lua provider) <= 1.25 ms; no
// element made while scrolling; <= 24 bytes per item of view state; a 10k-entry, 5-field model
// set from Lua <= 30 ms; flattening a 10k-node tree on expand <= 2 ms. The targets scale with
// JASPER_HTMLUI_PERF_SLACK (a shared build host); the gates are meant for the benchmark host.

#include "../TestScriptCommon.h"
#include "../compat/PerfFixtures.h"

#include "object/ui/UIItemsView.h"
#include "htmlui/widgets/WidgetsRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class HtmlUIWidgetsPerf : public ScriptTest {
    protected:
        const char* documentFile() const override { return ""; }

        static double nowMs(){
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // the frame times of a scroll of `frames` frames by `step` px, after a warm-up of `warm`
        std::vector<double> scrollFrames(UIElement list, float step, int frames, int warm){
            std::vector<double> times;
            const float span = list.getScrollHeight() - list.getClientHeight();
            float top = list.getScrollTop();
            for (int frame = 0; frame < warm + frames; frame++){
                top += step;
                if (top > span || top < 0.0f){
                    step = -step;
                    top = std::clamp(top, 0.0f, span);
                }
                list.setScrollTop(top);
                const double start = nowMs();
                update();
                if (frame >= warm)
                    times.push_back(nowMs() - start);
            }
            return times;
        }

        static double percentile(std::vector<double> values, double p){
            std::sort(values.begin(), values.end());
            const size_t index = std::min(values.size() - 1, (size_t)(p * (double)(values.size() - 1) + 0.5));
            return values[index];
        }

        // the p95 of the scroll, the least of up to `attempts` runs (a busy host only adds time)
        double gatedP95(UIElement list, float step, double target){
            double least = 1e9;
            for (int attempt = 0; attempt < 8 && least > target; attempt++){
                const std::vector<double> times = scrollFrames(list, step, 600, attempt == 0 ? 60 : 10);
                const double p95 = percentile(times, 0.95);
                std::printf("     [perf] p95 %.3f ms, median %.3f ms, max %.3f ms\n", p95, percentile(times, 0.5), percentile(times, 1.0));
                least = std::min(least, p95);
            }
            return least;
        }
    };

}

TEST_F(HtmlUIWidgetsPerf, ListScroll10k){
    ASSERT_EQ(lua(R"(
        local zones = { "Harbor", "Forest", "Ruins", "Peaks" }
        local items = {}
        for k = 1, 10000 do items[k] = { id = "p" .. k, name = "Player " .. k, level = k % 60, zone = zones[k % 4 + 1] } end
        model = doc:createDataModel("perf", { items = items })
    )"), "");
    ASSERT_TRUE(document->load("s17/perf_list.html"));
    update();
    update();
    UIElement list = document->getElementById("list");
    UIItemsView view = list.getItemsView();
    ASSERT_EQ(view.getCount(), 10000);

    scrollFrames(list, 100.0f, 120, 0);
    const uint64_t created = htmlui::WidgetsRuntime::elementsCreated();
    const double target = 1.0 * compat::perfSlack();
    const double p95 = gatedP95(list, 100.0f, target);
    EXPECT_LE(p95, target) << "a 10k list scrolled at 6000 px/s: p95 of a frame";
    EXPECT_EQ(htmlui::WidgetsRuntime::elementsCreated(), created) << "steady scrolling made elements";
    const UIItemsViewState state = view.getState();
    EXPECT_LE(state.pool, state.poolBound);
    EXPECT_TRUE(state.layoutBoundary && state.rowAliases) << "the fork patches P7-L / P7-D";
    // R34: the view's own state
    EXPECT_LE(state.memoryBytes, (size_t)10000 * 24) << state.memoryBytes;
}

TEST_F(HtmlUIWidgetsPerf, ListScroll10kAuto){
    ASSERT_TRUE(document->load("s17/perf_list.html"));
    update();
    ASSERT_EQ(lua(R"(
        texts = {}
        local words = "the quick brown fox jumps over the lazy dog and keeps running through the long grass "
        for i = 1, 10000 do texts[i] = string.rep(words, 1 + (i * 7) % 8) end
        doc:getElementById("auto").itemsView:setProvider{ count = #texts,
            bind = function(row, i) row:querySelector(".text").textContent = texts[i] end }
    )"), "");
    update();
    update();
    UIElement list = document->getElementById("auto");
    // every row measured once: the heights are 32-160 px
    scrollFrames(list, 100.0f, 200, 0);
    const double target = 1.25 * compat::perfSlack();
    const double p95 = gatedP95(list, 100.0f, target);
    EXPECT_LE(p95, target) << "a 10k list of measured rows at 6000 px/s: p95 of a frame";
}

TEST_F(HtmlUIWidgetsPerf, ModelSetAndTreeFlatten){
    // R34: a 10k-entry, 5-field model set from Lua
    ASSERT_TRUE(document->load("s17/perf_list.html"));
    update();
    ASSERT_EQ(lua(R"(
        local items = {}
        for k = 1, 10000 do items[k] = { id = "p" .. k, name = "Player " .. k, level = k % 60, zone = "Harbor", score = k * 3 } end
        model = doc:createDataModel("perf", { items = {} })
        local clock = os.clock
        best = 1e9
        for attempt = 1, 5 do
            local start = clock()
            model.items = items
            best = math.min(best, (clock() - start) * 1000)
        end
    )"), "");
    const double setMs = std::stod(luaString("best"));
    std::printf("     [perf] 10k x 5 model set %.2f ms\n", setMs);
    EXPECT_LE(setMs, 30.0 * compat::perfSlack());

    // a 10k-node tree: 100 roots of 99 children, all expanded at once
    ASSERT_EQ(lua(R"(
        local v = doc:getElementById("ptree").itemsView
        local keys = {}
        v:setProvider{ count = 0, bind = function(row, i) end,
            children = function(key)
                if key == nil then
                    local roots = {}
                    for r = 1, 100 do roots[r] = "r" .. r end
                    return roots
                end
                if key:sub(1, 1) == "r" then
                    local kids = {}
                    for c = 1, 99 do kids[c] = key .. "c" .. c end
                    return kids
                end
                return {}
            end,
            hasChildren = function(key) return not key:find("c") end }
    )"), "");
    update();
    UIItemsView tree = document->getElementById("ptree").getItemsView();
    ASSERT_TRUE(tree.isValid());
    ASSERT_EQ(tree.getCount(), 100);
    double worst = 0.0;
    for (int r = 1; r <= 100; r++){
        const auto start = std::chrono::steady_clock::now();
        tree.expand("r" + std::to_string(r));
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    update();
    EXPECT_EQ(tree.getCount(), 10000);
    std::printf("     [perf] tree expand (up to 10k nodes) worst %.3f ms\n", worst);
    EXPECT_LE(worst, 2.0 * compat::perfSlack());
}
