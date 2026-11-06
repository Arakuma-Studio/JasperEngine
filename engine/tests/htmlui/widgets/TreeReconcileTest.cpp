// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The live patch's diff (S17 R27-R29, AC11), without RmlUi: reading compiled RML back, matching
// (OpenEngine's UIHotReloadTests cases: a tag change replaces, an unchanged tag keeps the element,
// ids match across a reorder) and the 3-way merge (only what the author changed is in the plan:
// what a script changed live is kept otherwise; a binding change replaces the subtree).

#include "htmlui/compat/TreeReconcile.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    RmlNode parse(const std::string& body){
        RmlNode root;
        std::string error;
        EXPECT_TRUE(parseRmlElement("<rml><head><title>x</title></head>" + body + "</rml>", "body", root, &error)) << error;
        return root;
    }

    ReconcilePlan reconcile(const std::string& before, const std::string& after){
        static RmlNode oldRoot, newRoot;   // the plan points into the new tree
        oldRoot = parse(before);
        newRoot = parse(after);
        return reconcileTrees(oldRoot, newRoot, {}, {});
    }

    const ReconcilePlan::Element& childElement(const ReconcilePlan::Element& parent, size_t index){
        static const ReconcilePlan::Element none;
        if (index >= parent.children.size() || parent.children[index].element.empty()){
            ADD_FAILURE() << "no element child " << index;
            return none;
        }
        return parent.children[index].element.front();
    }

}

TEST(TreeReconcile, ReadsCompiledRml){
    const RmlNode root = parse(R"(<body id="htmlui-root" data-htmlui-loc="0"
><htmlbody data-htmlui-loc="1"><p class="a b" data-htmlui-loc="2">x &amp; y &#65;&#x42;</p
><img src="a.png" data-htmlui-loc="3"/><svg data-htmlui-loc="4"><path d="M0 0"/></svg></htmlbody></body>)");
    EXPECT_EQ(root.tag, "body");
    EXPECT_EQ(root.loc, 0);
    ASSERT_EQ(root.children.size(), 1u);
    const RmlNode& body = root.children[0];
    EXPECT_EQ(body.tag, "htmlbody");
    ASSERT_EQ(body.children.size(), 3u);
    EXPECT_EQ(body.children[0].loc, 2);
    EXPECT_EQ(*body.children[0].attribute("class"), "a b");
    ASSERT_EQ(body.children[0].children.size(), 1u);
    EXPECT_EQ(body.children[0].children[0].text, "x & y AB");
    EXPECT_TRUE(body.children[1].children.empty());
    EXPECT_EQ(body.children[1].rml, R"(<img src="a.png" data-htmlui-loc="3"/>)");
    // an inline SVG is text for RmlUi
    ASSERT_EQ(body.children[2].children.size(), 1u);
    EXPECT_FALSE(body.children[2].children[0].isElement());
    EXPECT_EQ(body.children[2].children[0].text, R"(<path d="M0 0"/>)");

    std::vector<RmlNode> fragment;
    ASSERT_TRUE(parseRmlFragment(R"(<div data-htmlui-loc="9">a</div> <b>c</b>)", fragment));
    ASSERT_EQ(fragment.size(), 3u);
    EXPECT_EQ(fragment[0].loc, 9);
    EXPECT_FALSE(fragment[1].isElement());

    RmlNode broken;
    std::string error;
    EXPECT_FALSE(parseRmlElement("<body><div></body>", "body", broken, &error) && error.empty());
}

TEST(TreeReconcile, DeclarationsKeepParenthesesAndQuotes){
    const auto declarations = parseDeclarations(R"(color: red; background-image: url("a;b.png"); width:10px)");
    ASSERT_EQ(declarations.size(), 3u);
    EXPECT_EQ(declarations[1].first, "background-image");
    EXPECT_EQ(declarations[1].second, R"(url("a;b.png"))");
    EXPECT_EQ(declarations[2].second, "10px");
}

