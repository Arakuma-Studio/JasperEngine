// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The debug overlay's document, as C++ raw strings (the user-agent sheet's precedent,
// htmlui/compat/ua): RML and RCSS of their own, no project file, the built-in Roboto only.
// Sizes are overlay px: DebugOverlay lays the document out at the view size divided by its
// scale, so text stays readable whatever the game's canvas is.

#include "DebugOverlayResources.h"

#include <cstring>

using namespace jasperengine;

namespace {

    const char* const overlayRml = R"rml(<rml>
<head>
    <title>JasperEngine Debug Overlay</title>
    <link type="text/rcss" href="overlay.rcss"/>
</head>
<body id="overlay-body">
    <div id="stats" class="hidden corner-tl">
        <div id="stats-line">-- FPS</div>
        <div id="graph"><div id="graph-bars"></div><div class="graph-line l30"></div><div class="graph-line l60"></div></div>
        <div id="stats-table"></div>
    </div>
    <div id="popup" class="hidden"><div id="popup-warn" class="count-warn">0</div><div id="popup-error" class="count-error">0</div></div>
    <div id="console" class="hidden half">
        <div id="toolbar">
            <div id="filter-info" class="btn toggle on">Info <span id="count-info" class="badge">0</span></div>
            <div id="filter-warn" class="btn toggle on">Warn <span id="count-warn" class="badge count-warn">0</span></div>
            <div id="filter-error" class="btn toggle on">Error <span id="count-error" class="badge count-error">0</span></div>
            <div id="collapse" class="btn toggle">Collapse</div>
            <div id="clear" class="btn">Clear</div>
            <div id="copy" class="btn">Copy</div>
            <div id="save" class="btn">Save</div>
            <div id="stats-button" class="btn">Stats</div>
            <div id="size" class="btn">Full</div>
            <div id="close" class="btn">Close</div>
        </div>
        <div id="searchbar"><input type="text" id="search" placeholder="Search messages and traces"/></div>
        <div id="list"></div>
        <div id="empty" class="hidden">No messages</div>
        <div id="commandbar"><input type="text" id="command" placeholder="Command (type help)"/></div>
    </div>
</body>
</rml>
)rml";

    const char* const overlayRcss = R"rcss(
body, div { display: block; }
span { display: inline; }
body {
    position: absolute; left: 0px; top: 0px; right: 0px; bottom: 0px; margin: 0px;
    font-family: Roboto; font-size: 13px; line-height: 1.3; color: #e6e8ee;
}

