// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The debug overlay's document, as C++ raw strings (the user-agent sheet's precedent,
// htmlui/compat/ua): RML and RCSS of their own, no project file, the built-in Roboto only.
// Lengths are rem (1rem = 13 overlay px): DebugOverlay sets the body's font size so an overlay px
// is the same number of screen pixels whatever the game's canvas is.

#include "DebugOverlayResources.h"

#include <cstring>

using namespace jasperengine;

namespace {

    const char* const overlayRml = R"rml(<rml>
<head>
    <title>JasperEngine Debug Overlay</title>
    <link type="text/rcss" href="overlay.rcss"/>
</head>
<body id="overlay-body" style="font-size: 13px;">
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
        <div id="list-box"><div id="list"><div id="list-top"></div><div id="list-rows"></div><div id="list-bottom"></div></div></div>
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
    font-family: Roboto; line-height: 1.3; color: #e6e8ee;
}

/* ---- stats */
#stats {
    position: absolute; padding: 0.3077rem 0.4615rem; background-color: #0b0d12c8; border-radius: 0.3077rem;
    pointer-events: none; min-width: 11.5385rem;
}
#stats.corner-tl { left: 0.3077rem; top: 0.3077rem; }
#stats.corner-tr { right: 0.3077rem; top: 0.3077rem; }
#stats.corner-bl { left: 0.3077rem; bottom: 0.3077rem; }
#stats.corner-br { right: 0.3077rem; bottom: 0.3077rem; }
#stats-line { font-size: 1.0769rem; white-space: nowrap; color: #ffffff; }
#stats-line.slow { color: #ffcc55; }
#stats-line.very-slow { color: #ff6b6b; }
#graph { position: relative; width: 18.4615rem; height: 3.0769rem; margin-top: 0.2308rem; background-color: #ffffff12; }
#graph-bars { position: absolute; left: 0px; top: 0px; right: 0px; bottom: 0px; }
.bar { position: absolute; bottom: 0px; width: 0.1538rem; background-color: #5fd38d; }
.bar.slow { background-color: #ffcc55; }
.bar.very-slow { background-color: #ff6b6b; }
.graph-line { position: absolute; left: 0px; right: 0px; height: 0.0769rem; background-color: #ffffff40; }
#stats-table { margin-top: 0.3077rem; font-size: 0.9231rem; }
.stat-row { white-space: nowrap; height: 1.2308rem; }
.stat-name { display: inline-block; width: 6.9231rem; color: #9aa3b5; }
.stat-value { display: inline-block; color: #ffffff; }

/* ---- popup */
#popup {
    position: absolute; right: 0px; top: 45%; width: 3.6923rem; padding: 0.3077rem 0px;
    background-color: #0b0d12e0; border-radius: 0.4615rem 0px 0px 0.4615rem; text-align: center; drag: drag;
}
#popup:active { background-color: #232838f0; }
#popup div { font-size: 1.0769rem; height: 1.5385rem; }

/* ---- console */
.count-warn { color: #ffcc55; }
.count-error { color: #ff6b6b; }
#console {
    position: absolute; left: 0px; top: 0px; right: 0px; height: 50%;
    display: flex; flex-direction: column; background-color: #0b0d12ee;
    border-bottom: 0.0769rem #ffffff30;
}
#console.full { height: 100%; }
#toolbar { display: flex; flex-direction: row; flex-wrap: wrap; flex: 0 0 auto; padding: 0.2308rem; }
.btn {
    padding: 0.4615rem 0.6923rem; margin: 0.1538rem; border-radius: 0.3077rem; background-color: #ffffff14; color: #cfd3dc;
    white-space: nowrap; cursor: pointer;
}
.btn:hover { background-color: #ffffff22; }
.btn:active { background-color: #ffffff38; }
.btn.toggle { color: #7d8496; }
.btn.toggle.on { color: #ffffff; background-color: #3d6bff55; }
.badge { color: #9aa3b5; }
#searchbar, #commandbar { flex: 0 0 auto; padding: 0.1538rem 0.3846rem; }
input {
    display: block; width: 100%; height: 2rem; padding: 0.3077rem 0.4615rem; box-sizing: border-box;
    background-color: #ffffff12; border-radius: 0.3077rem; color: #ffffff; font-size: 1rem; cursor: text;
}
input:focus { background-color: #ffffff22; }
/* the list's box takes the console's free height; the list is absolute in it, so a row added
   lays out the list alone, not the console's flex box around it */
#list-box { flex: 1 1 0px; position: relative; }
#list { position: absolute; left: 0px; top: 0px; right: 0px; bottom: 0px; overflow-y: auto; padding: 0px 0.3077rem; }
#empty { flex: 0 0 auto; padding: 0.4615rem 0.7692rem; color: #7d8496; }
#list-top, #list-bottom { height: 0px; }
/* a row: its level as the left border, then the time, the message and a count (few elements;
   the list makes only the rows in view, DebugOverlay.cpp's syncWindow; its height estimate
   follows these lengths) */
.row {
    padding: 0.2308rem 0.3077rem 0.2308rem 0.4615rem; border-left: 0.4615rem #7d8496; border-bottom: 0.0769rem #ffffff0d;
    white-space: nowrap; overflow: hidden; height: 1.3077rem;
}
.row.expanded { height: auto; }
.row:hover { background-color: #ffffff0a; }
.row.expanded { background-color: #ffffff12; }
.row.lvl-warn { border-left-color: #ffcc55; }
.row.lvl-error { border-left-color: #ff6b6b; }
.row.lvl-trace, .row.lvl-debug { border-left-color: #4d5466; }
.row.lvl-warn { color: #ffe3a3; }
.row.lvl-error { color: #ffb3b3; }
.time { color: #7d8496; margin-right: 0.4615rem; }
.count { margin-left: 0.4615rem; padding: 0px 0.3846rem; border-radius: 0.4615rem; background-color: #ffffff22; color: #ffffff; }
.detail { padding: 0.3077rem 0.3077rem 0.3077rem 1.3846rem; white-space: pre-wrap; word-break: break-word; color: #cfd3dc; }
.trace { color: #9aa3b5; margin-top: 0.2308rem; }

scrollbarvertical { width: 0.4615rem; }
scrollbarvertical sliderbar { background-color: #ffffff40; border-radius: 0.2308rem; min-height: 1.5385rem; }
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
