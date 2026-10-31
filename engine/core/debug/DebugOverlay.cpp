// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugOverlay.h"

#include "DebugOverlayResources.h"
#include "DebugStats.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "Scene.h"
#include "System.h"
#include "component/Transform.h"
#include "component/UIDocumentComponent.h"
#include "input/InputControlNames.h"
#include "io/FileUtils.h"
#include "manager/SceneManager.h"
#include "math/Rect.h"
#include "script/LuaBinding.h"
#include "subsystem/UIDocumentSystem.h"

#include "lua.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <set>

using namespace jasperengine;

namespace {

    using Stats = DebugOverlaySettings::Stats;

    constexpr double holdSeconds = 0.75;        // three fingers held this long toggle the console
    constexpr double tapSeconds = 0.5;          // four fingers down and up within it toggle it
    constexpr double textInterval = 0.25;       // stats text, 4 Hz
    constexpr double graphInterval = 0.1;       // the frame-time graph, 10 Hz
    constexpr size_t graphBars = 120;
    constexpr float graphHeight = 40.0f;        // px of the graph (overlay px), 50 ms at the top
    constexpr float graphMaxMs = 50.0f;
    constexpr size_t rowTextLimit = 300;        // characters of a message's first line in its row

    const Rml::EventId capturedEvents[] = {Rml::EventId::Change, Rml::EventId::Scroll, Rml::EventId::Dragstart,
                                           Rml::EventId::Drag, Rml::EventId::Dragend};

    struct Command {
        std::string name;
        std::string description;
        std::function<void(const std::vector<std::string>&)> function;
        std::string ownerTag;
        bool builtIn = false;
    };

    // one row of the console list: a line of the log, or (collapsed) every line with its text
    struct Row {
        uint64_t id = 0;            // the (first) line's id
        uint64_t lastId = 0;        // the newest line it stands for
        LogLevel level = LogLevel::Info;
        std::string message;
        std::string stackTrace;
        double time = 0.0;
        size_t count = 1;
        bool expanded = false;
        Rml::Element* element = nullptr;
        Rml::Element* countElement = nullptr;
    };

