// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase 4 (p4-inspector, spec §3.3.1, §5.1): htmlui::HtmlUIInspector over live documents:
// document listing and info, the element tree (refs, links, truncation), boxes, problem flags,
// hit tests, computed styles with their sources, viewport overrides, selectors, data models;
// and the edge cases: empty documents, hidden elements, pseudo-elements, texture-mode
// documents, reloads between and during queries, unicode text, .rml documents, large trees.

#include "InspectorTestCommon.h"

#include "LuaBinding.h"
#include "object/GameObject.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIDataModel.h"
#include "object/ui/UIValue.h"
#include "component/UIDocumentComponent.h"

#include "lua.hpp"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    class InspectorTest : public InspectorTestBase {
    protected:
        // every element of the document (depth 64 below <body>)
        std::vector<InspectElement> all(Scene* scene, Entity entity, size_t limit = 100000){
            std::vector<InspectElement> out;
            bool truncated = false;
            std::string error;
            EXPECT_TRUE(HtmlUIInspector::query(scene, entity, "", 64, limit, out, truncated, error)) << error;
            return out;
        }

        InspectElement one(Scene* scene, Entity entity, const std::string& selector){
            std::vector<InspectElement> out;
            bool truncated = false;
            std::string error;
            EXPECT_TRUE(HtmlUIInspector::query(scene, entity, selector, 0, 1000, out, truncated, error)) << error;
            EXPECT_EQ(out.size(), 1u) << selector;
            return out.empty() ? InspectElement() : out.front();
        }

        const InspectStyle* style(const InspectDetail& detail, const std::string& property){
            for (const InspectStyle& entry : detail.styles){
                if (entry.property == property)
                    return &entry;
            }
            return nullptr;
        }
    };

    bool validUtf8(const std::string& text){
        size_t i = 0;
        while (i < text.size()){
            const unsigned char c = (unsigned char)text[i];
            const size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (length == 0 || i + length > text.size())
                return false;
            for (size_t j = 1; j < length; j++){
                if (((unsigned char)text[i + j] & 0xC0) != 0x80)
                    return false;
            }
            i += length;
        }
        return true;
    }

    size_t codePoints(const std::string& text){
        size_t count = 0;
        for (char c : text){
            if (((unsigned char)c & 0xC0) != 0x80)
                count++;
        }
        return count;
    }

}

// 1. documents() lists exactly the GameObjects with a UIDocumentComponent, in entity order
TEST_F(InspectorTest, DocumentsListsTheUIDocuments){
    page("a", "", "<p>a</p>");
    page("b", "", "<p>b</p>");
    SceneFixture fixture;
    GameObject& plain = fixture.add<GameObject>();
    UIDocument second = fixture.addDocument("ui/b.html");
    UIDocument first = fixture.addDocument("ui/a.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    std::vector<Entity> expected{first.getEntity(), second.getEntity()};
    std::sort(expected.begin(), expected.end());
    const std::vector<Entity> documents = HtmlUIInspector::documents(&fixture.getScene());
    EXPECT_EQ(documents, expected);
    EXPECT_EQ(std::count(documents.begin(), documents.end(), plain.getEntity()), 0);
    EXPECT_EQ(std::count(documents.begin(), documents.end(), fixture.getCamera().getEntity()), 0);
    EXPECT_TRUE(HtmlUIInspector::documents(nullptr).empty());

    DocumentInfo info;
    std::string error;
    EXPECT_FALSE(HtmlUIInspector::documentInfo(&fixture.getScene(), plain.getEntity(), info, error));
    EXPECT_NE(error.find("has no UI Document component"), std::string::npos) << error;
    std::vector<InspectElement> out;
    bool truncated = false;
    EXPECT_FALSE(HtmlUIInspector::query(&fixture.getScene(), plain.getEntity(), "", 1, 10, out, truncated, error));
    EXPECT_NE(error.find("has no UI Document component"), std::string::npos) << error;
}

// 2. documentInfo: path, viewport, load count, diagnostics with their location
TEST_F(InspectorTest, DocumentInfo){
    page("info", "* { margin: 0; }\n.panel {\n  mix-blend-mode: multiply;\n  width: 100px;\n}\n", "<div class=\"panel\">x</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/info.html", 3);
    ASSERT_TRUE(fixture.waitUntilLoaded());

    DocumentInfo info;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::documentInfo(&fixture.getScene(), document.getEntity(), info, error)) << error;
    EXPECT_EQ(info.entity, document.getEntity());
    EXPECT_EQ(info.html, "ui/info.html");
    EXPECT_TRUE(info.loaded);
    EXPECT_FALSE(info.renderToTexture);
    EXPECT_TRUE(info.visible);
    EXPECT_EQ(info.sortOrder, 3);
    EXPECT_EQ(info.width, 1280.0f);
    EXPECT_EQ(info.height, 720.0f);
    EXPECT_EQ(info.loadCount, 1u);
    EXPECT_NE(std::find(info.dependencies.begin(), info.dependencies.end(), "ui/info.html"), info.dependencies.end());
    EXPECT_NE(std::find(info.dependencies.begin(), info.dependencies.end(), "ui/info.css"), info.dependencies.end());
    EXPECT_TRUE(info.dataModels.empty());

    const hc::Diagnostic* blend = nullptr;
    for (const hc::Diagnostic& diagnostic : info.diagnostics){
        if (diagnostic.subject == "mix-blend-mode")
            blend = &diagnostic;
    }
    ASSERT_NE(blend, nullptr);
    EXPECT_EQ(blend->code, hc::codes::UnsupportedProperty);
    EXPECT_EQ(blend->code.id(), "HTMLUI-CSS-0002");
    EXPECT_EQ(blend->severity, hc::Severity::Warning);
    EXPECT_EQ(blend->file, "ui/info.css");
    EXPECT_EQ(blend->line, 3u);
    EXPECT_EQ(blend->column, 3u);

    // a document that is not loaded: info without layout, queries fail
    page("missing-css", "", "<p>x</p>");
    UIDocument broken = fixture.addDocument("ui/does-not-exist.html");
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(HtmlUIInspector::documentInfo(&fixture.getScene(), broken.getEntity(), info, error)) << error;
    EXPECT_FALSE(info.loaded);
    EXPECT_EQ(info.html, "ui/does-not-exist.html");
    EXPECT_FALSE(info.diagnostics.empty()) << "the reason it did not load";
    std::vector<InspectElement> out;
    bool truncated = false;
    EXPECT_FALSE(HtmlUIInspector::query(&fixture.getScene(), broken.getEntity(), "", 1, 10, out, truncated, error));
    EXPECT_NE(error.find("is not loaded"), std::string::npos) << error;
}

