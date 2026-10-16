// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIINSPECTOR_H
#define HTMLUIINSPECTOR_H

#include "Export.h"
#include "ecs/Entity.h"
#include "htmlui/compat/Diagnostics.h"

#include <cstdint>
#include <functional>
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