    class OverlayListener : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override;
    };

    struct OverlayState {
        DebugOverlaySettings settings;
        bool enabled = false;
        bool consoleVisible = false;
        Stats statsMode = Stats::Off;
        bool warnedDisabled = false;
        // the document could not load: the overlay turned itself off
        bool loadFailed = false;
        int toggleKey = D_KEY_UNKNOWN;
        int statsKey = D_KEY_UNKNOWN;

        // the overlay Scene and its document
        std::unique_ptr<Scene> scene;
        Entity entity = NULL_ENTITY;
        Rml::ElementDocument* document = nullptr;
        OverlayListener listener;
        std::vector<Rml::Element*> bars;
        std::vector<std::pair<std::string, Rml::Element*>> statValues;

        // the console's view
        bool showInfo = true, showWarn = true, showError = true;
        bool collapse = false;
        bool fullHeight = false;
        std::string search;             // lower case
        std::deque<Row> rows;
        uint64_t lastSeenId = 0;        // the newest line the list has looked at
        bool rebuild = true;
        bool stickToBottom = true;
        bool scrollToBottom = false;
        size_t warnAtOpen = 0, errorAtOpen = 0;
        size_t errorsAtEnable = 0;
        bool openedOnError = false;

        // the clock of the overlay (unscaled, runs while the game is held)
        double clock = 0.0;
        double nextText = 0.0, nextGraph = 0.0;
        bool layoutChanged = true;

        // gestures
        struct TouchPoint { float x, y; };
        std::map<int, TouchPoint> touches;
        std::map<int, TouchPoint> touchStarts;
        double holdStart = -1.0;
        bool holdFired = false;
        bool tapArmed = false;
        double tapStart = 0.0;

        // the popup's place (fraction of the height) and its drag
        float popupTop = 0.45f;
        bool popupDragging = false;
        float popupDragStartY = 0.0f;
        float popupDragStartTop = 0.0f;
        bool popupDragged = false;

        std::vector<Command> commands;

        DebugOverlay::InsetsFunction safeArea = nullptr;
        std::string appliedInsets;
    };

    OverlayState& state(){
        static OverlayState* instance = new OverlayState();
        return *instance;
    }

    // ---- small helpers

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return text;
    }

    const char* levelName(LogLevel level){
        switch (level){
            case LogLevel::Verbose: return "VERBOSE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Warn: return "WARN";
            case LogLevel::Error: return "ERROR";
            default: return "INFO";
        }
    }

    const char* levelClass(LogLevel level){
        switch (level){
            case LogLevel::Verbose: return "lvl-verbose";
            case LogLevel::Debug: return "lvl-debug";
            case LogLevel::Warn: return "lvl-warn";
            case LogLevel::Error: return "lvl-error";
            default: return "lvl-info";
        }
    }

    // mm:ss.mmm since the app started
    std::string formatTime(double seconds){
        if (seconds < 0.0) seconds = 0.0;
        const long long millis = static_cast<long long>(seconds * 1000.0);
        char text[32];
        snprintf(text, sizeof(text), "%02lld:%02lld.%03lld", millis / 60000, (millis / 1000) % 60, millis % 1000);
        return text;
    }

    std::string countText(size_t count){
        return count > 999 ? std::string("999+") : std::to_string(count);
    }

    std::string formatBytes(uint64_t bytes){
        char text[32];
        if (bytes >= 1024ull * 1024ull * 1024ull)
            snprintf(text, sizeof(text), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024ull * 1024ull)
            snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
        else
            snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
        return text;
    }

    std::string firstLine(const std::string& text){
        const size_t end = text.find('\n');
        std::string line = end == std::string::npos ? text : text.substr(0, end);
        if (line.size() > rowTextLimit){
            // not inside a UTF-8 sequence
            size_t cut = rowTextLimit;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
                cut--;
            line = line.substr(0, cut) + "...";
        }
        return line;
    }

    // `[00:12.345] ERROR message` and the trace under it
    std::string rowText(const Row& row){
        std::string text = "[" + formatTime(row.time) + "] " + levelName(row.level) + " " + row.message;
        if (row.count > 1)
            text += " (x" + std::to_string(row.count) + ")";
        if (!row.stackTrace.empty())
            text += "\n" + row.stackTrace;
        return text;
    }

    std::string entryText(const LogEntry& entry){
        std::string text = "[" + formatTime(entry.time) + "] " + levelName(entry.level) + " " + entry.message;
        if (!entry.stackTrace.empty())
            text += "\n" + entry.stackTrace;
        return text;
    }

    bool validCommandName(const std::string& name){
        if (name.empty())
            return false;
        for (char c : name){
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    // words of a command line; "double" or 'single' quotes group words
    std::vector<std::string> splitCommandLine(const std::string& line){
        std::vector<std::string> words;
        std::string word;
        bool inWord = false;
        char quote = 0;
        for (char c : line){
            if (quote){
                if (c == quote){
                    quote = 0;
                }else{
                    word += c;
                }
                continue;
            }
            if (c == '"' || c == '\''){
                quote = c;
                inWord = true;
                continue;
            }
            if (c == ' ' || c == '\t'){
                if (inWord){
                    words.push_back(word);
                    word.clear();
                    inWord = false;
                }
                continue;
            }
            word += c;
            inWord = true;
        }
        if (inWord)
            words.push_back(word);
        return words;
    }

    Rml::Element* byId(const char* id){
        OverlayState& s = state();
        return s.document ? s.document->GetElementById(id) : nullptr;
    }

    void setHidden(Rml::Element* element, bool hidden){
        if (element)
            element->SetClass("hidden", hidden);
    }

    void setText(Rml::Element* element, const std::string& text){
        if (!element || !state().document)
            return;
        while (element->GetNumChildren() > 0)
            element->RemoveChild(element->GetChild(0));
        element->AppendChild(state().document->CreateTextNode(text));
    }

    void setText(const char* id, const std::string& text){
        setText(byId(id), text);
    }

    std::string inputValue(const char* id){
        Rml::Element* element = byId(id);
        if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element))
            return control->GetValue();
        return std::string();
    }

    void setInputValue(const char* id, const std::string& value){
        Rml::Element* element = byId(id);
        if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element))
            control->SetValue(value);
    }

    bool popupWanted(){
        const OverlayState& s = state();
        if (!s.enabled || s.consoleVisible)
            return false;
        switch (s.settings.popup){
            case DebugOverlaySettings::Popup::Always: return true;
            case DebugOverlaySettings::Popup::TouchDevices: return System::hasInstance() && System::instance().isTouchDevice();
            default: return false;
        }
    }

    // something of the overlay shows
    bool anythingVisible(){
        const OverlayState& s = state();
        return s.enabled && (s.consoleVisible || s.statsMode != Stats::Off || popupWanted());
    }

    uint32_t levelMask(){
        const OverlayState& s = state();
        uint32_t mask = 0;
        if (s.showInfo) mask |= logLevelBit(LogLevel::Info) | logLevelBit(LogLevel::Verbose) | logLevelBit(LogLevel::Debug);
        if (s.showWarn) mask |= logLevelBit(LogLevel::Warn);
        if (s.showError) mask |= logLevelBit(LogLevel::Error);
        return mask;
    }

    bool matchesSearch(const LogEntry& entry){
        const std::string& search = state().search;
        if (search.empty())
            return true;
        return lower(entry.message).find(search) != std::string::npos || lower(entry.stackTrace).find(search) != std::string::npos;
    }

    // ---- console rows

    void fillRow(Row& row){
        OverlayState& s = state();
        Rml::Element* element = row.element;
        while (element->GetNumChildren() > 0)
            element->RemoveChild(element->GetChild(0));
        element->SetClassNames(std::string("row ") + levelClass(row.level) + (row.expanded ? " expanded" : ""));

        Rml::Element* line = element->AppendChild(s.document->CreateElement("div"));
        line->SetClass("line", true);
        line->AppendChild(s.document->CreateElement("span"))->SetClass("mark", true);
        Rml::Element* time = line->AppendChild(s.document->CreateElement("span"));
        time->SetClass("time", true);
        time->AppendChild(s.document->CreateTextNode(formatTime(row.time)));
        Rml::Element* message = line->AppendChild(s.document->CreateElement("span"));
        message->SetClass("msg", true);
        message->AppendChild(s.document->CreateTextNode(firstLine(row.message)));
        row.countElement = line->AppendChild(s.document->CreateElement("span"));
        row.countElement->SetClass("count", true);
        row.countElement->SetClass("hidden", row.count <= 1);
        row.countElement->AppendChild(s.document->CreateTextNode(countText(row.count)));

        if (row.expanded){
            Rml::Element* detail = element->AppendChild(s.document->CreateElement("div"));
            detail->SetClass("detail", true);
            detail->AppendChild(s.document->CreateTextNode(row.message));
            if (!row.stackTrace.empty()){
                Rml::Element* trace = detail->AppendChild(s.document->CreateElement("div"));
                trace->SetClass("trace", true);
                trace->AppendChild(s.document->CreateTextNode(row.stackTrace));
            }
        }
    }

    void appendRow(Row row){
        OverlayState& s = state();
        Rml::Element* list = byId("list");
        if (!list)
            return;
        row.element = list->AppendChild(s.document->CreateElement("div"));
        row.element->SetAttribute("data-id", std::to_string(row.id));
        s.rows.push_back(std::move(row));
        fillRow(s.rows.back());
    }

    void removeFirstRow(){
        OverlayState& s = state();
        Rml::Element* list = byId("list");
        if (list && s.rows.front().element)
            list->RemoveChild(s.rows.front().element);
        s.rows.pop_front();
    }

    void clearRows(){
        OverlayState& s = state();
        Rml::Element* list = byId("list");
        // the DOM children only: the scrollbars stay
        if (list){
            while (list->GetNumChildren() > 0)
                list->RemoveChild(list->GetChild(0));
        }
        s.rows.clear();
    }

    Row* findCollapsed(const LogEntry& entry){
        for (Row& row : state().rows){
            if (row.level == entry.level && row.message == entry.message && row.stackTrace == entry.stackTrace)
                return &row;
        }
        return nullptr;
    }

    void addEntry(const LogEntry& entry){
        OverlayState& s = state();
        if (!(levelMask() & logLevelBit(entry.level)) || !matchesSearch(entry))
            return;
        if (s.collapse){
            if (Row* row = findCollapsed(entry)){
                row->count++;
                row->lastId = entry.id;
                if (row->countElement){
                    row->countElement->SetClass("hidden", false);
                    setText(row->countElement, countText(row->count));
                }
                return;
            }
        }
        Row row;
        row.id = entry.id;
        row.lastId = entry.id;
        row.level = entry.level;
        row.message = entry.message;
        row.stackTrace = entry.stackTrace;
        row.time = entry.time;
        appendRow(std::move(row));
    }

    bool listAtBottom(Rml::Element* list){
        return list->GetScrollTop() + list->GetClientHeight() >= list->GetScrollHeight() - 4.0f;
    }

    void updateRows(){
        OverlayState& s = state();
        Rml::Element* list = byId("list");
        if (!list)
            return;

        const uint64_t firstBuffered = Log::getFirstBufferedId();
        if (s.rebuild){
            std::set<uint64_t> expanded;
            for (const Row& row : s.rows){
                if (row.expanded) expanded.insert(row.id);
            }
            clearRows();
            for (const LogEntry& entry : Log::getEntries())
                addEntry(entry);
            for (Row& row : s.rows){
                if (expanded.count(row.id)){
                    row.expanded = true;
                    fillRow(row);
                }
            }
            s.lastSeenId = Log::getLastId();
            s.rebuild = false;
            s.scrollToBottom = s.stickToBottom;
        }else{
            // lines that left the buffer (trimmed, Log.clear) leave the list; a collapsed row
            // stays while a line of its text is still buffered
            const bool atBottom = listAtBottom(list);
            while (!s.rows.empty() && s.rows.front().lastId < firstBuffered)
                removeFirstRow();
            const std::vector<LogEntry> added = Log::getEntries(0, LOG_LEVELS_ALL, s.lastSeenId);
            for (const LogEntry& entry : added)
                addEntry(entry);
            if (!added.empty())
                s.lastSeenId = added.back().id;
            if (!added.empty() && (atBottom || s.stickToBottom))
                s.scrollToBottom = true;
        }
        setHidden(byId("empty"), !s.rows.empty());
    }

    // ---- the static parts of the document

    void buildDocument(){
        OverlayState& s = state();
        s.bars.clear();
        s.statValues.clear();
        if (Rml::Element* bars = byId("graph-bars")){
            for (size_t i = 0; i < graphBars; i++){
                Rml::Element* bar = bars->AppendChild(s.document->CreateElement("div"));
                bar->SetClass("bar", true);
                bar->SetProperty("left", std::to_string(i * 2) + "px");
                bar->SetProperty("height", "0px");
                s.bars.push_back(bar);
            }
        }
        // the 16.7 ms (60 FPS) and 33.3 ms (30 FPS) lines
        if (Rml::Element* graph = byId("graph")){
            for (int i = 0; i < graph->GetNumChildren(); i++){
                Rml::Element* child = graph->GetChild(i);
                if (child->IsClassSet("l60"))
                    child->SetProperty("bottom", std::to_string((int)std::round(16.7f / graphMaxMs * graphHeight)) + "px");
                else if (child->IsClassSet("l30"))
                    child->SetProperty("bottom", std::to_string((int)std::round(33.3f / graphMaxMs * graphHeight)) + "px");
            }
        }
        if (Rml::Element* table = byId("stats-table")){
            static const char* const names[] = {"Frame ms", "1% low", "Draw calls", "Instances", "Triangles", "Memory", "Lua",
                                                "Audio", "Bodies 2D/3D", "Entities", "Scenes", "Coroutines", "HTML UI"};
            for (const char* name : names){
                Rml::Element* row = table->AppendChild(s.document->CreateElement("div"));
                row->SetClass("stat-row", true);
                Rml::Element* label = row->AppendChild(s.document->CreateElement("span"));
                label->SetClass("stat-name", true);
                label->AppendChild(s.document->CreateTextNode(name));
                Rml::Element* value = row->AppendChild(s.document->CreateElement("span"));
                value->SetClass("stat-value", true);
                value->AppendChild(s.document->CreateTextNode("-"));
                s.statValues.push_back({name, value});
            }
        }
        // clicks bubble up to the document; the others are caught on their way down (scroll
        // does not bubble)
        s.document->AddEventListener(Rml::EventId::Click, &s.listener);
        for (Rml::EventId id : capturedEvents)
            s.document->AddEventListener(id, &s.listener, true);
        s.rebuild = true;
        s.layoutChanged = true;
        s.nextText = 0.0;
        s.nextGraph = 0.0;
    }

    void setStatValue(const char* name, const std::string& text){
        for (auto& [label, element] : state().statValues){
            if (label == name){
                setText(element, text);
                return;
            }
        }
    }

    void updateStatsText(){
        OverlayState& s = state();
        const DebugStatsSnapshot stats = DebugStats::sample();
        char line[64];
        snprintf(line, sizeof(line), "%.0f FPS  %.1f ms", stats.fps, stats.frameMsAvg);
        if (Rml::Element* element = byId("stats-line")){
            setText(element, line);
            element->SetClass("slow", stats.frameMsAvg > 17.5f && stats.frameMsAvg <= 34.0f);
            element->SetClass("very-slow", stats.frameMsAvg > 34.0f);
        }
        if (s.statsMode != Stats::Full)
            return;
        char text[96];
        snprintf(text, sizeof(text), "%.1f  (%.1f - %.1f)", stats.frameMsAvg, stats.frameMsMin, stats.frameMsMax);
        setStatValue("Frame ms", text);
        snprintf(text, sizeof(text), "%.1f ms", stats.frameMs1PercentLow);
        setStatValue("1% low", text);
        setStatValue("Draw calls", std::to_string(stats.drawCalls));
        setStatValue("Instances", std::to_string(stats.instances));
        setStatValue("Triangles", std::to_string(stats.triangles));
        setStatValue("Memory", stats.processMemoryBytes ? formatBytes(stats.processMemoryBytes) : std::string("-"));
        setStatValue("Lua", formatBytes(stats.luaMemoryBytes));
        setStatValue("Audio", std::to_string(stats.audioVoices) + " / " + std::to_string(stats.audioVoicesMax) + " voices");
        setStatValue("Bodies 2D/3D", std::to_string(stats.bodies2D) + " / " + std::to_string(stats.bodies3D));
        setStatValue("Entities", std::to_string(stats.entities));
        setStatValue("Scenes", std::to_string(stats.scenes));
        setStatValue("Coroutines", std::to_string(stats.coroutines) + " (" + std::to_string(stats.timers) + " timers)");
        setStatValue("HTML UI", std::to_string(stats.htmlUiDrawCalls) + " draws");
    }

    void updateGraph(){
        OverlayState& s = state();
        const std::vector<float> history = DebugStats::frameHistory();
        const size_t count = std::min(history.size(), graphBars);
        for (size_t i = 0; i < s.bars.size(); i++){
            // the newest frame at the right
            const size_t fromEnd = s.bars.size() - 1 - i;
            float ms = 0.0f;
            if (fromEnd < count)
                ms = history[history.size() - 1 - fromEnd];
            const float height = std::min(ms, graphMaxMs) / graphMaxMs * graphHeight;
            s.bars[i]->SetProperty("height", std::to_string((int)std::ceil(height)) + "px");
            s.bars[i]->SetClass("slow", ms > 17.5f && ms <= 34.0f);
            s.bars[i]->SetClass("very-slow", ms > 34.0f);
        }
    }

    void updateCounts(){
        setText("count-info", countText(Log::getCount(LogLevel::Info) + Log::getCount(LogLevel::Verbose) + Log::getCount(LogLevel::Debug)));
        setText("count-warn", countText(Log::getCount(LogLevel::Warn)));
        setText("count-error", countText(Log::getCount(LogLevel::Error)));
    }

    void updatePopup(){
        OverlayState& s = state();
        const size_t warnings = Log::getCount(LogLevel::Warn);
        const size_t errors = Log::getCount(LogLevel::Error);
        // a clear since the last open
        if (warnings < s.warnAtOpen) s.warnAtOpen = 0;
        if (errors < s.errorAtOpen) s.errorAtOpen = 0;
        setText("popup-warn", countText(warnings - s.warnAtOpen));
        setText("popup-error", countText(errors - s.errorAtOpen));
        if (Rml::Element* popup = byId("popup")){
            char top[32];
            snprintf(top, sizeof(top), "%.1f%%", s.popupTop * 100.0f);
            popup->SetProperty("top", top);
        }
    }

    // the classes and inline styles that follow the state
    void applyLayout(){
        OverlayState& s = state();
        setHidden(byId("console"), !s.consoleVisible);
        setHidden(byId("popup"), !popupWanted());
        setHidden(byId("stats"), s.statsMode == Stats::Off);
        setHidden(byId("stats-table"), s.statsMode != Stats::Full);
        if (Rml::Element* stats = byId("stats")){
            using Corner = DebugOverlaySettings::Corner;
            const Corner corner = s.settings.statsCorner;
            stats->SetClass("corner-tl", corner == Corner::TopLeft);
            stats->SetClass("corner-tr", corner == Corner::TopRight);
            stats->SetClass("corner-bl", corner == Corner::BottomLeft);
            stats->SetClass("corner-br", corner == Corner::BottomRight);
            // the console covers the top: the stats move under it
            stats->SetProperty("margin-top", s.consoleVisible && !s.fullHeight && (corner == Corner::TopLeft || corner == Corner::TopRight)
                                              ? "50%" : "0px");
        }
        if (Rml::Element* console = byId("console")){
            console->SetClass("full", s.fullHeight);
            console->SetClass("half", !s.fullHeight);
        }
        setText("size", s.fullHeight ? "Half" : "Full");
        if (Rml::Element* button = byId("filter-info")) button->SetClass("on", s.showInfo);
        if (Rml::Element* button = byId("filter-warn")) button->SetClass("on", s.showWarn);
        if (Rml::Element* button = byId("filter-error")) button->SetClass("on", s.showError);
        if (Rml::Element* button = byId("collapse")) button->SetClass("on", s.collapse);
        s.layoutChanged = false;
    }

    // the safe area (feature safe-area) in overlay px, as padding of the panels
    void applySafeArea(float scale){
        OverlayState& s = state();
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        if (s.safeArea)
            s.safeArea(left, top, right, bottom);
        // canvas px to overlay px: the canvas fills the view rect
        const Rect view = Engine::getViewRect();
        const float canvasToView = Engine::getCanvasWidth() > 0 ? view.getWidth() / (float)Engine::getCanvasWidth() : 1.0f;
        const float k = canvasToView / scale;
        auto px = [k](float v){ return std::to_string((int)std::round(std::max(0.0f, v) * k)) + "px"; };
        // the body is the panels' containing block: its edges move in by the insets (only
        // when they change: a property set dirties the layout)
        const std::string insets = px(left) + " " + px(top) + " " + px(right) + " " + px(bottom);
        if (insets == s.appliedInsets)
            return;
        s.appliedInsets = insets;
        if (Rml::Element* body = byId("overlay-body")){
            body->SetProperty("left", px(left));
            body->SetProperty("top", px(top));
            body->SetProperty("right", px(right));
            body->SetProperty("bottom", px(bottom));
        }
    }

    // overlay px per view pixel: text of 13 px stays legible whatever the canvas is
    float overlayScale(){
        const Rect view = Engine::getViewRect();
        const float shorter = std::min(view.getWidth(), view.getHeight());
        const bool touch = System::hasInstance() && System::instance().isTouchDevice();
        return std::max(1.0f, shorter / (touch ? 400.0f : 720.0f));
    }

    // ---- the Scene

    void destroyScene(){
        OverlayState& s = state();
        if (s.document){
            s.document->RemoveEventListener(Rml::EventId::Click, &s.listener);
            for (Rml::EventId id : capturedEvents)
                s.document->RemoveEventListener(id, &s.listener, true);
        }
        s.document = nullptr;
        s.appliedInsets.clear();
        s.bars.clear();
        s.statValues.clear();
        s.rows.clear();
        s.rebuild = true;
        s.entity = NULL_ENTITY;
        s.scene.reset();
    }

    void createScene(){
        OverlayState& s = state();
        s.scene = std::make_unique<Scene>();
        Scene& scene = *s.scene;
        s.entity = scene.createEntity();
        scene.addComponent<Transform>(s.entity, Transform{});
        UIDocumentComponent document;
        document.filename = EngineResources::overlayDocument;
        document.sortOrder = 1000;
        scene.addComponent<UIDocumentComponent>(s.entity, document);
        // its default UI camera; the systems load with the view
        scene.load();

        const float scale = overlayScale();
        const Rect view = Engine::getViewRect();
        auto documents = scene.getSystem<UIDocumentSystem>();
        documents->setReferenceSize(std::max(1, (int)std::round(view.getWidth() / scale)), std::max(1, (int)std::round(view.getHeight() / scale)));
        documents->loadDocumentNow(s.entity);
        s.document = documents->getDocument(s.entity);
        if (!s.document){
            Log::error("Debug overlay: its document (%s) could not be loaded; the overlay is off", EngineResources::overlayDocument);
            s.loadFailed = true;
            s.enabled = false;
            destroyScene();
            return;
        }
        buildDocument();
    }

    // ---- commands

    void builtInCommands(){
        OverlayState& s = state();
        auto add = [&s](const std::string& name, const std::string& description, std::function<void(const std::vector<std::string>&)> function){
            Command command;
            command.name = name;
            command.description = description;
            command.function = std::move(function);
            command.builtIn = true;
            s.commands.push_back(std::move(command));
        };
        add("help", "lists the commands", [](const std::vector<std::string>&){
            std::string text = "Commands:";
            for (const auto& [name, description] : DebugOverlay::getCommands())
                text += "\n  " + name + (description.empty() ? std::string() : " - " + description);
            Log::print("%s", text.c_str());
        });
        add("clear", "clears the console (the log buffer)", [](const std::vector<std::string>&){
            Log::clearEntries();
        });
        add("stats", "stats off|fps|full: the stats panel", [](const std::vector<std::string>& args){
            const std::string mode = args.empty() ? std::string() : lower(args[0]);
            if (mode == "off") DebugOverlay::setStatsMode(Stats::Off);
            else if (mode == "fps") DebugOverlay::setStatsMode(Stats::Fps);
            else if (mode == "full") DebugOverlay::setStatsMode(Stats::Full);
            else if (mode.empty()) DebugOverlay::cycleStatsMode();
            else Log::warn("stats: off, fps or full (not '%s')", args[0].c_str());
        });
        add("timescale", "timescale <x>: Engine.timeScale (0 freezes, 1 normal)", [](const std::vector<std::string>& args){
            if (args.empty()){
                Log::print("timescale %g", Engine::getTimeScale());
                return;
            }
            char* end = nullptr;
            const double value = std::strtod(args[0].c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(value)){
                Log::warn("timescale: '%s' is not a number", args[0].c_str());
                return;
            }
            Engine::setTimeScale((float)value);
            Log::print("timescale %g", Engine::getTimeScale());
        });
        add("scene", "scene <name>: loads that scene (SceneManager.loadScene)", [](const std::vector<std::string>& args){
            if (args.empty()){
                Log::warn("scene: give the name of a scene");
                return;
            }
            std::string name = args[0];
            for (size_t i = 1; i < args.size(); i++)
                name += " " + args[i];
            if (!SceneManager::loadScene(name))
                Log::warn("scene: no scene named '%s'", name.c_str());
        });
        add("lua", "lua <code>: runs Lua (Project Settings > Debug > Allow Lua Commands)", [](const std::vector<std::string>& args){
            if (!state().settings.allowLua){
                Log::warn("lua: Lua commands are off (Project Settings > Debug > Allow Lua Commands)");
                return;
            }
            lua_State* L = LuaBinding::getLuaState();
            if (!L){
                Log::warn("lua: no Lua state");
                return;
            }
            std::string code;
            for (size_t i = 0; i < args.size(); i++)
                code += (i ? " " : "") + args[i];
            const int top = lua_gettop(L);
            // an expression prints its value, as Lua's own prompt does
            const std::string expression = "return " + code;
            if (luaL_loadbuffer(L, expression.c_str(), expression.size(), "=console") != LUA_OK){
                lua_pop(L, 1);
                if (luaL_loadbuffer(L, code.c_str(), code.size(), "=console") != LUA_OK){
                    const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                    lua_settop(L, top);
                    Log::error("%s", error.c_str());
                    return;
                }
            }
            if (LuaBinding::pcallWithTraceback(L, 0, LUA_MULTRET) != LUA_OK){
                const std::string error = LuaBinding::getLuaStackErrorString(L, -1);
                lua_settop(L, top);
                Log::error("Lua Error: %s", error.c_str());
                return;
            }
            std::string results;
            for (int i = top + 1; i <= lua_gettop(L); i++){
                size_t length = 0;
                const char* text = luaL_tolstring(L, i, &length);
                if (i > top + 1) results += "\t";
                results.append(text, length);
                lua_pop(L, 1);
            }
            lua_settop(L, top);
            if (!results.empty())
                Log::print("%s", results.c_str());
        });
    }

    Command* findCommand(const std::string& name){
        for (Command& command : state().commands){
            if (command.name == name)
                return &command;
        }
        return nullptr;
    }

    // ---- console actions

    void copyToClipboard(){
        OverlayState& s = state();
        std::string text;
        bool anyExpanded = false;
        for (const Row& row : s.rows){
            if (row.expanded){
                anyExpanded = true;
                break;
            }
        }
        for (const Row& row : s.rows){
            if (anyExpanded && !row.expanded)
                continue;
            if (!text.empty()) text += "\n";
            text += rowText(row);
        }
        size_t copied = 0;
        for (const Row& row : s.rows){
            if (!anyExpanded || row.expanded) copied++;
        }
        if (System::hasInstance())
            System::instance().setClipboardText(text);
        Log::print("Debug overlay: %zu line%s copied", copied, copied == 1 ? "" : "s");
    }

    void saveToFile(){
        std::string text;
        for (const LogEntry& entry : Log::getEntries()){
            text += entryText(entry);
            text += "\n";
        }
        const std::time_t now = std::time(nullptr);
        std::tm local = {};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "data://logs/log-%Y%m%d-%H%M%S.txt", &local);
        std::string error;
        if (!FileUtils::writeText(name, text, error)){
            Log::error("Debug overlay: the log could not be saved (%s)", error.c_str());
            return;
        }
        std::string normalized, systemPath;
        FileUtils::resolve(name, false, normalized, systemPath, error);
        Log::print("log saved to %s", systemPath.empty() ? name : systemPath.c_str());
    }

    void runCommandField(){
        const std::string line = inputValue("command");
        if (line.empty())
            return;
        setInputValue("command", "");
        DebugOverlay::executeCommand(line);
        state().stickToBottom = true;
    }

    Row* rowOf(Rml::Element* element){
        for (Rml::Element* at = element; at; at = at->GetParentNode()){
            if (at->IsClassSet("row")){
                for (Row& row : state().rows){
                    if (row.element == at)
                        return &row;
                }
                return nullptr;
            }
        }
        return nullptr;
    }

    // the id of the button an element is in ("" when none)
    std::string buttonOf(Rml::Element* element){
        for (Rml::Element* at = element; at; at = at->GetParentNode()){
            if (at->IsClassSet("btn") || at->GetId() == "popup")
                return at->GetId();
        }
        return std::string();
    }

    void OverlayListener::ProcessEvent(Rml::Event& event){
        OverlayState& s = state();
        Rml::Element* target = event.GetTargetElement();
        switch (event.GetId()){
            case Rml::EventId::Click: {
                const std::string button = buttonOf(target);
                if (button == "popup"){
                    if (!s.popupDragged)
                        DebugOverlay::showConsole();
                    s.popupDragged = false;
                }else if (button == "filter-info"){
                    s.showInfo = !s.showInfo;
                    s.rebuild = true;
                }else if (button == "filter-warn"){
                    s.showWarn = !s.showWarn;
                    s.rebuild = true;
                }else if (button == "filter-error"){
                    s.showError = !s.showError;
                    s.rebuild = true;
                }else if (button == "collapse"){
                    s.collapse = !s.collapse;
                    s.rebuild = true;
                }else if (button == "clear"){
                    Log::clearEntries();
                }else if (button == "copy"){
                    copyToClipboard();
                }else if (button == "save"){
                    saveToFile();
                }else if (button == "stats-button"){
                    DebugOverlay::cycleStatsMode();
                }else if (button == "size"){
                    s.fullHeight = !s.fullHeight;
                }else if (button == "close"){
                    DebugOverlay::hideConsole();
                }else if (Row* row = rowOf(target)){
                    row->expanded = !row->expanded;
                    fillRow(*row);
                    // reading an old line: the list stays where it is
                    s.stickToBottom = false;
                }
                s.layoutChanged = true;
                break;
            }
            case Rml::EventId::Change: {
                if (!target)
                    break;
                if (target->GetId() == "search"){
                    const std::string search = lower(inputValue("search"));
                    if (search != s.search){
                        s.search = search;
                        s.rebuild = true;
                    }
                }else if (target->GetId() == "command" && event.GetParameter<bool>("linebreak", false)){
                    runCommandField();
                }
                break;
            }
            case Rml::EventId::Scroll: {
                if (target && target->GetId() == "list")
                    s.stickToBottom = listAtBottom(target);
                break;
            }
            case Rml::EventId::Dragstart: {
                if (target && target->GetId() == "popup"){
                    s.popupDragging = true;
                    s.popupDragged = false;
                    s.popupDragStartY = event.GetParameter<float>("mouse_y", 0.0f);
                    s.popupDragStartTop = s.popupTop;
                }
                break;
            }
            case Rml::EventId::Drag: {
                if (s.popupDragging && s.document){
                    const float height = std::max(1.0f, s.document->GetClientHeight());
                    const float dy = event.GetParameter<float>("mouse_y", 0.0f) - s.popupDragStartY;
                    if (std::fabs(dy) > 6.0f)
                        s.popupDragged = true;
                    s.popupTop = std::clamp(s.popupDragStartTop + dy / height, 0.0f, 0.9f);
                    updatePopup();
                }
                break;
            }
            case Rml::EventId::Dragend: {
                s.popupDragging = false;
                break;
            }
            default:
                break;
        }
    }

    void warnNotEnabled(const char* function){
        OverlayState& s = state();
        if (s.warnedDisabled)
            return;
        s.warnedDisabled = true;
        Log::warn("DebugOverlay.%s: the overlay is not enabled (Project Settings > Debug > Overlay, or DebugOverlay.enabled = true)", function);
    }

    int parseKeySetting(const std::string& name, const char* setting){
        if (name.empty())
            return D_KEY_UNKNOWN;
        const int key = InputControlNames::parseKey(name);
        if (key == D_KEY_UNKNOWN)
            Log::warn("Debug overlay: %s '%s' is not a key name (backquote, f3, f12...)", setting, name.c_str());
        return key;
    }

    void resetGestures(){
        OverlayState& s = state();
        s.touches.clear();
        s.touchStarts.clear();
        s.holdStart = -1.0;
        s.holdFired = false;
        s.tapArmed = false;
    }
}