// 3. query(""): <body> first, consistent parent/depth links, limit and truncation
TEST_F(InspectorTest, QueryTreeLinksAndLimit){
    page("tree", "* { margin: 0; }",
         "<div class=\"menu\">\n"
         "  <h1 class=\"title\">Skyward</h1>\n"
         "  <nav class=\"buttons\"><button id=\"play\">Play</button><button id=\"quit\">Quit</button></nav>\n"
         "</div>\n"
         "<div id=\"side\"><p>One <b>two</b></p></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/tree.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    std::vector<InspectElement> out;
    bool truncated = true;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "", 16, 1000, out, truncated, error)) << error;
    EXPECT_FALSE(truncated);
    ASSERT_EQ(out.size(), 9u);
    EXPECT_EQ(out[0].tag, "body");
    EXPECT_EQ(out[0].ref, "body");
    EXPECT_EQ(out[0].parent, -1);
    EXPECT_EQ(out[0].depth, 0);
    EXPECT_EQ(out[0].childCount, 2);
    EXPECT_RECT(out[0].border, 0, 0, 1280, out[0].border.height);
    for (size_t i = 0; i < out.size(); i++){
        EXPECT_EQ(out[i].index, (int)i);
        if (i == 0)
            continue;
        ASSERT_GE(out[i].parent, 0) << out[i].ref;
        ASSERT_LT(out[i].parent, (int)i);
        EXPECT_EQ(out[out[i].parent].depth + 1, out[i].depth) << out[i].ref;
    }
    std::vector<std::string> tags;
    for (const InspectElement& element : out)
        tags.push_back(element.tag);
    EXPECT_EQ(tags, (std::vector<std::string>{"body", "div", "h1", "nav", "button", "button", "div", "p", "b"}));
    const InspectElement* play = findRef(out, "#play");
    ASSERT_NE(play, nullptr);
    EXPECT_EQ(play->tag, "button");
    EXPECT_EQ(play->id, "play");
    EXPECT_EQ(play->text, "Play");
    EXPECT_EQ(play->depth, 3);
    EXPECT_EQ(out[play->parent].tag, "nav");
    EXPECT_EQ(out[play->parent].classes, (std::vector<std::string>{"buttons"}));
    const InspectElement* p = findRef(out, "#side > p:nth-child(1)");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->text, "One") << "own text only: the <b> is a child";
    EXPECT_EQ(p->childCount, 1);

    // depth limits the descendants; a selector lists its matches with theirs
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "", 1, 1000, out, truncated, error)) << error;
    EXPECT_EQ(out.size(), 3u) << "body and its two children";
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "button", 0, 1000, out, truncated, error)) << error;
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].ref, "#play");
    EXPECT_EQ(out[1].ref, "#quit");
    EXPECT_EQ(out[0].parent, -1);
    EXPECT_EQ(out[0].depth, 0);
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), ".menu", 1, 1000, out, truncated, error)) << error;
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1].parent, 0);
    EXPECT_EQ(out[2].tag, "nav");

    // limit
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "", 16, 4, out, truncated, error)) << error;
    EXPECT_EQ(out.size(), 4u);
    EXPECT_TRUE(truncated);
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "", 16, 9, out, truncated, error)) << error;
    EXPECT_EQ(out.size(), 9u);
    EXPECT_FALSE(truncated) << "exactly the limit: nothing more existed";
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "", 16, 0, out, truncated, error)) << error;
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(truncated);

    // no match is not an error
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), ".nothing", 2, 10, out, truncated, error)) << error;
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(truncated);

    // the <html> root
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "html", 1, 10, out, truncated, error)) << error;
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].tag, "html");
    EXPECT_EQ(out[0].ref, "html");
    EXPECT_EQ(out[0].id, "");
    EXPECT_EQ(out[1].ref, "body");
    EXPECT_RECT(out[0].border, 0, 0, 1280, 720);
}

// 4. every ref resolves to exactly its element
TEST_F(InspectorTest, RefsAreUnique){
    page("refs", ".item { height: 10px; }",
         "<ul class=\"list\">\n"
         "  <li class=\"item\">a<ul><li class=\"item\">a1</li><li class=\"item\">a2</li></ul></li>\n"
         "  <li class=\"item\">b<ul><li class=\"item\">b1</li><li class=\"item\" id=\"dup\">b2</li></ul></li>\n"
         "</ul>\n"
         "<ul class=\"list\"><li class=\"item\" id=\"dup\">c</li><li class=\"item\" id=\"9lives\">d</li><li id=\"ok-id\"><span>e</span><span>f</span></li></ul>\n"
         "<section><div><div><div>deep</div></div></div><div>x</div></section>\n"
         "<p>text <span>s1</span> more <span>s2</span></p>\n"
         "<template id=\"tpl\"><b>t</b></template>\n"
         "<svg width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\"/></svg>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/refs.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    const std::vector<InspectElement> elements = all(scene, document.getEntity());
    ASSERT_GT(elements.size(), 25u);
    std::set<std::string> refs;
    for (const InspectElement& element : elements){
        EXPECT_TRUE(refs.insert(element.ref).second) << "duplicate ref " << element.ref;
        std::vector<InspectElement> found;
        bool truncated = false;
        std::string error;
        ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), element.ref, 0, 10, found, truncated, error)) << element.ref << ": " << error;
        ASSERT_EQ(found.size(), 1u) << element.ref;
        EXPECT_EQ(found[0].ref, element.ref);
        EXPECT_EQ(found[0].tag, element.tag);
        EXPECT_EQ(found[0].text, element.text);
        EXPECT_EQ(found[0].childCount, element.childCount);
        EXPECT_RECT(found[0].border, element.border.x, element.border.y, element.border.width, element.border.height);
    }
    EXPECT_NE(findRef(elements, "#ok-id"), nullptr);
    EXPECT_NE(findRef(elements, "#tpl"), nullptr);
    EXPECT_EQ(findRef(elements, "#dup"), nullptr) << "a duplicate id is not a ref";
    EXPECT_EQ(findRef(elements, "#9lives"), nullptr) << "an id that needs escaping is not a ref";
    for (const InspectElement& element : elements){
        if (element.tag == "svg"){
            EXPECT_EQ(element.childCount, 0) << "<svg> is a leaf";
        }
        if (element.tag == "template"){
            EXPECT_TRUE(element.flags.hidden);
        }
    }
}

