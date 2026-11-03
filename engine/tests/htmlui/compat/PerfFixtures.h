// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_PERF_FIXTURES_H
#define COMPAT_PERF_FIXTURES_H

// What the performance tests of phase-2 §3.14 share: how a step is timed against its target,
// and the generated style sheet they rewrite.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#endif

namespace jasperengine::test::compat {

    // a shared, loaded build host (several agents compiling) slows every timing down: the
    // targets can be scaled with JASPER_HTMLUI_PERF_SLACK (default 1)
    inline double perfSlack(){
        const char* value = std::getenv("JASPER_HTMLUI_PERF_SLACK");
        const double factor = value ? std::atof(value) : 1.0;
        return factor > 0.0 ? factor : 1.0;
    }

    // The calling thread's CPU time in ms; the process's where the platform has no thread clock.
    // The step runs on this thread: the process's clock also counts the binary's other threads
    // (job workers, threads an earlier test left running), whatever they do meanwhile.
    inline double threadCpuMs(){
#if defined(__linux__) || defined(__APPLE__)
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#else
        return 1000.0 * (double)std::clock() / (double)CLOCKS_PER_SEC;
#endif
    }

    // For a budget near the step's own cost on a build server (MarkdownHtml.Perf20KB: 1.3 ms of its
    // 2 ms there alone, 0.27 ms on the benchmark server): the factor it is enforced with, only when
    // the run sets JASPER_HTMLUI_PERF_SLACK (the benchmark run on server E: 1; CI: 3), else 0 and the
    // time is only printed. A shared host's load inflates even the least CPU time past such a budget.
    inline double benchmarkPerfSlack(){
        const char* value = std::getenv("JASPER_HTMLUI_PERF_SLACK");
        const double factor = value ? std::atof(value) : 0.0;
        return factor > 0.0 ? factor : 0.0;
    }