// ---------------------------------------------------------------- settings and state

void DebugOverlay::configure(const DebugOverlaySettings& settings){
    OverlayState& s = state();
    s.settings = settings;
    s.toggleKey = parseKeySetting(settings.toggleKey, "the toggle key");
    s.statsKey = parseKeySetting(settings.statsKey, "the stats key");
    s.statsMode = settings.stats;
    s.consoleVisible = false;
    s.loadFailed = false;
    s.warnedDisabled = false;
    s.layoutChanged = true;
    setEnabled(settings.enabled);
}

const DebugOverlaySettings& DebugOverlay::getSettings(){
    return state().settings;
}

void DebugOverlay::setEnabled(bool enabled){
    OverlayState& s = state();
    if (enabled && !s.enabled){
        s.errorsAtEnable = Log::getCount(LogLevel::Error);
        s.openedOnError = false;
        s.loadFailed = false;
    }
    if (!enabled){
        // the Scene goes at the next update
        s.consoleVisible = false;
        resetGestures();
    }
    s.enabled = enabled;
    s.layoutChanged = true;
}

bool DebugOverlay::isEnabled(){
    return state().enabled;
}

void DebugOverlay::showConsole(){
    OverlayState& s = state();
    if (!s.enabled){
        warnNotEnabled("showConsole");
        return;
    }
    if (!s.consoleVisible){
        s.consoleVisible = true;
        s.warnAtOpen = Log::getCount(LogLevel::Warn);
        s.errorAtOpen = Log::getCount(LogLevel::Error);
        s.stickToBottom = true;
        s.scrollToBottom = true;
        s.layoutChanged = true;
    }
}