// 5. boxes: the border box, and detail's content/padding/margin boxes and edges
TEST_F(InspectorTest, BoxModel){
    page("box", "* { margin: 0; padding: 0; }\n"
                "#box { position: absolute; left: 40px; top: 30px; width: 200px; height: 100px; padding: 10px;\n"
                "       border: 2px solid #fff; margin: 5px; box-sizing: content-box; }\n"
                "#moved { position: absolute; left: 400px; top: 100px; width: 100px; height: 50px; transform: translate(-50%, -50%); }\n",
         "<div id=\"box\">box</div><div id=\"moved\"></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/box.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    const InspectElement box = one(scene, document.getEntity(), "#box");
    EXPECT_RECT(box.border, 45, 35, 224, 124);

    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, document.getEntity(), "#box", {"width"}, detail, error)) << error;
    EXPECT_EQ(detail.element.ref, "#box");
    EXPECT_RECT(detail.box.border, 45, 35, 224, 124);
    EXPECT_RECT(detail.box.padding, 47, 37, 220, 120);
    EXPECT_RECT(detail.box.content, 57, 47, 200, 100);
    EXPECT_RECT(detail.box.margin, 40, 30, 234, 134);
    EXPECT_EQ(detail.box.paddingEdges.top, 10.0f);
    EXPECT_EQ(detail.box.paddingEdges.left, 10.0f);
    EXPECT_EQ(detail.box.borderEdges.right, 2.0f);
    EXPECT_EQ(detail.box.borderEdges.bottom, 2.0f);
    EXPECT_EQ(detail.box.marginEdges.top, 5.0f);
    EXPECT_EQ(detail.box.marginEdges.left, 5.0f);
    EXPECT_EQ(detail.box.clientWidth, 220.0f);
    EXPECT_EQ(detail.box.clientHeight, 120.0f);
    EXPECT_EQ(detail.box.scrollLeft, 0.0f);
    ASSERT_EQ(detail.styles.size(), 1u);
    EXPECT_EQ(detail.styles[0].property, "width");
    EXPECT_EQ(detail.styles[0].value, "200px");
    EXPECT_EQ(detail.attributes, (std::vector<std::pair<std::string, std::string>>{{"id", "box"}}));

    // a transform moves the box as getBoundingClientRect does
    const InspectElement moved = one(scene, document.getEntity(), "#moved");
    EXPECT_RECT(moved.border, 350, 75, 100, 50);
    InspectRect rect;
    ASSERT_TRUE(HtmlUIInspector::elementRect(scene, document.getEntity(), "#moved", rect, error)) << error;
    EXPECT_RECT(rect, 350, 75, 100, 50);
}

// 6. problem flags, one case each
TEST_F(InspectorTest, Flags){
    page("flags", "* { margin: 0; padding: 0; }\n"
                  "#narrow { width: 100px; white-space: nowrap; font-size: 20px; }\n"
                  "#clip { overflow: hidden; width: 100px; height: 50px; }\n"
                  "#inner { width: 200px; height: 20px; }\n"
                  "#gone { display: none; }\n"
                  "#faint { opacity: 0.02; width: 50px; height: 50px; }\n"
                  "#far { position: absolute; left: 5000px; top: 10px; width: 50px; height: 50px; }\n"
                  "#flat { width: 0; height: 30px; }\n"
                  "#tall { height: 20px; }\n"
                  "#tall div { height: 60px; }\n"
                  "#veiled { visibility: hidden; height: 10px; }\n"
                  "#ok { width: 100px; height: 20px; }\n",
         "<div id=\"narrow\">A label that is much wider than its box</div>\n"
         "<div id=\"clip\"><div id=\"inner\"></div></div>\n"
         "<div id=\"gone\"><span id=\"child\">x</span></div>\n"
         "<div id=\"faint\"><div id=\"faint-child\">x</div></div>\n"
         "<div id=\"far\"></div>\n"
         "<div id=\"flat\"></div>\n"
         "<div id=\"tall\"><div></div></div>\n"
         "<div id=\"veiled\"><b id=\"veiled-child\">v</b></div>\n"
         "<div id=\"ok\">fine</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/flags.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    EXPECT_TRUE(one(scene, entity, "#narrow").flags.overflowX);
    EXPECT_FALSE(one(scene, entity, "#narrow").flags.overflowY);
    EXPECT_TRUE(one(scene, entity, "#inner").flags.clipped);
    EXPECT_FALSE(one(scene, entity, "#clip").flags.clipped);
    EXPECT_TRUE(one(scene, entity, "#clip").flags.overflowX);
    EXPECT_TRUE(one(scene, entity, "#child").flags.hidden) << "display: none on the parent";
    EXPECT_TRUE(one(scene, entity, "#gone").flags.hidden);
    EXPECT_TRUE(one(scene, entity, "#faint").flags.transparent);
    EXPECT_TRUE(one(scene, entity, "#faint-child").flags.transparent) << "effective opacity";
    EXPECT_NEAR(one(scene, entity, "#faint-child").opacity, 0.02f, 1e-4f);
    EXPECT_TRUE(one(scene, entity, "#far").flags.offscreen);
    EXPECT_TRUE(one(scene, entity, "#flat").flags.zeroSize);
    EXPECT_TRUE(one(scene, entity, "#tall").flags.overflowY);
    EXPECT_TRUE(one(scene, entity, "#veiled").flags.hidden);
    EXPECT_TRUE(one(scene, entity, "#veiled-child").flags.hidden) << "visibility is inherited";

    const InspectFlags ok = one(scene, entity, "#ok").flags;
    EXPECT_FALSE(ok.overflowX || ok.overflowY || ok.clipped || ok.hidden || ok.transparent || ok.offscreen || ok.zeroSize);
}

// 7. hit test: topmost first, pointer-events: none skipped, empty outside every element
TEST_F(InspectorTest, HitTest){
    page("hit", "* { margin: 0; padding: 0; }\n"
                ".panel { position: absolute; left: 100px; top: 100px; width: 400px; height: 300px; background: #223; }\n"
                ".btn { position: absolute; left: 50px; top: 50px; width: 120px; height: 40px; background: #46f; }\n"
                ".overlay { position: absolute; left: 0; top: 0; width: 400px; height: 300px; pointer-events: none; }\n"
                ".hidden { position: absolute; left: 300px; top: 200px; width: 50px; height: 50px; visibility: hidden; }\n",
         "<div class=\"panel\"><button class=\"btn\" id=\"buy\">Buy</button><div class=\"overlay\"></div><div class=\"hidden\"></div></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/hit.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    std::vector<InspectElement> stack;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), 170, 170, stack, error)) << error;
    ASSERT_EQ(stack.size(), 3u);
    EXPECT_EQ(stack[0].ref, "#buy");
    EXPECT_EQ(stack[1].classes, (std::vector<std::string>{"panel"}));
    EXPECT_EQ(stack[2].ref, "body");
    EXPECT_EQ(stack[0].parent, 1);
    EXPECT_EQ(stack[2].parent, -1);
    EXPECT_EQ(stack[2].depth, 0);
    EXPECT_EQ(stack[0].depth, 2);

    // under the transparent overlay, the panel
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), 450, 350, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].classes, (std::vector<std::string>{"panel"}));
    // a hidden element is not hit
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), 420, 320, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].classes, (std::vector<std::string>{"panel"}));
    // outside: the transparent <body> lets the pointer through
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), 900, 600, stack, error)) << error;
    EXPECT_TRUE(stack.empty());
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), -10, 5000, stack, error)) << error;
    EXPECT_TRUE(stack.empty());
}