// OE: ReconcileReplacesWhenOnlyTheTagDiffers (a type change under the same id)
TEST(TreeReconcile, ReplacesWhenOnlyTheTagDiffers){
    const ReconcilePlan plan = reconcile(R"(<body data-htmlui-loc="0"><div id="a" data-htmlui-loc="1">x</div></body>)",
                                         R"(<body data-htmlui-loc="0"><span id="a" data-htmlui-loc="1">x</span></body>)");
    const ReconcilePlan::Element& child = childElement(plan.root, 0);
    EXPECT_TRUE(child.replace);
    EXPECT_EQ(child.reason, "its element type changed");
    EXPECT_EQ(plan.replaced, 1);
    ASSERT_NE(child.node, nullptr);
    EXPECT_EQ(child.node->tag, "span");
}

// OE: ReconcilePreservesAnElementWhoseTagIsUnchanged (its content is updated in place)
TEST(TreeReconcile, PreservesAnElementWhoseTagIsUnchanged){
    const ReconcilePlan plan = reconcile(R"(<body data-htmlui-loc="0"><p data-htmlui-loc="1">old</p></body>)",
                                         R"(<body data-htmlui-loc="0"><p data-htmlui-loc="1">new</p></body>)");
    const ReconcilePlan::Element& p = childElement(plan.root, 0);
    EXPECT_FALSE(p.replace);
    ASSERT_EQ(p.children.size(), 1u);
    EXPECT_EQ(p.children[0].kind, ReconcilePlan::Child::Kind::Text);
    EXPECT_TRUE(p.children[0].textChanged);
    EXPECT_EQ(p.children[0].text, "new");
    EXPECT_EQ(plan.updated, 1);
    EXPECT_EQ(plan.replaced, 0);
}

TEST(TreeReconcile, IdsMatchAcrossAReorder){
    const ReconcilePlan plan = reconcile(
        R"(<body data-htmlui-loc="0"><div id="x" data-htmlui-loc="1"/><div id="y" data-htmlui-loc="2"/></body>)",
        R"(<body data-htmlui-loc="0"><div id="y" data-htmlui-loc="1"/><div id="x" data-htmlui-loc="2"/></body>)");
    ASSERT_EQ(plan.root.children.size(), 2u);
    EXPECT_EQ(childElement(plan.root, 0).oldLoc, 2);
    EXPECT_EQ(childElement(plan.root, 1).oldLoc, 1);
    EXPECT_EQ(plan.locs.at(2), 1);
    EXPECT_EQ(plan.locs.at(1), 2);
    EXPECT_EQ(plan.inserted, 0);
    EXPECT_EQ(plan.removed, 0);
}

TEST(TreeReconcile, UnidentifiedChildrenMatchByTagAndClasses){
    // the author added a class to the second <li>: it is still the second
    const ReconcilePlan plan = reconcile(
        R"(<body data-htmlui-loc="0"><ul data-htmlui-loc="1"><li class="a" data-htmlui-loc="2"/><li class="b" data-htmlui-loc="3"/></ul></body>)",
        R"(<body data-htmlui-loc="0"><ul data-htmlui-loc="1"><li class="a" data-htmlui-loc="2"/><li class="b c" data-htmlui-loc="3"/></ul></body>)");
    const ReconcilePlan::Element& ul = childElement(plan.root, 0);
    ASSERT_EQ(ul.children.size(), 2u);
    EXPECT_EQ(childElement(ul, 0).oldLoc, 2);
    const ReconcilePlan::Element& second = childElement(ul, 1);
    EXPECT_EQ(second.oldLoc, 3);
    EXPECT_EQ(second.addClasses, std::vector<std::string>{"c"});
    EXPECT_TRUE(second.removeClasses.empty());
}

// 3-way: what the author did not change is not in the plan (a script's change of it stays live)
TEST(TreeReconcile, UnchangedAuthoredValuesLeaveTheLiveOnes){
    const std::string page = R"(<body data-htmlui-loc="0"><button id="b" class="big" title="t" style="color: red; width: 10px" data-htmlui-loc="1">Go</button></body>)";
    const ReconcilePlan plan = reconcile(page, page);
    const ReconcilePlan::Element& button = childElement(plan.root, 0);
    EXPECT_FALSE(button.changed());
    ASSERT_EQ(button.children.size(), 1u);
    EXPECT_FALSE(button.children[0].textChanged);
    EXPECT_EQ(plan.updated, 0);
    EXPECT_GE(plan.kept, 2);
}