void DebugOverlay::hideConsole(){
    OverlayState& s = state();
    if (!s.enabled){
        warnNotEnabled("hideConsole");
        return;
    }
    if (s.consoleVisible){
        s.consoleVisible = false;
        // the keys are the game's again
        if (s.document && s.document->GetContext()){
            if (Rml::Element* focus = s.document->GetContext()->GetFocusElement()){
                if (focus->GetOwnerDocument() == s.document)
                    focus->Blur();
            }
        }
        s.layoutChanged = true;
    }
}

void DebugOverlay::toggleConsole(){
    if (!state().enabled){
        warnNotEnabled("toggleConsole");
        return;
    }
    if (isConsoleVisible())
        hideConsole();
    else
        showConsole();
}

bool DebugOverlay::isConsoleVisible(){
    const OverlayState& s = state();
    return s.enabled && s.consoleVisible;
}

void DebugOverlay::setStatsMode(DebugOverlaySettings::Stats mode){
    OverlayState& s = state();
    if (s.statsMode != mode){
        s.statsMode = mode;
        s.layoutChanged = true;
        s.nextText = 0.0;
        s.nextGraph = 0.0;
    }
}

DebugOverlaySettings::Stats DebugOverlay::getStatsMode(){
    return state().statsMode;
}

void DebugOverlay::cycleStatsMode(){
    switch (getStatsMode()){
        case Stats::Off: setStatsMode(Stats::Fps); break;
        case Stats::Fps: setStatsMode(Stats::Full); break;
        default: setStatsMode(Stats::Off); break;
    }
}