// a page background (body's, on the root) takes the pointer as the engine's input routing
// does: hitTest is empty exactly where UIDocumentSystem lets the pointer through to the game
TEST_F(InspectorTest, HitTestFollowsTheEngineRouting){
    page("opaque", "html, body { margin: 0; }\nbody { background: #000; }\n#box { width: 100px; height: 100px; background: #f00; }", "<div id=\"box\"></div>");
    page("clear", "html, body { margin: 0; }\n#box { width: 100px; height: 100px; background: #f00; }", "<div id=\"box\"></div>");
    for (const char* file : {"ui/opaque.html", "ui/clear.html"}){
        SceneFixture fixture;
        UIDocument document = fixture.addDocument(file);
        ASSERT_TRUE(fixture.waitUntilLoaded());
        UIDocumentSystem& system = *fixture.getScene().getSystem<UIDocumentSystem>();
        for (const auto& point : {std::make_pair(50.0f, 50.0f), std::make_pair(900.0f, 600.0f), std::make_pair(150.0f, 20.0f)}){
            std::vector<InspectElement> stack;
            std::string error;
            ASSERT_TRUE(HtmlUIInspector::hitTest(&fixture.getScene(), document.getEntity(), point.first, point.second, stack, error)) << error;
            const bool consumed = system.eventOnPointerMove(point.first, point.second, 0);
            EXPECT_EQ(!stack.empty(), consumed) << file << " at " << point.first << ", " << point.second;
            if (point.first == 50.0f){
                ASSERT_FALSE(stack.empty());
                EXPECT_EQ(stack[0].ref, "#box");
            }
        }
        system.eventOnMouseLeave();
    }
}