// 3-way: what the author changed wins
TEST(TreeReconcile, ChangedAuthoredValuesWin){
    const ReconcilePlan plan = reconcile(
        R"(<body data-htmlui-loc="0"><button class="big" title="t" disabled="" style="color: red; width: 10px" data-htmlui-loc="1">Go</button></body>)",
        R"(<body data-htmlui-loc="0"><button class="small" title="u" style="color: blue; width: 10px; height: 4px" data-htmlui-loc="1">Stop</button></body>)");
    const ReconcilePlan::Element& button = childElement(plan.root, 0);
    EXPECT_FALSE(button.replace);
    EXPECT_EQ(button.addClasses, std::vector<std::string>{"small"});
    EXPECT_EQ(button.removeClasses, std::vector<std::string>{"big"});
    ASSERT_EQ(button.setAttributes.size(), 1u);
    EXPECT_EQ(button.setAttributes[0], (std::pair<std::string, std::string>{"title", "u"}));
    EXPECT_EQ(button.removeAttributes, std::vector<std::string>{"disabled"});
    // only the changed inline properties: width stays the live one (a script's, maybe)
    EXPECT_EQ(button.setStyles.size(), 2u);
    EXPECT_TRUE(std::find(button.setStyles.begin(), button.setStyles.end(), std::pair<std::string, std::string>{"color", "blue"}) != button.setStyles.end());
    EXPECT_TRUE(std::find(button.setStyles.begin(), button.setStyles.end(), std::pair<std::string, std::string>{"height", "4px"}) != button.setStyles.end());
    ASSERT_EQ(button.children.size(), 1u);
    EXPECT_TRUE(button.children[0].textChanged);
    EXPECT_EQ(button.children[0].text, "Stop");
}

// a new node goes after its new predecessor (the patch keeps nodes scripts added after the
// predecessor they followed); a removed one is listed by its loc
TEST(TreeReconcile, InsertsAfterTheirPredecessorAndRemovesByLoc){
    const ReconcilePlan plan = reconcile(
        R"(<body data-htmlui-loc="0"><div id="menu" data-htmlui-loc="1"><button id="play" data-htmlui-loc="2"/><button id="quit" data-htmlui-loc="3"/><hr data-htmlui-loc="4"/></div></body>)",
        R"(<body data-htmlui-loc="0"><div id="menu" data-htmlui-loc="1"><button id="play" data-htmlui-loc="2"/><button id="options" data-htmlui-loc="3"/><button id="quit" data-htmlui-loc="4"/></div></body>)");
    const ReconcilePlan::Element& menu = childElement(plan.root, 0);
    ASSERT_EQ(menu.children.size(), 3u);
    EXPECT_EQ(menu.children[0].kind, ReconcilePlan::Child::Kind::Element);
    EXPECT_EQ(menu.children[1].kind, ReconcilePlan::Child::Kind::Insert);
    EXPECT_EQ(*menu.children[1].node->attribute("id"), "options");
    EXPECT_EQ(childElement(menu, 2).oldLoc, 3);
    ASSERT_EQ(menu.removedChildren.size(), 1u);
    EXPECT_EQ(menu.removedChildren[0].loc, 4);
    EXPECT_EQ(plan.inserted, 1);
    EXPECT_EQ(plan.removed, 1);
    // renumbered: quit was 3, is 4
    EXPECT_EQ(plan.locs.at(3), 4);
}