void DebugOverlay::setSafeAreaInsetsProvider(InsetsFunction provider){
    state().safeArea = provider;
    state().layoutChanged = true;
}

// ---------------------------------------------------------------- commands

bool DebugOverlay::addCommand(const std::string& name, const std::string& description,
                              std::function<void(const std::vector<std::string>&)> function, const std::string& ownerTag){
    OverlayState& s = state();
    if (s.commands.empty())
        builtInCommands();
    if (!validCommandName(name)){
        Log::error("DebugOverlay.addCommand: '%s' is not a command name (lower-case letters, digits, '_' and '-')", name.c_str());
        return false;
    }
    if (!function){
        Log::error("DebugOverlay.addCommand: '%s' has no function", name.c_str());
        return false;
    }
    if (Command* existing = findCommand(name)){
        if (existing->builtIn){
            Log::error("DebugOverlay.addCommand: '%s' is a built-in command", name.c_str());
            return false;
        }
        existing->description = description;
        existing->function = std::move(function);
        existing->ownerTag = ownerTag;
        return true;
    }
    Command command;
    command.name = name;
    command.description = description;
    command.function = std::move(function);
    command.ownerTag = ownerTag;
    s.commands.push_back(std::move(command));
    return true;
}

void DebugOverlay::removeCommand(const std::string& name){
    std::vector<Command>& commands = state().commands;
    commands.erase(std::remove_if(commands.begin(), commands.end(), [&name](const Command& c){ return !c.builtIn && c.name == name; }),
                   commands.end());
}