// 8. computed styles and the rule, style attribute, ancestor or default that set them
TEST_F(InspectorTest, DetailStyles){
    write("ui/menu.css",
          "* { margin: 0; }\n"                              // 1
          ".panel {\n"                                     // 2
          "  color: #ffffff;\n"                            // 3
          "  font-size: 20px;\n"                           // 4
          "  padding: 4px 6px;\n"                          // 5
          "  --accent: #ff8800;\n"                         // 6
          "}\n"                                            // 7
          "\n"                                             // 8
          ".btn {\n"                                       // 9
          "  width: 10em;\n"                               // 10
          "  font-size: 20px;\n"                           // 11
          "}\n"                                            // 12
          ".btn.primary, .other {\n"                       // 13
          "  background-color: #4f8cff;\n"                 // 14
          "  opacity: 0.5;\n"                              // 15
          "}\n");                                          // 16
    write("ui/menu.html",
          "<!DOCTYPE html>\n"                              // 1
          "<html>\n"                                       // 2
          "<head><link rel=\"stylesheet\" href=\"menu.css\"></head>\n"   // 3
          "<body>\n"                                       // 4
          "<div class=\"panel\" id=\"panel\">\n"           // 5
          "  <button class=\"btn primary\" id=\"play\">Play</button>\n"                 // 6
          "  <span id=\"note\"\n"                          // 7
          "        style=\"border-top: 3px solid red\">n</span>\n"                     // 8
          "</div>\n"                                       // 9
          "</body>\n"
          "</html>\n");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/menu.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#play", {"background-color", "width", "opacity", "font-size", "color"}, detail, error)) << error;
    ASSERT_EQ(detail.styles.size(), 5u);
    const InspectStyle& background = detail.styles[0];
    EXPECT_EQ(background.property, "background-color");
    EXPECT_EQ(background.value, "rgb(79, 140, 255)");
    EXPECT_EQ(background.source, "ui/menu.css:14:3");
    EXPECT_EQ(background.file, "ui/menu.css");
    EXPECT_EQ(background.line, 14u);
    EXPECT_EQ(background.column, 3u);
    EXPECT_EQ(background.selector, ".btn.primary") << "the selector of the list that matched";
    EXPECT_EQ(detail.styles[1].value, "200px") << "10em at 20px";
    EXPECT_EQ(detail.styles[1].source, "ui/menu.css:10:3");
    EXPECT_EQ(detail.styles[1].selector, ".btn");
    EXPECT_EQ(detail.styles[2].value, "0.5");
    EXPECT_EQ(detail.styles[2].source, "ui/menu.css:15:3");
    EXPECT_EQ(detail.styles[3].value, "20px");
    EXPECT_EQ(detail.styles[3].source, "ui/menu.css:11:3");
    EXPECT_EQ(detail.styles[4].source, "user-agent") << "a button's colour";
    EXPECT_EQ(detail.styles[4].file, "");

    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#note", {"color", "font-size", "max-width", "--accent"}, detail, error)) << error;
    ASSERT_EQ(detail.styles.size(), 4u);
    EXPECT_EQ(detail.styles[0].value, "rgb(255, 255, 255)");
    EXPECT_EQ(detail.styles[0].source, "inherited from #panel");
    EXPECT_EQ(detail.styles[0].file, "");
    EXPECT_EQ(detail.styles[1].value, "20px");
    EXPECT_EQ(detail.styles[1].source, "inherited from #panel");
    EXPECT_EQ(detail.styles[2].property, "max-width");
    EXPECT_EQ(detail.styles[2].source, "default");
    EXPECT_EQ(detail.styles[3].property, "--accent");
    EXPECT_EQ(detail.styles[3].value.rfind("#ff8800", 0), 0u) << detail.styles[3].value;
    EXPECT_EQ(detail.styles[3].source, "inherited from #panel");

    // a shorthand: its longhands, from the declaration that set them
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#panel", {"padding", "padding-left", "--accent"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].value, "4px 6px 4px 6px");
    EXPECT_EQ(detail.styles[0].source, "ui/menu.css:5:3");
    EXPECT_EQ(detail.styles[1].value, "6px");
    EXPECT_EQ(detail.styles[2].source, "ui/menu.css:6:3");

    // the style attribute: the element's line in the HTML
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#note", {"border-top-width"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].value, "3px");
    EXPECT_EQ(detail.styles[0].source, "ui/menu.html:7 (style attribute)");
    EXPECT_EQ(detail.styles[0].line, 7u);

    // a user-agent property (button's display)
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#play", {"display"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].source, "user-agent");

    // every non-initial property, with the always-shown ones first
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#play", {}, detail, error)) << error;
    ASSERT_GE(detail.styles.size(), 9u);
    const char* const first[] = {"display", "position", "width", "height", "color", "font-size", "font-family"};
    for (size_t i = 0; i < 7; i++)
        EXPECT_EQ(detail.styles[i].property, first[i]);
    const InspectStyle* found = style(detail, "background-color");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->source, "ui/menu.css:14:3");
    std::set<std::string> names;
    for (const InspectStyle& entry : detail.styles)
        EXPECT_TRUE(names.insert(entry.property).second) << "listed twice: " << entry.property;

    // unknown property names are an error with a suggestion
    EXPECT_FALSE(HtmlUIInspector::detail(scene, entity, "#play", {"colr"}, detail, error));
    EXPECT_EQ(error, "unknown CSS property \"colr\" (did you mean color?)");
    EXPECT_FALSE(HtmlUIInspector::detail(scene, entity, "#nothing", {"color"}, detail, error));
    EXPECT_EQ(error, "selector \"#nothing\" matched nothing");
    // a CSS property the engine has no value for
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#play", {"mix-blend-mode"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].value, "");
    EXPECT_EQ(detail.styles[0].source, "not supported");

    // styles(): several elements, values only
    std::vector<std::vector<InspectStyle>> values;
    ASSERT_TRUE(HtmlUIInspector::styles(scene, entity, {"#play", "#panel", "#note"}, {"color", "background-color"}, values, error)) << error;
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0][1].value, "rgb(79, 140, 255)");
    EXPECT_EQ(values[0][1].source, "");
    EXPECT_EQ(values[1][1].value, "rgba(0, 0, 0, 0)");
    EXPECT_EQ(values[2][0].value, "rgb(255, 255, 255)");
    EXPECT_FALSE(HtmlUIInspector::styles(scene, entity, {"#play"}, {"nope-nope"}, values, error));
    EXPECT_NE(error.find("unknown CSS property \"nope-nope\""), std::string::npos) << error;
}