// R29: a data binding that changed replaces the element and what is under it
TEST(TreeReconcile, BindingChangesReplaceTheSubtree){
    const ReconcilePlan forPlan = reconcile(
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><p data-for="x : items" data-htmlui-loc="2">{{x}}</p><b data-htmlui-loc="3">kept</b></div></body>)",
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><p data-for="x : others" data-htmlui-loc="2">{{x}}</p><b data-htmlui-loc="3">kept</b></div></body>)");
    const ReconcilePlan::Element& model = childElement(forPlan.root, 0);
    EXPECT_FALSE(model.replace) << "only the data-for element";
    EXPECT_TRUE(childElement(model, 0).replace);
    EXPECT_EQ(childElement(model, 0).reason, "data-for changed");
    EXPECT_FALSE(childElement(model, 1).replace);
    EXPECT_EQ(forPlan.replaced, 1);
    ASSERT_EQ(forPlan.causes.size(), 1u);

    const ReconcilePlan textPlan = reconcile(
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><span data-htmlui-loc="2">Gold: {{gold}}</span></div></body>)",
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><span data-htmlui-loc="2">Coins: {{gold}}</span></div></body>)");
    EXPECT_TRUE(childElement(childElement(textPlan.root, 0), 0).replace);
    EXPECT_EQ(childElement(childElement(textPlan.root, 0), 0).reason, "a {{ }} text changed");

    // a data-for template's markup is RmlUi's: a change replaces; a renumbering does not
    const ReconcilePlan templatePlan = reconcile(
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><p data-for="x : items" data-htmlui-loc="2"><i data-htmlui-loc="3">{{x}}</i></p></div></body>)",
        R"(<body data-htmlui-loc="0"><div data-model="m" data-htmlui-loc="1"><i data-htmlui-loc="2">new</i><p data-for="x : items" data-htmlui-loc="3"><i data-htmlui-loc="4">{{x}}</i></p></div></body>)");
    EXPECT_EQ(templatePlan.replaced, 0);
    EXPECT_EQ(templatePlan.locs.at(3), 4);
    EXPECT_EQ(templatePlan.locs.at(2), 3);
}

TEST(TreeReconcile, TemplatesComparedBeyondTheirNumbers){
    RmlNode oldRoot = parse(R"(<body data-htmlui-loc="0"><list-view data-htmlui-loc="1"><htmltemplate data-htmlui-template="0" data-htmlui-loc="2"/></list-view></body>)");
    RmlNode newRoot = parse(R"(<body data-htmlui-loc="0"><p data-htmlui-loc="1">new</p><list-view data-htmlui-loc="2"><htmltemplate data-htmlui-template="0" data-htmlui-loc="3"/></list-view></body>)");
    std::vector<std::vector<RmlNode>> oldTemplates(1), newTemplates(1);
    ASSERT_TRUE(parseRmlFragment(R"(<div class="row" data-htmlui-loc="4">{{item.name}}</div>)", oldTemplates[0]));
    ASSERT_TRUE(parseRmlFragment(R"(<div class="row" data-htmlui-loc="5">{{item.name}}</div>)", newTemplates[0]));
    ReconcilePlan plan = reconcileTrees(oldRoot, newRoot, oldTemplates, newTemplates);
    EXPECT_TRUE(plan.changedTemplates.empty()) << "renumbered only";
    EXPECT_EQ(plan.templates.at(0), 0);
    EXPECT_EQ(plan.locs.at(4), 5);
    EXPECT_EQ(plan.replaced, 0);

    ASSERT_TRUE(parseRmlFragment(R"(<div class="row big" data-htmlui-loc="5">{{item.name}}</div>)", newTemplates[0]));
    plan = reconcileTrees(oldRoot, newRoot, oldTemplates, newTemplates);
    EXPECT_TRUE(plan.changedTemplates.count(0));
}

TEST(TreeReconcile, MarkdownContentIsReplacedWhenTheAuthorChangedIt){
    const ReconcilePlan same = reconcile(
        R"(<body data-htmlui-loc="0"><markdown data-htmlui-markdown="" data-htmlui-loc="1"><div class="markdown-body"><p>a</p></div></markdown></body>)",
        R"(<body data-htmlui-loc="0"><markdown data-htmlui-markdown="" data-htmlui-loc="1"><div class="markdown-body"><p>a</p></div></markdown></body>)");
    EXPECT_FALSE(childElement(same.root, 0).content);
    const ReconcilePlan changed = reconcile(
        R"(<body data-htmlui-loc="0"><markdown data-htmlui-markdown="" data-htmlui-loc="1"><div class="markdown-body"><p>a</p></div></markdown></body>)",
        R"(<body data-htmlui-loc="0"><markdown data-htmlui-markdown="" data-htmlui-loc="1"><div class="markdown-body"><p>b</p></div></markdown></body>)");
    EXPECT_TRUE(childElement(changed.root, 0).content);
    EXPECT_EQ(childElement(changed.root, 0).contentRml, R"(<div class="markdown-body"><p>b</p></div>)");
}