bool DebugOverlay::hasCommand(const std::string& name){
    if (state().commands.empty())
        builtInCommands();
    return findCommand(name) != nullptr;
}

std::vector<std::pair<std::string, std::string>> DebugOverlay::getCommands(){
    OverlayState& s = state();
    if (s.commands.empty())
        builtInCommands();
    std::vector<std::pair<std::string, std::string>> out;
    for (const Command& command : s.commands){
        if (command.name == "lua" && !s.settings.allowLua)
            continue;
        out.push_back({command.name, command.description});
    }
    return out;
}

void DebugOverlay::executeCommand(const std::string& line){
    OverlayState& s = state();
    if (s.commands.empty())
        builtInCommands();
    const std::vector<std::string> words = splitCommandLine(line);
    if (words.empty())
        return;
    Log::print("> %s", line.c_str());
    const std::string name = lower(words[0]);
    Command* command = findCommand(name);
    if (!command){
        Log::warn("unknown command '%s' (type help)", words[0].c_str());
        return;
    }
    // `lua`: the code as typed, quotes included
    std::vector<std::string> args;
    if (name == "lua"){
        const size_t at = line.find_first_not_of(" \t");
        const size_t afterName = at == std::string::npos ? std::string::npos : line.find_first_of(" \t", at);
        if (afterName != std::string::npos){
            const size_t codeStart = line.find_first_not_of(" \t", afterName);
            if (codeStart != std::string::npos)
                args.push_back(line.substr(codeStart));
        }
    }else{
        args.assign(words.begin() + 1, words.end());
    }
    // a copy: the command may remove itself
    std::function<void(const std::vector<std::string>&)> function = command->function;
    function(args);
}