// a stylesheet change restyles in place: the next detail names the new line
TEST_F(InspectorTest, StylesFollowAStylesheetReload){
    page("live", "#a {\n  width: 50px;\n}\n", "<div id=\"a\">a</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/live.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    UIDocumentSystem& system = *scene->getSystem<UIDocumentSystem>();

    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, document.getEntity(), "#a", {"width"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].source, "ui/live.css:2:3");
    write("ui/live.css", "\n\n#a {\n  height: 4px;\n  width: 70px;\n}\n");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/live.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    ASSERT_TRUE(HtmlUIInspector::detail(scene, document.getEntity(), "#a", {"width"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].value, "70px");
    EXPECT_EQ(detail.styles[0].source, "ui/live.css:5:3");
    EXPECT_EQ(detail.element.border.width, 70.0f);
}

// 9. withViewport: media queries at the other size inside fn, everything restored after
TEST_F(InspectorTest, WithViewport){
    page("vp", "* { margin: 0; }\n"
               ".row { display: flex; flex-direction: row; }\n"
               ".row div { width: 200px; height: 50px; }\n"
               "@media (max-width: 800px) {\n  .row { flex-direction: column; }\n}\n",
         "<div class=\"row\"><div id=\"one\">1</div><div id=\"two\">2</div></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/vp.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    EXPECT_RECT(one(scene, entity, "#two").border, 200, 0, 200, 50);
    bool ran = false;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, 720, 1280, [&](){
        ran = true;
        EXPECT_RECT(one(scene, entity, "#two").border, 0, 50, 200, 50);
        DocumentInfo info;
        std::string infoError;
        ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, entity, info, infoError)) << infoError;
        EXPECT_EQ(info.width, 720.0f);
        EXPECT_EQ(info.height, 1280.0f);
        InspectDetail detail;
        ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, ".row", {"flex-direction"}, detail, infoError)) << infoError;
        EXPECT_EQ(detail.styles[0].value, "column");
        EXPECT_EQ(detail.styles[0].source, "ui/vp.css:5:10");
    }, error)) << error;
    EXPECT_TRUE(ran);
    DocumentInfo info;
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, entity, info, error)) << error;
    EXPECT_EQ(info.width, 1280.0f);
    EXPECT_EQ(info.height, 720.0f);
    EXPECT_RECT(one(scene, entity, "#two").border, 200, 0, 200, 50);

    // restored when fn throws
    EXPECT_THROW(HtmlUIInspector::withViewport(scene, 600, 400, [](){ throw std::runtime_error("boom"); }, error), std::runtime_error);
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, entity, info, error)) << error;
    EXPECT_EQ(info.width, 1280.0f);
    EXPECT_RECT(one(scene, entity, "#two").border, 200, 0, 200, 50);

    // invalid sizes; a scene without documents
    EXPECT_FALSE(HtmlUIInspector::withViewport(scene, 0, 100, [](){}, error));
    EXPECT_NE(error.find("is not valid"), std::string::npos) << error;
    EXPECT_FALSE(HtmlUIInspector::withViewport(scene, NAN, 100, [](){}, error));
    Scene empty;
    EXPECT_FALSE(HtmlUIInspector::withViewport(&empty, 100, 100, [](){}, error));
    EXPECT_NE(error.find("no HTML UI context"), std::string::npos) << error;

    // a reload inside fn: the restore still holds
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, 640, 480, [&](){
        scene->getSystem<UIDocumentSystem>()->reloadDocument(entity, true);
    }, error)) << error;
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, entity, info, error)) << error;
    EXPECT_EQ(info.width, 1280.0f);
    EXPECT_EQ(info.loadCount, 2u);
    EXPECT_RECT(one(scene, entity, "#two").border, 200, 0, 200, 50);
}

// withViewport: CSS transforms follow the other size (a panel centered with translateX(-50%)), and the restored
// layout's transforms are back before anything renders (fork P4-6, p4-mcp-tools: assert_ui viewports)
TEST_F(InspectorTest, WithViewportTransforms){
    page("vpt", "* { margin: 0; }\n"
                ".panel { position: absolute; left: 50%; top: 0; width: 50%; height: 100px; transform: translateX(-50%); }\n",
         "<div id=\"panel\" class=\"panel\"></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/vpt.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    EXPECT_RECT(one(scene, entity, "#panel").border, 320, 0, 640, 100);
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, entity, 720, 1280, [&](){
        EXPECT_RECT(one(scene, entity, "#panel").border, 180, 0, 360, 100);
    }, error)) << error;
    EXPECT_RECT(one(scene, entity, "#panel").border, 320, 0, 640, 100);
}

// 10. invalid selectors are errors; elementRect skips hidden matches
TEST_F(InspectorTest, SelectorsAndElementRect){
    page("sel", "* { margin: 0; }\n.target { width: 30px; height: 20px; }\n.first { display: none; }\n.faint { opacity: 0; }\n",
         "<div class=\"target first\"></div><div class=\"target faint\"></div><div class=\"target\" id=\"third\"></div><div id=\"none\" class=\"first\"></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/sel.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();
    TestSystem::get().clearLogs();

    std::vector<InspectElement> out;
    bool truncated = false;
    std::string error;
    for (const char* bad : {"div[[", ">>", "#", "div >", ":::"}){
        EXPECT_FALSE(HtmlUIInspector::query(scene, entity, bad, 0, 10, out, truncated, error)) << bad;
        EXPECT_EQ(error.rfind(std::string("selector \"") + bad + "\" is not valid: ", 0), 0u) << error;
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "Query selector"), 0u) << "no RmlUi warning logged";

    InspectRect rect;
    ASSERT_TRUE(HtmlUIInspector::elementRect(scene, entity, ".target", rect, error)) << error;
    const InspectElement third = one(scene, entity, "#third");
    EXPECT_RECT(rect, third.border.x, third.border.y, 30, 20);
    EXPECT_FALSE(HtmlUIInspector::elementRect(scene, entity, "#none", rect, error));
    EXPECT_NE(error.find("none visible"), std::string::npos) << error;
    EXPECT_FALSE(HtmlUIInspector::elementRect(scene, entity, ".absent", rect, error));
    EXPECT_EQ(error, "selector \".absent\" matched nothing");
    EXPECT_FALSE(HtmlUIInspector::elementRect(scene, entity, "div[[", rect, error));

    // CSS selector features through the compat rewrite
    ASSERT_TRUE(HtmlUIInspector::query(scene, entity, "body > div:nth-child(3)", 0, 10, out, truncated, error)) << error;
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].ref, "#third");
    ASSERT_TRUE(HtmlUIInspector::query(scene, entity, ".target:not(.first), #none", 0, 10, out, truncated, error)) << error;
    EXPECT_EQ(out.size(), 3u);
}

