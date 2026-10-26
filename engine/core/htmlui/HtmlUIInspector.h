// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIINSPECTOR_H
#define HTMLUIINSPECTOR_H

#include "Export.h"
#include "ecs/Entity.h"
#include "htmlui/compat/Diagnostics.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace jasperengine {
class Scene;
}

namespace jasperengine::htmlui {

// Read-only introspection of live HTML UI documents for tools (MCP inspect_ui / assert_ui /
// send_input target). Plain structs only: no RmlUi type crosses this header. Coordinates are CSS
// px = canvas px, origin top-left of the document's viewport. Main thread only. Every query
// first brings the document's layout up to date (Rml::ElementDocument::UpdateDocument).
// Paths are asset-relative ("ui/menu.css"), as everywhere in the engine: the editor prefixes the
// assets directory at its boundary (CONTRACTS "Paths").
struct InspectRect { float x = 0, y = 0, width = 0, height = 0; };
struct InspectEdges { float top = 0, right = 0, bottom = 0, left = 0; };

struct InspectFlags {
    bool overflowX = false;    // scroll width  > client width  + 0.5 (text or children wider than the content box)
    bool overflowY = false;    // scroll height > client height + 0.5
    bool clipped = false;      // part of the border box is cut by an ancestor with overflow != visible
    bool hidden = false;       // display: none on it or an ancestor, or visibility: hidden (computed, so inherited)
    bool transparent = false;  // effective opacity (product with ancestors) < 0.05
    bool offscreen = false;    // border box entirely outside the viewport
    bool zeroSize = false;     // border box width or height < 1
};

// A UI effect the element uses (phase 5, UIElement::getEffects): the property
// ("background-image", "mask-image", "filter", "backdrop-filter"), the 1-based index of the layer
// or function in it, the form ("paint"/"filter") and every parameter's canonical CSS text
struct InspectEffect {
    std::string property;
    int index = 0;
    std::string name;
    std::string form;
    std::vector<std::pair<std::string, std::string>> params;
    // phase 6: a stateful effect's simulation (its state images' bytes, steps since its reset,
    // simulation time in s, paused: not drawn last frame)
    bool stateful = false;
    size_t stateBytes = 0;
    int64_t steps = 0;
    double simTime = 0.0;
    bool paused = false;
    // its state images, pinned-clock replays run (one still running), splats queued or logged,
    // and the scripted pointer path's samples (MCP set_ui_effect_time)
    int stateImages = 0;
    uint64_t replays = 0;
    bool replaying = false;
    int splats = 0;
    int pathSamples = 0;
};

// The state of a <video> (phase 6, p6-video): HTMLMediaElement's values, the decoder's statistics,
// and the rectangles of its default controls (non-DOM parts: send_input targets)
struct InspectMedia {
    std::string currentSrc;
    double currentTime = 0, duration = 0, playbackRate = 1;   // duration: -1 before the metadata
    bool paused = true, ended = false, seeking = false, loop = false, muted = false, showingPoster = false;
    float volume = 1;
    int readyState = 0, networkState = 0, videoWidth = 0, videoHeight = 0;
    int errorCode = 0;
    std::string errorMessage;
    int frame = -1;                    // index of the frame on screen
    uint64_t droppedFrames = 0;
    double decodeMs = 0;               // average decode time per frame
    size_t memoryBytes = 0;
    bool controls = false, controlsVisible = false;
    InspectRect controlsBar, playButton, timeline, muteButton;
};

// A virtual view's window (S17 R39): <list-view>, <grid-view>, <tree-view>, <table-view>. Indices
// are 0-based (data-model indices, data-for's i); -1 for none
struct InspectWidget {
    std::string kind;                  // "list", "grid", "tree", "table"
    int count = 0;
    int first = -1, last = -1;         // the visible items
    int pool = 0;                      // row elements
    int poolBound = 0;                 // the most the pool may hold (R2)
    int overscan = 0;
    int lanes = 1;
    std::string selection;             // "none", "single", "multiple"
    std::vector<int> selected;
    std::vector<std::string> selectedKeys;
    int active = -1;
    float scrollTop = 0, totalHeight = 0;
    std::string source;                // "model:<name>", "lua", ""
    bool layoutBoundary = false, rowAliases = false;
    std::string sortKey, sortDirection;
};

struct InspectElement {
    int index = -1;            // position in the returned list
    int parent = -1;           // index of the parent in the list, -1 when not listed
    int depth = 0;             // relative to the first listed ancestor
    std::string ref;           // unique selector: "#id" when the id is unique, else the nearest uniquely-id'd
                               // ancestor (or "body") + " > tag:nth-child(n)" steps; resolves to exactly this element
    std::string tag, id;       // DOM names (an HTML document's <html> and <body>, not RmlUi's)
    std::vector<std::string> classes;
    std::vector<std::string> pseudo;   // set pseudo-classes among hover, active, focus, checked, disabled
    std::string text;          // own text (direct text children), whitespace collapsed, at most 120 chars ("..." appended)
    InspectRect border;        // border box (through CSS transforms, as getBoundingClientRect)
    float opacity = 1;         // effective
    int childCount = 0;        // element children (SVG internals are not elements here: <svg> is a leaf)
    InspectFlags flags;
    std::vector<InspectEffect> effects;   // p5-tooling: the effects it draws with, in property order
    std::shared_ptr<InspectMedia> media;  // p6-video: a <video>'s state, null for other elements
    std::shared_ptr<InspectWidget> widget;   // S17: a virtual view's window, null for other elements
};

struct InspectBoxModel {
    InspectRect content, padding, border, margin;
    InspectEdges paddingEdges, borderEdges, marginEdges;
    float scrollWidth = 0, scrollHeight = 0, clientWidth = 0, clientHeight = 0, scrollLeft = 0, scrollTop = 0;
};

struct InspectStyle {
    std::string property;   // CSS name (compat::cssPropertyName)
    std::string value;      // computed value as CSS text ("16px", "rgba(255, 255, 255, 0.8)", "flex")
    std::string source;     // "ui/menu.css:42:3" (via the compat line maps), "ui/menu.html:12 (style attribute)",
                            // "inherited from <ref>", "user-agent", "default", "not supported" (a CSS property the
                            // engine has no value for)
    std::string selector;   // the rule's selector when known (".panel h1")
    // the location of `source` when it names a file (empty / 0 otherwise): what a tool converts to its own paths
    std::string file;
    uint32_t line = 0, column = 0;
};

struct InspectDetail {
    InspectElement element;
    InspectBoxModel box;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<InspectStyle> styles;
};

// The document's tooltip and the drag in progress (S17 R39); refs as InspectElement::ref
struct InspectTooltip {
    bool visible = false;
    std::string source;                // the element it is about
    std::string text;
    InspectRect rect;
    std::string side;
};

struct InspectDrag {
    bool active = false;
    std::string source, target;        // target: the drop target under the drag ("" none)
    std::vector<std::string> types;
    bool allowed = false;
    std::string effect;                // the dropEffect it would end with
    bool programmatic = false;         // UIDrag
};

// The last hot reload of the document (S17 R31)
struct InspectReload {
    std::string mode;                  // "patch", "rebuild"; "" before one
    int kept = 0, updated = 0, inserted = 0, removed = 0, replaced = 0, runtimeKept = 0, scriptsRerun = 0;
    std::string fallbackReason;
    double ms = 0;
    std::vector<std::string> causes;
};

struct DocumentInfo {
    Entity entity = NULL_ENTITY;
    std::string html;                 // asset-relative (the component's filename)
    bool loaded = false;
    bool renderToTexture = false;
    bool visible = true;
    int sortOrder = 0;
    float width = 0, height = 0;      // viewport (the document's context, CSS px)
    uint32_t loadCount = 0;
    std::vector<compat::Diagnostic> diagnostics;
    std::vector<std::string> dependencies;
    std::vector<std::string> dataModels;
    InspectTooltip tooltip;            // S17
    InspectDrag drag;
    InspectReload reload;
};

class JASPERENGINE_API HtmlUIInspector {
public:
    // GameObjects with a UIDocumentComponent, in entity order
    static std::vector<Entity> documents(Scene* scene);
    static bool documentInfo(Scene* scene, Entity entity, DocumentInfo& out, std::string& error);
    // Elements matching `selector` ("" = <body>) and up to `depth` levels of their descendants,
    // document order, at most `limit` (truncated = more existed). An invalid selector is an error
    // ("selector \"div[[\" is not valid: ...").
    static bool query(Scene* scene, Entity entity, const std::string& selector, int depth, size_t limit,
                      std::vector<InspectElement>& out, bool& truncated, std::string& error);
    // The element of this document that receives the pointer at (x, y) and its ancestors up to
    // <body>, topmost first; empty when the point hits no element (it falls through to the game:
    // a transparent <html>/<body>). Other documents of the context are not considered.
    static bool hitTest(Scene* scene, Entity entity, float x, float y, std::vector<InspectElement>& stack, std::string& error);
    // Box model, attributes and computed styles of the element `ref` (any selector: its first match).
    // properties empty = every property whose computed value differs from its initial value, plus
    // display, position, width, height, color, font-size, font-family. Unknown property names are an error.
    static bool detail(Scene* scene, Entity entity, const std::string& ref, const std::vector<std::string>& properties,
                       InspectDetail& out, std::string& error);
    // Computed values of `properties` for every element of `refs` (same order; each ref's first
    // match), source omitted
    static bool styles(Scene* scene, Entity entity, const std::vector<std::string>& refs, const std::vector<std::string>& properties,
                       std::vector<std::vector<InspectStyle>>& out, std::string& error);
    // Border box of the first visible (not hidden/transparent/zero-size) element matching selector
    static bool elementRect(Scene* scene, Entity entity, const std::string& selector, InspectRect& out, std::string& error);
    // Lays out the scene's context at width x height (media queries re-evaluated), runs fn, then restores
    // the dimensions and lays out again, before returning. Nothing renders in between.
    static bool withViewport(Scene* scene, float width, float height, const std::function<void()>& fn, std::string& error);
    // The same for the context the entity's document is in: a texture-mode document's own context
    // (its texture size), else the scene's
    static bool withViewport(Scene* scene, Entity entity, float width, float height, const std::function<void()>& fn, std::string& error);
    // Pushes the Lua table bound to data model `name` (UIDocumentSystem::pushDataModel)
    static bool pushDataModel(Scene* scene, Entity entity, const std::string& name, lua_State* L);
};

}

#endif //HTMLUIINSPECTOR_H