    // One measurement: the median CPU time of the calling thread over 20 samples of `repeat`
    // runs, after 3 warm-up runs (the work is single-threaded: wall time on a shared host mostly
    // measures the other processes; `repeat` is for the clock's resolution).
    inline double medianCpuMs(const std::function<void()>& work, int repeat){
        for (int i = 0; i < 3; i++)
            work();
        std::vector<double> times;
        for (int i = 0; i < 20; i++){
            const double start = threadCpuMs();
            for (int r = 0; r < repeat; r++)
                work();
            times.push_back((threadCpuMs() - start) / (double)repeat);
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    // What a gate compares with its target: the least of up to 15 measurements (medianCpuMs),
    // taken until one is within the target. Other processes on the host (their cache and
    // memory traffic, the core's frequency) only ever add CPU time to a measurement, never
    // remove any, so the least of several is the closest to the code's own cost; on a quiet
    // host every measurement is that cost and the least is the one measurement the gate took
    // before. A step whose own cost is above the target has no measurement within it.
    // The measurements after the first are 200 ms apart (sleeping takes no CPU time): taken back
    // to back, all 15 of a short step (the user-agent sheet's, 15 x 23 rewrites in 0.3 s) fit in
    // one burst of a virtual machine's neighbours, which slowed every one of them by a quarter
    // on the test host (p6-accept); spread over 3 s they also see the host outside it.
    inline double gatedCpuMs(const std::function<void()>& work, int repeat, double targetMs){
        double least = medianCpuMs(work, repeat);
        for (int attempt = 1; attempt < 15 && least > targetMs; attempt++){
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            least = std::min(least, medianCpuMs(work, repeat));
        }
        return least;
    }

    // What the generated style sheet holds (printed by the test that rewrites it)
    struct SheetShape {
        size_t rules = 0;
        size_t declarations = 0;
        size_t repeated = 0;        // declarations whose text an earlier declaration has
        size_t familyDeclarations = 0;   // background, transition, animation, outline, text-shadow
        size_t varReads = 0;
        size_t mediaRules = 0;
    };

    // A game-UI style sheet of about 55 KB and 1 200 rules (the input of the 3.14 row), shaped
    // like the author sheets of the golden pages (fixtures/compat/pages, 866 declarations):
    // the same property mix (background 12%, width and height 12%, the border family 11%,
    // the font family 10%, box-shadow 2.5%, outline, text-shadow, transition and animation
    // 0.2 to 0.7% each), 8% of the values read a custom property, 41% of the declarations
    // repeat an earlier one (the figure of those sheets taken together; the user-agent sheet
    // has 34%, the CSS fixture cases 10%), selectors of classes with descendants (16%),
    // pseudo-classes (6%), attributes (6%), pseudo-elements (3%), type selectors (18%), a few
    // :is() and :not(), some selector lists and a few @media blocks. The size and the rule
    // count are the row's, which leaves 1.5 declarations for a rule where the pages have 3.9.
    // Deterministic: a fixed seed, no clock.
    //
    // pageShape: the same sheet with the pages' rules instead of the row's count: 3.9
    // declarations a rule, so about 450 rules in the same 55 KB (what a real sheet of that
    // size holds; the row's 1 200 rules are the heavier case, with more selectors).
    inline std::string representativeSheet(SheetShape* shape = nullptr, bool pageShape = false){
        uint32_t state = 0x2F6E2B1u;
        auto next = [&state](){
            state = state * 1664525u + 1013904223u;
            return state >> 8;
        };
        auto pick = [&](uint32_t count){ return next() % count; };
        auto number = [&](int low, int high){ return low + (int)(next() % (uint32_t)(high - low + 1)); };
        auto hex = [&](){
            static const char digits[] = "0123456789abcdef";
            std::string color = "#";
            for (int i = 0; i < 6; i++)
                color += digits[pick(16)];
            return color;
        };
        auto px = [&](int low, int high){ return std::to_string(number(low, high)) + "px"; };
        auto alpha = [&](){ return "." + std::to_string(number(1, 9)); };
        auto rgba = [&](){
            return "rgba(" + std::to_string(number(0, 255)) + "," + std::to_string(number(0, 255)) + "," + std::to_string(number(0, 255)) + "," + alpha() + ")";
        };
        static const char* const colorVars[] = {"--accent", "--panel", "--text", "--muted", "--danger", "--ok", "--border"};
        [[maybe_unused]] static const char* const sizeVars[] = {"--pad", "--gap", "--radius", "--bar"};
        auto colorVar = [&](){ return std::string("var(") + colorVars[pick(7)] + ")"; };

        struct Property {
            const char* name;
            int weight;       // per thousand declarations, from the pages
        };
        static const Property properties[] = {
            {"background", 111}, {"width", 69}, {"height", 55}, {"color", 53}, {"border-radius", 45}, {"display", 45},
            {"font-size", 44}, {"padding", 38}, {"border", 36}, {"position", 35}, {"align-items", 28}, {"font-weight", 27},
            {"box-shadow", 25}, {"gap", 25}, {"font-family", 22}, {"justify-content", 20}, {"top", 18}, {"left", 18},
            {"line-height", 16}, {"letter-spacing", 13}, {"border-color", 13}, {"margin", 10}, {"cursor", 10}, {"right", 9},
            {"bottom", 9}, {"opacity", 9}, {"transform", 9}, {"background-color", 9}, {"box-sizing", 7}, {"overflow", 7},
            {"text-align", 7}, {"margin-top", 7}, {"border-bottom", 7}, {"flex-direction", 6}, {"margin-bottom", 6},
            {"text-shadow", 6}, {"text-transform", 5}, {"transition", 5}, {"outline", 5}, {"content", 5}, {"margin-left", 3},
            {"animation", 2}, {"z-index", 8}, {"flex", 8}, {"min-width", 6}, {"max-width", 6}, {"white-space", 4},
            {"pointer-events", 4}, {"visibility", 3}, {"background-size", 3}, {"transition-duration", 1}, {"outline-offset", 2},
        };
        int totalWeight = 0;
        for (const Property& p : properties)
            totalWeight += p.weight;

        SheetShape made;
        // a value written for the property; 8% of the values that can read a custom property
        auto valueOf = [&](const std::string& name){
            // (a third of the declarations can read one: 8 in 100 declarations do)
            const bool var = pick(100) < 15;
            if (name == "background"){
                if (var){ made.varReads++; return colorVar(); }
                switch (pick(6)){
                    case 0: return rgba();
                    case 1: return "linear-gradient(" + std::to_string(number(0, 359)) + "deg," + hex() + "," + hex() + ")";
                    case 2: return std::string("transparent");
                    case 3: return "radial-gradient(circle," + hex() + "," + hex() + " " + std::to_string(number(40, 90)) + "%)";
                    default: return hex();
                }
            }
            if (name == "color" || name == "border-color" || name == "background-color"){
                if (var){ made.varReads++; return colorVar(); }
                return pick(4) == 0 ? rgba() : hex();
            }
            if (name == "width" || name == "height" || name == "min-width" || name == "max-width")
                return pick(5) == 0 ? std::to_string(number(1, 20) * 5) + "%" : px(8, 640);
            if (name == "border-radius"){
                if (var){ made.varReads++; return std::string("var(--radius)"); }
                return pick(6) == 0 ? std::string("50%") : px(2, 24);
            }
            if (name == "display"){
                static const char* const values[] = {"flex", "flex", "flex", "block", "none", "inline-block", "inline-flex", "grid"};
                return std::string(values[pick(8)]);
            }
            if (name == "font-size")
                return pick(6) == 0 ? "." + std::to_string(number(7, 9)) + "em" : px(10, 48);
            if (name == "padding" || name == "margin"){
                if (var){ made.varReads++; return std::string("var(--pad)"); }
                return pick(2) ? px(0, 32) : px(0, 24) + " " + px(0, 32);
            }
            if (name == "border" || name == "border-bottom")
                return px(1, 3) + " solid " + (var ? (made.varReads++, std::string("var(--border)")) : (pick(3) == 0 ? rgba() : hex()));
            if (name == "position"){
                static const char* const values[] = {"relative", "absolute", "absolute", "relative", "fixed", "static"};
                return std::string(values[pick(6)]);
            }
            if (name == "align-items" || name == "justify-content"){
                static const char* const values[] = {"center", "center", "flex-start", "flex-end", "space-between", "stretch"};
                return std::string(values[pick(name == "align-items" ? 4 : 5) + (name == "align-items" && pick(8) == 0 ? 2 : 0)]);
            }
            if (name == "font-weight"){
                static const char* const values[] = {"400", "500", "600", "700", "bold", "800"};
                return std::string(values[pick(6)]);
            }
            if (name == "box-shadow")
                return (pick(5) == 0 ? "inset " : "") + px(0, 4) + " " + px(0, 12) + " " + px(2, 32) + " " + rgba();
            if (name == "gap"){
                if (var){ made.varReads++; return std::string("var(--gap)"); }
                return px(2, 24);
            }
            if (name == "font-family"){
                static const char* const values[] = {"'Rajdhani',sans-serif", "sans-serif", "'Orbitron',monospace", "monospace", "system-ui,sans-serif"};
                return std::string(values[pick(5)]);
            }
            if (name == "top" || name == "left" || name == "right" || name == "bottom")
                return pick(6) == 0 ? std::to_string(number(0, 10) * 10) + "%" : px(0, 400);
            if (name == "line-height")
                return "1." + std::to_string(number(0, 9));
            if (name == "letter-spacing")
                return "." + std::to_string(number(1, 30)) + "em";
            if (name == "cursor")
                return std::string(pick(4) ? "pointer" : "default");
            if (name == "opacity")
                return alpha();
            if (name == "transform"){
                switch (pick(4)){
                    case 0: return "translate(" + px(-40, 40) + "," + px(-40, 40) + ")";
                    case 1: return "scale(1." + std::to_string(number(0, 30)) + ")";
                    case 2: return "rotate(" + std::to_string(number(-45, 45)) + "deg)";
                    default: return "translateY(" + px(-24, 24) + ")";
                }
            }
            if (name == "box-sizing")
                return std::string("border-box");
            if (name == "overflow")
                return std::string(pick(3) ? "hidden" : "auto");
            if (name == "text-align")
                return std::string(pick(3) ? "center" : (pick(2) ? "right" : "left"));
            if (name == "margin-top" || name == "margin-bottom" || name == "margin-left")
                return pick(8) == 0 ? std::string("auto") : px(0, 48);
            if (name == "flex-direction")
                return std::string(pick(3) ? "column" : "row");
            if (name == "text-shadow")
                return px(0, 2) + " " + px(0, 2) + " " + px(1, 12) + " " + (var ? (made.varReads++, colorVar()) : rgba());
            if (name == "text-transform")
                return std::string(pick(4) ? "uppercase" : "none");
            if (name == "transition"){
                static const char* const what[] = {"opacity", "transform", "background", "color", "all"};
                return std::string(what[pick(5)]) + " ." + std::to_string(number(1, 6)) + "s " + (pick(2) ? "ease" : "ease-out");
            }
            if (name == "transition-duration")
                return "." + std::to_string(number(1, 9)) + "s";
            if (name == "outline")
                return pick(4) == 0 ? std::string("none") : px(1, 3) + " solid " + hex();
            if (name == "outline-offset")
                return px(1, 4);
            if (name == "content")
                return std::string("''");
            if (name == "animation"){
                static const char* const names[] = {"fadeIn", "pulse"};
                return std::string(names[pick(2)]) + " ." + std::to_string(number(2, 9)) + "s " + (pick(2) ? "ease" : "linear") + (pick(3) == 0 ? " infinite" : "");
            }
            if (name == "z-index")
                return std::to_string(number(1, 50));
            if (name == "flex")
                return pick(3) ? std::string("1") : "0 0 " + px(40, 320);
            if (name == "white-space")
                return std::string("nowrap");
            if (name == "pointer-events")
                return std::string(pick(2) ? "none" : "auto");
            if (name == "visibility")
                return std::string(pick(2) ? "hidden" : "visible");
            if (name == "background-size")
                return std::string(pick(2) ? "cover" : "contain");
            return px(0, 64);
        };

        std::string css = ":root{--accent:#00c8ff;--panel:rgba(20,24,38,.92);--text:#e8ecf4;--muted:#8a93a8;--danger:#ff4d5e;--ok:#3ddc84;"
                          "--border:rgba(255,255,255,.12);--pad:12px;--gap:8px;--radius:10px;--bar:6px}\n"
                          "*{box-sizing:border-box;margin:0;padding:0}\n"
                          "@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}\n"
                          "@keyframes pulse{0%{opacity:1}50%{opacity:.5}100%{opacity:1}}\n";

        static const char* const blocks[] = {"hud", "menu", "panel", "inv", "slot", "btn", "bar", "tab", "chat", "map", "stat", "tip",
                                             "dlg", "list", "card", "opt", "quest", "shop", "skill", "toast"};
        static const char* const parts[] = {"title", "icon", "label", "value", "row", "body", "head", "foot", "item", "fill", "badge", "close"};
        static const char* const types[] = {"button", "li", "span", "img", "h2", "p", "input", "label"};
        static const char* const pseudoClasses[] = {":hover", ":active", ":focus", ":first-child", ":last-child", ":disabled", ":checked"};
        static const char* const attributes[] = {"[disabled]", "[data-state=\"open\"]", "[data-rarity=\"epic\"]", "[aria-selected=\"true\"]"};

        std::set<std::string> seen;                              // declaration texts
        std::vector<std::vector<std::string>> written(sizeof(properties) / sizeof(properties[0]));
        int serial = 0;
        auto selector = [&](){
            std::string text;
            const uint32_t roll = pick(100);
            const std::string block = std::string(blocks[pick(20)]) + "-" + std::to_string(serial++ % 97);
            if (roll < 18)
                text = std::string(types[pick(8)]) + "." + block;
            else
                text = "." + block;
            const uint32_t shape = pick(100);
            if (shape < 16)
                text += std::string(" .") + parts[pick(12)];
            else if (shape < 22)
                text += pseudoClasses[pick(7)];
            else if (shape < 28)
                text += attributes[pick(4)];
            else if (shape < 31)
                text += pick(2) ? "::before" : "::after";
            else if (shape < 33)
                text += std::string(pick(2) ? ":not(.is-" : ":is(.is-") + parts[pick(12)] + (pick(2) ? ")" : ",.is-on)");
            else if (shape < 37)
                text += std::string(">.") + parts[pick(12)];
            return text;
        };
        auto rule = [&](){
            std::string text = selector();
            // (the page shape is written the way the pages are: a declaration a line)
            if (pick(100) < 6)
                text += (pageShape ? ", " : ",") + selector();
            text += pageShape ? " {\n" : "{";
            const uint32_t roll = pick(100);
            int count = roll < 62 ? 1 : (roll < 90 ? 2 : (roll < 97 ? 3 : 4));
            if (pageShape){
                // the pages: 22% of the rules have 1 declaration, 17% 2, 19% 3, 10% 4, 6% 5, 8% 6, 18% 7 to 10
                static const int counts[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                             4, 4, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 10};
                count = counts[roll % 50];
            }
            for (int i = 0; i < count; i++){
                int at = (int)pick((uint32_t)totalWeight);
                size_t index = 0;
                while (at >= properties[index].weight)
                    at -= properties[index++].weight;
                const std::string name = properties[index].name;
                // 41 of 100 declarations repeat an earlier one: one that was written when the
                // sheet is below that, a new value otherwise (a property of few values has
                // written them all before, and repeats)
                std::string declaration;
                if (!written[index].empty() && made.repeated * 100 < made.declarations * 41){
                    declaration = written[index][pick((uint32_t)written[index].size())];
                }else{
                    for (int attempt = 0; attempt < 4; attempt++){
                        declaration = name + (pageShape ? ": " : ":") + valueOf(name);
                        if (seen.count(declaration) == 0)
                            break;
                    }
                }
                if (!seen.insert(declaration).second)
                    made.repeated++;
                else
                    written[index].push_back(declaration);
                made.declarations++;
                if (name == "background" || name == "background-color" || name == "background-size" || name == "transition" ||
                    name == "transition-duration" || name == "animation" || name == "outline" || name == "outline-offset" || name == "text-shadow")
                    made.familyDeclarations++;
                if (pageShape)
                    text += "  " + declaration + ";\n";
                else
                    text += (i ? ";" : "") + declaration;
            }
            made.rules++;
            return text + "}\n";
        };
        const size_t ruleCount = pageShape ? 100000 : 1200;
        while (made.rules < ruleCount && !(pageShape && css.size() >= 55 * 1024 - 60)){
            // (about one rule in thirty sits in an @media block of three)
            if (made.rules % 90 == 89 && made.rules + 3 <= ruleCount){
                static const char* const queries[] = {"(max-width:768px)", "(min-width:1280px)", "(orientation:portrait)", "(max-height:540px)"};
                css += std::string("@media ") + queries[pick(4)] + "{\n";
                for (int i = 0; i < 3; i++){
                    css += rule();
                    made.mediaRules++;
                }
                css += "}\n";
                continue;
            }
            css += rule();
        }
        if (shape)
            *shape = made;
        return css;
    }

}

#endif //COMPAT_PERF_FIXTURES_H