void DebugOverlay::removeCommandsByTag(const std::string& substring){
    if (substring.empty())
        return;
    std::vector<Command>& commands = state().commands;
    commands.erase(std::remove_if(commands.begin(), commands.end(), [&substring](const Command& c){
        return !c.builtIn && !c.ownerTag.empty() && c.ownerTag.find(substring) != std::string::npos;
    }), commands.end());
}

void DebugOverlay::removeOwnedCommands(){
    std::vector<Command>& commands = state().commands;
    commands.erase(std::remove_if(commands.begin(), commands.end(), [](const Command& c){ return !c.builtIn && !c.ownerTag.empty(); }),
                   commands.end());
}

// ---------------------------------------------------------------- engine hooks

void DebugOverlay::handleKey(int key, int mods, bool down, bool repeat){
    (void)mods;
    OverlayState& s = state();
    if (!s.enabled || !down || repeat || key == D_KEY_UNKNOWN)
        return;
    if (key == s.toggleKey)
        toggleConsole();
    else if (key == s.statsKey)
        cycleStatsMode();
}

void DebugOverlay::handleTouch(int pointer, float x, float y, int phase){
    OverlayState& s = state();
    if (!s.enabled)
        return;
    using Gesture = DebugOverlaySettings::Gesture;
    const Gesture gesture = s.settings.gesture;
    if (gesture == Gesture::None)
        return;

    // a finger that slides is not a hold: about 4% of the shorter side of the screen
    float slop = 24.0f;
    if (System::hasInstance())
        slop = std::max(slop, 0.04f * (float)std::min(System::instance().getScreenWidth(), System::instance().getScreenHeight()));

    switch (phase){
        case 0: {
            s.touches[pointer] = {x, y};
            s.touchStarts[pointer] = {x, y};
            const size_t count = s.touches.size();
            if (gesture == Gesture::ThreeFingerHold){
                if (count == 3){
                    s.holdStart = s.clock;
                    s.holdFired = false;
                }else{
                    s.holdStart = -1.0;
                }
            }else if (gesture == Gesture::FourFingerTap){
                if (count == 1){
                    s.tapStart = s.clock;
                    s.tapArmed = false;
                }
                if (count == 4)
                    s.tapArmed = s.clock - s.tapStart <= tapSeconds;
                else if (count > 4)
                    s.tapArmed = false;
            }
            break;
        }
        case 1: {
            auto it = s.touches.find(pointer);
            if (it == s.touches.end())
                break;
            it->second = {x, y};
            const OverlayState::TouchPoint start = s.touchStarts[pointer];
            if (std::hypot(x - start.x, y - start.y) > slop){
                s.holdStart = -1.0;
                s.tapArmed = false;
            }
            break;
        }
        default: {
            s.touches.erase(pointer);
            s.touchStarts.erase(pointer);
            if (phase == 3)
                s.tapArmed = false;
            if (gesture == Gesture::ThreeFingerHold && s.touches.size() < 3)
                s.holdStart = -1.0;
            if (gesture == Gesture::FourFingerTap && s.touches.empty()){
                if (s.tapArmed && s.clock - s.tapStart <= tapSeconds)
                    toggleConsole();
                s.tapArmed = false;
            }
            break;
        }
    }
}