// pseudo-classes in the element entries
TEST_F(InspectorTest, PseudoClasses){
    page("pseudo", "* { margin: 0; }\nbutton { width: 100px; height: 40px; }\n",
         "<button id=\"off\" disabled>Off</button><input id=\"box\" type=\"checkbox\" checked><button id=\"on\">On</button>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/pseudo.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    EXPECT_EQ(one(scene, entity, "#off").pseudo, (std::vector<std::string>{"disabled"}));
    EXPECT_EQ(one(scene, entity, "#box").pseudo, (std::vector<std::string>{"checked"}));
    EXPECT_TRUE(one(scene, entity, "#on").pseudo.empty());
    // hover: a pointer over the button
    const InspectElement on = one(scene, entity, "#on");
    UIDocumentSystem& system = *scene->getSystem<UIDocumentSystem>();
    system.eventOnPointerMove(on.border.x + 5, on.border.y + 5, 0);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(one(scene, entity, "#on").pseudo, (std::vector<std::string>{"hover"}));
    system.eventOnMouseLeave();
}

// an empty document: <body> only, the pointer falls through everywhere
TEST_F(InspectorTest, EmptyDocument){
    write("ui/empty.html", "<!DOCTYPE html><html><head></head><body></body></html>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/empty.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    const std::vector<InspectElement> elements = all(scene, document.getEntity());
    ASSERT_EQ(elements.size(), 1u);
    EXPECT_EQ(elements[0].ref, "body");
    EXPECT_EQ(elements[0].childCount, 0);
    EXPECT_EQ(elements[0].text, "");
    std::vector<InspectElement> stack;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), 10, 10, stack, error)) << error;
    EXPECT_TRUE(stack.empty());
    InspectDetail detail;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, document.getEntity(), "body", {}, detail, error)) << error;
    EXPECT_FALSE(detail.styles.empty());

    // a whitespace-only file loads as an empty document too
    write("ui/blank.html", "   \n");
    UIDocument blank = fixture.addDocument("ui/blank.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    const std::vector<InspectElement> blankElements = all(scene, blank.getEntity());
    ASSERT_EQ(blankElements.size(), 1u);
    EXPECT_EQ(blankElements[0].tag, "body");
}

// ::before / ::after are not elements: not listed, their text not own text, hits go to the element
TEST_F(InspectorTest, PseudoElements){
    page("pe", "* { margin: 0; }\n"
               ".tag { position: absolute; left: 10px; top: 10px; font-size: 20px; background: #333; }\n"
               ".tag::before { content: \"NEW \"; }\n"
               ".tag::after { content: \"!\"; }\n",
         "<div class=\"tag\" id=\"tag\">Item</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/pe.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    const std::vector<InspectElement> elements = all(scene, document.getEntity());
    ASSERT_EQ(elements.size(), 2u);
    const InspectElement& tag = elements[1];
    EXPECT_EQ(tag.ref, "#tag");
    EXPECT_EQ(tag.text, "Item");
    EXPECT_EQ(tag.childCount, 0);
    // the start of the box is the ::before text
    std::vector<InspectElement> stack;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, document.getEntity(), tag.border.x + 3, tag.border.y + tag.border.height / 2, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#tag");
    // a selector naming a pseudo-element is refused: it is not an element
    std::vector<InspectElement> out;
    bool truncated = false;
    for (const char* selector : {".tag::before", ".tag:after", "div::marker", ".tag::before, .tag"}){
        EXPECT_FALSE(HtmlUIInspector::query(scene, document.getEntity(), selector, 0, 10, out, truncated, error)) << selector;
        EXPECT_NE(error.find("pseudo-element"), std::string::npos) << error;
    }
    ASSERT_TRUE(HtmlUIInspector::query(scene, document.getEntity(), "[title='a::b'], .tag:hover, .tag", 0, 10, out, truncated, error)) << error;
    EXPECT_EQ(out.size(), 1u);
}

// a texture-mode document: its own viewport, boxes in texture px, the entity overload of withViewport
TEST_F(InspectorTest, TextureModeDocument){
    page("tex", "* { margin: 0; }\n#a { width: 50%; height: 40px; background: #f00; }\n@media (max-width: 300px) { #a { height: 10px; } }\n",
         "<div id=\"a\">texture</div>");
    page("screen", "* { margin: 0; }\n#s { width: 50%; height: 40px; }\n", "<div id=\"s\">screen</div>");
    SceneFixture fixture;
    UIDocument screen = fixture.addDocument("ui/screen.html");
    UIDocument texture = fixture.addDocument("ui/tex.html");
    UIDocumentComponent& component = texture.getComponent<UIDocumentComponent>();
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.textureWidth = 512;
    component.textureHeight = 256;
    component.pixelRatio = 1.0f;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    DocumentInfo info;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, texture.getEntity(), info, error)) << error;
    EXPECT_TRUE(info.renderToTexture);
    EXPECT_TRUE(info.loaded);
    EXPECT_EQ(info.width, 512.0f);
    EXPECT_EQ(info.height, 256.0f);
    EXPECT_RECT(one(scene, texture.getEntity(), "#a").border, 0, 0, 256, 40);
    EXPECT_RECT(one(scene, screen.getEntity(), "#s").border, 0, 0, 640, 40);

    std::vector<InspectElement> stack;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, texture.getEntity(), 100, 20, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#a");

    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, texture.getEntity(), 200, 100, [&](){
        EXPECT_RECT(one(scene, texture.getEntity(), "#a").border, 0, 0, 100, 10);
        EXPECT_RECT(one(scene, screen.getEntity(), "#s").border, 0, 0, 640, 40);   // the screen context is not touched
    }, error)) << error;
    EXPECT_RECT(one(scene, texture.getEntity(), "#a").border, 0, 0, 256, 40);
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, screen.getEntity(), 400, 300, [&](){
        EXPECT_RECT(one(scene, screen.getEntity(), "#s").border, 0, 0, 200, 40);
        EXPECT_RECT(one(scene, texture.getEntity(), "#a").border, 0, 0, 256, 40);
    }, error)) << error;
    EXPECT_RECT(one(scene, screen.getEntity(), "#s").border, 0, 0, 640, 40);

    // fn turns the document into a screen one: its texture context goes, nothing is restored on it
    UIDocumentSystem& system = *scene->getSystem<UIDocumentSystem>();
    ASSERT_TRUE(HtmlUIInspector::withViewport(scene, texture.getEntity(), 300, 100, [&](){
        component.renderMode = UIDocumentRenderMode::SCREEN;
        system.reloadDocument(texture.getEntity(), false);
        EXPECT_EQ(system.getTextureContext(texture.getEntity()), nullptr);
    }, error)) << error;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, texture.getEntity(), info, error)) << error;
    EXPECT_FALSE(info.renderToTexture);
    EXPECT_EQ(info.width, 1280.0f);
    EXPECT_RECT(one(scene, texture.getEntity(), "#a").border, 0, 0, 640, 40);
}