/* ---- stats */
#stats {
    position: absolute; padding: 4px 6px; background-color: #0b0d12c8; border-radius: 4px;
    pointer-events: none; min-width: 150px;
}
#stats.corner-tl { left: 4px; top: 4px; }
#stats.corner-tr { right: 4px; top: 4px; }
#stats.corner-bl { left: 4px; bottom: 4px; }
#stats.corner-br { right: 4px; bottom: 4px; }
#stats-line { font-size: 14px; white-space: nowrap; color: #ffffff; }
#stats-line.slow { color: #ffcc55; }
#stats-line.very-slow { color: #ff6b6b; }
#graph { position: relative; width: 240px; height: 40px; margin-top: 3px; background-color: #ffffff12; }
#graph-bars { position: absolute; left: 0px; top: 0px; right: 0px; bottom: 0px; }
.bar { position: absolute; bottom: 0px; width: 2px; background-color: #5fd38d; }
.bar.slow { background-color: #ffcc55; }
.bar.very-slow { background-color: #ff6b6b; }
.graph-line { position: absolute; left: 0px; right: 0px; height: 1px; background-color: #ffffff40; }
#stats-table { margin-top: 4px; font-size: 12px; }
.stat-row { white-space: nowrap; height: 16px; }
.stat-name { display: inline-block; width: 90px; color: #9aa3b5; }
.stat-value { display: inline-block; color: #ffffff; }

/* ---- popup */
#popup {
    position: absolute; right: 0px; top: 45%; width: 48px; padding: 4px 0px;
    background-color: #0b0d12e0; border-radius: 6px 0px 0px 6px; text-align: center; drag: drag;
}
#popup:active { background-color: #232838f0; }
#popup div { font-size: 14px; height: 20px; }

/* ---- console */
.count-warn { color: #ffcc55; }
.count-error { color: #ff6b6b; }
#console {
    position: absolute; left: 0px; top: 0px; right: 0px; height: 50%;
    display: flex; flex-direction: column; background-color: #0b0d12ee;
    border-bottom: 1px #ffffff30;
}
#console.full { height: 100%; }
#toolbar { display: flex; flex-direction: row; flex-wrap: wrap; flex: 0 0 auto; padding: 3px; }
.btn {
    padding: 6px 9px; margin: 2px; border-radius: 4px; background-color: #ffffff14; color: #cfd3dc;
    white-space: nowrap; cursor: pointer;
}
.btn:hover { background-color: #ffffff22; }
.btn:active { background-color: #ffffff38; }
.btn.toggle { color: #7d8496; }
.btn.toggle.on { color: #ffffff; background-color: #3d6bff55; }
.badge { color: #9aa3b5; }
#searchbar, #commandbar { flex: 0 0 auto; padding: 2px 5px; }
input {
    display: block; width: 100%; height: 26px; padding: 4px 6px; box-sizing: border-box;
    background-color: #ffffff12; border-radius: 4px; color: #ffffff; font-size: 13px; cursor: text;
}
input:focus { background-color: #ffffff22; }
#list { flex: 1 1 0px; overflow-y: auto; padding: 0px 4px; }
#empty { flex: 0 0 auto; padding: 6px 10px; color: #7d8496; }
.row { padding: 3px 4px; border-bottom: 1px #ffffff0d; }
.row:hover { background-color: #ffffff0a; }
.row.expanded { background-color: #ffffff12; }
.line { white-space: nowrap; overflow: hidden; height: 17px; }
.mark { display: inline-block; width: 6px; height: 12px; margin-right: 6px; border-radius: 2px; background-color: #7d8496; }
.lvl-warn .mark { background-color: #ffcc55; }
.lvl-error .mark { background-color: #ff6b6b; }
.lvl-verbose .mark, .lvl-debug .mark { background-color: #4d5466; }
.time { display: inline-block; color: #7d8496; margin-right: 6px; }
.lvl-warn .msg { color: #ffe3a3; }
.lvl-error .msg { color: #ffb3b3; }
.count { display: inline-block; margin-left: 6px; padding: 0px 5px; border-radius: 6px; background-color: #ffffff22; color: #ffffff; }
.detail { padding: 4px 4px 4px 18px; white-space: pre-wrap; word-break: break-word; color: #cfd3dc; }
.trace { color: #9aa3b5; margin-top: 3px; }

scrollbarvertical { width: 6px; }
scrollbarvertical sliderbar { background-color: #ffffff40; border-radius: 3px; min-height: 20px; }
scrollbarhorizontal { height: 0px; }

/* last, and as specific as the rules that set a display: hidden panels take no input */
.hidden, #console.hidden, #stats.hidden, #popup.hidden, .count.hidden { display: none; }
)rcss";

    struct Resource {
        const char* name;
        const char* data;
    };

    const Resource resources[] = {
        {"debug/overlay.rml", overlayRml},
        {"debug/overlay.rcss", overlayRcss},
    };

    // "engine://x" or RmlUi's "engine|//x": x
    bool resourceName(const std::string& path, std::string& name){
        for (const char* scheme : {"engine://", "engine|//", "/engine://", "/engine|//"}){
            const size_t length = std::strlen(scheme);
            if (path.compare(0, length, scheme) == 0){
                name = path.substr(length);
                return true;
            }
        }
        return false;
    }

}

bool EngineResources::isEnginePath(const std::string& path){
    std::string name;
    return resourceName(path, name);
}

bool EngineResources::find(const std::string& path, const char*& data, size_t& size){
    std::string name;
    if (!resourceName(path, name))
        return false;
    for (const Resource& resource : resources){
        if (name == resource.name){
            data = resource.data;
            size = std::strlen(resource.data);
            return true;
        }
    }
    return false;
}
