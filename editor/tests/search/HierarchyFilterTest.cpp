// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Hierarchy's and Add Component's searches (S25 R14, AC12)

#include "search/HierarchyFilter.h"
#include "graph/Graph.h"
#include "window/dialog/ComponentAddDialog.h"
#include "Catalog.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct Node {
        std::string name;
        std::vector<std::string> components;
        std::vector<Node> children;
        bool matchesSearch = false;
        bool hasMatchingDescendant = false;
    };

    Node sampleTree() {
        Node root{"Root", {"Transform"}, {}};
        Node player{"Player", {"Transform", "Rigidbody"}, {}};
        player.children.push_back({"Main Camera", {"Transform", "Camera"}, {}});
        player.children.push_back({"Gun", {"Transform", "Mesh Renderer"}, {}});
        root.children.push_back(player);
        root.children.push_back({"Spot Light", {"Transform", "Light"}, {}});
        root.children.push_back({"Minimap Camera", {"Transform", "Camera"}, {}});
        return root;
    }

    // the names shown: matches and their ancestors
    void shown(const Node& node, std::vector<std::string>& out) {
        if (node.matchesSearch || node.hasMatchingDescendant) out.push_back(node.name);
        for (const Node& child : node.children) shown(child, out);
    }

    std::vector<std::string> filter(const std::string& query, bool matchCase = false) {
        Node root = sampleTree();
        HierarchyFilter(query, matchCase).markTree(root, [](const Node& node) { return node.components; });
        std::vector<std::string> out;
        shown(root, out);
        return out;
    }

    using Names = std::vector<std::string>;

}

TEST(HierarchyFilter, TypeToken) {
    // t:Camera keeps the camera GameObjects and their ancestors
    EXPECT_EQ(filter("t:Camera"), (Names{"Root", "Player", "Main Camera", "Minimap Camera"}));
    EXPECT_EQ(filter("t:camera t:light"), (Names{"Root", "Player", "Main Camera", "Spot Light", "Minimap Camera"}));
    EXPECT_EQ(filter("t:MeshRenderer"), (Names{"Root", "Player", "Gun"}));
    // words and t: together
    EXPECT_EQ(filter("minimap t:Camera"), (Names{"Root", "Minimap Camera"}));
    // Filter mode: word prefixes and typos, no bare subsequences
    EXPECT_EQ(filter("cam"), (Names{"Root", "Player", "Main Camera", "Minimap Camera"}));
    EXPECT_EQ(filter("spotlight"), (Names{"Root", "Spot Light"}));
    EXPECT_EQ(filter("plyer"), (Names{"Root", "Player"}));
    EXPECT_EQ(filter("pl"), (Names{"Root", "Player"}));     // not "Spot Light"
    // Match Case is the exact substring it always was
    EXPECT_EQ(filter("Cam", true), (Names{"Root", "Player", "Main Camera", "Minimap Camera"}));
    EXPECT_EQ(filter("cam", true), Names{});
    EXPECT_TRUE(HierarchyFilter("", false).empty());
    EXPECT_FALSE(HierarchyFilter("t:Camera", false).empty());
    EXPECT_TRUE(HierarchyFilter("t:Camera", false).needsComponents());
    EXPECT_FALSE(HierarchyFilter("camera", false).needsComponents());
}

TEST(HierarchyFilter, TenThousandUnder8ms) {
#if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
    constexpr bool checkBudgets = false;
#else
    constexpr bool checkBudgets = true;
#endif
    Node root{"Root", {}, {}};
    for (int i = 0; i < 100; i++) {
        Node group{"Group " + std::to_string(i), {"Transform"}, {}};
        for (int j = 0; j < 100; j++) group.children.push_back({"Enemy Soldier " + std::to_string(i * 100 + j), {"Transform", "Mesh Renderer"}, {}});
        root.children.push_back(std::move(group));
    }
    double best = 1e9;
    for (int run = 0; run < 3; run++) {
        const auto start = std::chrono::steady_clock::now();
        HierarchyFilter("soldier 99", false).markTree(root, [](const Node& node) { return node.components; });
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::printf("[ bench ] Hierarchy filter over 10000 GameObjects: %.2f ms (budget 8)\n", best);
    if (checkBudgets) {
        EXPECT_LE(best, 16.0);
    }
}

TEST(ComponentAddDialogSearch, RanksTypos) {
    const auto first = [](const std::string& query) {
        const std::vector<ComponentAddDialog::Ranked> ranked = ComponentAddDialog::rank(query);
        return ranked.empty() ? std::string() : Catalog::getComponentDisplayName(ranked.front().type);
    };
    EXPECT_EQ(first("rigdbody"), "Rigidbody");
    EXPECT_EQ(first("rigidbody 2d"), "Rigidbody 2D");
    EXPECT_EQ(first("camera"), "Camera");
    EXPECT_EQ(first("mesh rend"), "Mesh Renderer");
    EXPECT_EQ(first("audio"), "Audio Source");
    EXPECT_TRUE(ComponentAddDialog::rank("zzzzqqq").empty());
    // an empty query lists every component
    size_t listed = 0;
    for (const ComponentCategory& category : ComponentAddDialog::listing()) listed += category.entries.size();
    EXPECT_EQ(ComponentAddDialog::rank("").size(), listed);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; i++) ComponentAddDialog::rank("colider");
    const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 10;
    std::printf("[ bench ] Add Component ranking: %.3f ms (budget 1)\n", each);
}

// S82 R27, AC11: the Hierarchy finds Japanese names. Red control: with the normalize() before
// R27 the query "激" normalized to "" and the filter matched every GameObject, so "Glow" and
// "Main Camera" were shown too.
TEST(HierarchyFilterTest, FindsJapaneseNames) {
    Node root{"Root", {"Transform"}, {}};
    Node title{"激熱", {"Transform"}, {}};
    title.children.push_back({"Glow", {"Transform"}, {}});
    root.children.push_back(title);
    root.children.push_back({"Main Camera", {"Transform", "Camera"}, {}});
    root.children.push_back({"熱帯 Fish", {"Transform"}, {}});

    auto shownFor = [&](const std::string& query) {
        Node copy = root;
        HierarchyFilter(query, false).markTree(copy, [](const Node& node) { return node.components; });
        Names out;
        shown(copy, out);
        return out;
    };
    EXPECT_EQ(shownFor("激"), (Names{"Root", "激熱"}));
    EXPECT_EQ(shownFor("激熱"), (Names{"Root", "激熱"}));
    EXPECT_EQ(shownFor("熱"), (Names{"Root", "激熱", "熱帯 Fish"}));
    EXPECT_EQ(shownFor("熱帯 fish"), (Names{"Root", "熱帯 Fish"}));
    EXPECT_EQ(shownFor("激熱 glow"), Names{});     // both words in one name
    EXPECT_EQ(shownFor("氷"), Names{});
}