// a full reload between queries: refs taken before resolve in the new document
TEST_F(InspectorTest, ReloadBetweenQueries){
    page("re", "* { margin: 0; }\n.item { height: 20px; }\n", "<div id=\"list\"><div class=\"item\">a</div><div class=\"item\">b</div></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/re.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    const std::vector<InspectElement> before = all(scene, entity);
    ASSERT_EQ(before.size(), 4u);
    page("re", "* { margin: 0; }\n.item { height: 30px; }\n",
         "<div id=\"list\"><div class=\"item\">a</div><div class=\"item\">b</div><div class=\"item\">c</div></div>");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = scene->getSystem<UIDocumentSystem>()->reloadChangedFiles({"ui/re.html", "ui/re.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    // queried right away, before any frame: the new document, laid out
    std::vector<std::vector<InspectStyle>> values;
    std::vector<std::string> refs;
    for (const InspectElement& element : before)
        refs.push_back(element.ref);
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::styles(scene, entity, refs, {"height"}, values, error)) << error;
    EXPECT_EQ(values[2][0].value, "30px");
    const std::vector<InspectElement> after = all(scene, entity);
    ASSERT_EQ(after.size(), 5u);
    EXPECT_EQ(after[4].text, "c");
    EXPECT_RECT(after[4].border, 0, 60, 1280, 30);
    DocumentInfo info;
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, entity, info, error)) << error;
    EXPECT_EQ(info.loadCount, 2u);

    // the file removed: the document fails to load and queries say so
    std::filesystem::remove(dir / "ui/re.html");
    scene->getSystem<UIDocumentSystem>()->reloadChangedFiles({"ui/re.html"});
    GpuEnvironment::drawFrames(2);
    std::vector<InspectElement> out;
    bool truncated = false;
    if (!HtmlUIInspector::query(scene, entity, "", 1, 10, out, truncated, error)){
        EXPECT_NE(error.find("is not loaded"), std::string::npos) << error;
    }
}

// unicode text: collapsed, cut at 120 code points with "...", valid UTF-8; unicode ids get a path ref
TEST_F(InspectorTest, UnicodeText){
    std::string longText;
    for (int i = 0; i < 40; i++)
        longText += "日本語 ";
    page("uni", "* { margin: 0; }",
         "<p id=\"short\">  Größe —\n  日本語 🎮 &amp; ok </p>\n"
         "<p id=\"long\">" + longText + "</p>\n"
         "<p id=\"naïve\">accent</p>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/uni.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    EXPECT_EQ(one(scene, document.getEntity(), "#short").text, "Größe — 日本語 🎮 & ok");
    const std::string cut = one(scene, document.getEntity(), "#long").text;
    EXPECT_TRUE(validUtf8(cut));
    EXPECT_EQ(codePoints(cut), 123u);
    EXPECT_EQ(cut.substr(cut.size() - 3), "...");
    const std::vector<InspectElement> elements = all(scene, document.getEntity());
    ASSERT_EQ(elements.size(), 4u);
    EXPECT_EQ(elements[3].id, "naïve");
    EXPECT_EQ(elements[3].ref, "body > p:nth-child(3)");
    EXPECT_EQ(one(scene, document.getEntity(), elements[3].ref).text, "accent");
}

// an .rml document: RCSS selectors as they are, the RML root is <body>
TEST_F(InspectorTest, RmlDocument){
    write("ui/plain.rml",
          "<rml>\n<head>\n<style>\nbody { width: 100%; height: 100%; }\n#box { width: 120px;\n height: 30px; }\n</style>\n</head>\n"
          "<body><div id=\"box\">rml</div><div class=\"x\"/><div class=\"x\"/></body>\n</rml>\n");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/plain.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    const std::vector<InspectElement> elements = all(scene, document.getEntity());
    ASSERT_EQ(elements.size(), 4u);
    EXPECT_EQ(elements[0].ref, "body");
    EXPECT_EQ(elements[1].ref, "#box");
    EXPECT_EQ(elements[3].ref, "body > div:nth-child(3)");
    EXPECT_EQ(one(scene, document.getEntity(), elements[3].ref).classes, (std::vector<std::string>{"x"}));
    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, document.getEntity(), "#box", {"width"}, detail, error)) << error;
    EXPECT_EQ(detail.styles[0].value, "120px");
    EXPECT_EQ(detail.styles[0].selector, "#box");
    EXPECT_EQ(detail.styles[0].file, "ui/plain.rml");
    EXPECT_GT(detail.styles[0].line, 0u);
}

// data models: names in documentInfo, the Lua proxy pushed
TEST_F(InspectorTest, DataModels){
    page("dm", "", "<div data-model=\"hud\"><span id=\"hp\">{{health}}</span></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/dm.html");
    UIValue initial = UIValue::object();
    initial["health"] = 75;
    UIDataModel model = document.createDataModel("hud", initial);
    ASSERT_TRUE(model.isValid());
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();

    DocumentInfo info;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::documentInfo(scene, document.getEntity(), info, error)) << error;
    EXPECT_EQ(info.dataModels, (std::vector<std::string>{"hud"}));
    EXPECT_EQ(one(scene, document.getEntity(), "#hp").text, "75");

    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    const int top = lua_gettop(L);
    ASSERT_TRUE(HtmlUIInspector::pushDataModel(scene, document.getEntity(), "hud", L));
    ASSERT_EQ(lua_gettop(L), top + 1);
    lua_getfield(L, -1, "health");
    EXPECT_EQ(lua_tointeger(L, -1), 75);
    lua_pop(L, 2);
    EXPECT_FALSE(HtmlUIInspector::pushDataModel(scene, document.getEntity(), "nope", L));
    EXPECT_EQ(lua_gettop(L), top);
    EXPECT_FALSE(HtmlUIInspector::pushDataModel(scene, document.getEntity(), "hud", nullptr));
    model = UIDataModel();
    lua_gc(L, LUA_GCCOLLECT, 0);
}

// a large tree: every element listed with a unique ref in one query, limit truncates