bool DebugOverlay::needsUpdate(){
    const OverlayState& s = state();
    return s.enabled || s.scene != nullptr;
}

void DebugOverlay::update(double unscaledDelta){
    OverlayState& s = state();
    if (!s.enabled){
        // turned off at runtime: the Scene goes
        if (s.scene)
            destroyScene();
        return;
    }
    s.clock += std::max(0.0, unscaledDelta);

    // the three-finger hold
    if (s.holdStart >= 0.0 && !s.holdFired && s.touches.size() == 3 && s.clock - s.holdStart >= holdSeconds){
        s.holdFired = true;
        toggleConsole();
    }

    // the first error opens the console
    if (s.settings.openOnError && !s.openedOnError && Log::getCount(LogLevel::Error) > s.errorsAtEnable){
        s.openedOnError = true;
        showConsole();
    }

    const bool visible = anythingVisible();
    if (!s.scene){
        if (!visible || s.loadFailed || !Engine::isViewLoaded())
            return;
        createScene();
        if (!s.scene)
            return;
    }

    // nothing shows: the document is hidden and nothing is drawn
    UIDocumentComponent* component = s.scene->findComponent<UIDocumentComponent>(s.entity);
    if (component && component->enabled != visible){
        component->enabled = visible;
        // hidden now: once, so the document stops drawing
        if (!visible)
            s.scene->getSystem<UIDocumentSystem>()->update(0.0);
    }
    if (!visible)
        return;

    const float scale = overlayScale();
    const Rect view = Engine::getViewRect();
    s.scene->getSystem<UIDocumentSystem>()->setReferenceSize(std::max(1, (int)std::round(view.getWidth() / scale)),
                                                           std::max(1, (int)std::round(view.getHeight() / scale)));

    if (s.layoutChanged)
        applyLayout();
    applySafeArea(scale);
    if (s.consoleVisible){
        updateRows();
        updateCounts();
    }
    if (popupWanted())
        updatePopup();
    if (s.statsMode != Stats::Off){
        if (s.clock >= s.nextText){
            updateStatsText();
            s.nextText = s.clock + textInterval;
        }
        if (s.clock >= s.nextGraph){
            updateGraph();
            s.nextGraph = s.clock + graphInterval;
        }
    }

    s.scene->update(unscaledDelta);

    // after the layout: the newest line in view
    if (s.consoleVisible && s.scrollToBottom){
        if (Rml::Element* list = byId("list"))
            list->SetScrollTop(std::max(0.0f, list->GetScrollHeight() - list->GetClientHeight()));
        s.scrollToBottom = false;
    }
}

void DebugOverlay::draw(){
    OverlayState& s = state();
    if (s.scene && s.enabled && anythingVisible())
        s.scene->draw();
}

Scene* DebugOverlay::getScene(){
    return state().scene.get();
}

Entity DebugOverlay::getDocumentEntity(){
    return state().scene ? state().entity : NULL_ENTITY;
}

bool DebugOverlay::hasTextFocus(){
    OverlayState& s = state();
    if (!s.document || !s.consoleVisible || !s.document->GetContext())
        return false;
    Rml::Element* focus = s.document->GetContext()->GetFocusElement();
    return focus && (focus->GetId() == "search" || focus->GetId() == "command") && focus->GetOwnerDocument() == s.document;
}

void DebugOverlay::viewDestroyed(){
    destroyScene();
    resetGestures();
}

void DebugOverlay::shutdown(){
    OverlayState& s = state();
    destroyScene();
    resetGestures();
    removeOwnedCommands();
    s.enabled = false;
    s.consoleVisible = false;
    s.statsMode = Stats::Off;
    s.search.clear();
    s.showInfo = s.showWarn = s.showError = true;
    s.collapse = false;
    s.fullHeight = false;
    s.loadFailed = false;
    s.warnedDisabled = false;
}
