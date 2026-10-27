// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Hierarchy's search (S25 R14): its words match a GameObject's name in Filter mode (every
// word a word prefix or a near miss, no bare subsequences: a tree shows every match in place),
// t:<Component> keeps GameObjects with that component (Unity's, by display name; several t: OR
// together). Match Case keeps the exact substring match the window always had. A match keeps
// its ancestors shown (markTree).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    class HierarchyFilter {
    public:
        HierarchyFilter(std::string_view query, bool matchCase);

        bool empty() const { return text.empty() && types.empty(); }
        // t: terms ask for the components of a GameObject
        bool needsComponents() const { return !types.empty(); }
        // components: the display names of the GameObject's components (called only for t:)
        bool matches(std::string_view name, const std::function<std::vector<std::string>()>& components) const;

        // Marks node.matchesSearch / node.hasMatchingDescendant down a tree whose nodes have
        // `name` and `children`; componentsOf(node) gives a node's component display names
        template <class Node, class ComponentsOf>
        void markTree(Node& node, ComponentsOf&& componentsOf) const {
            node.matchesSearch = matches(node.name, [&]() { return componentsOf(node); });
            node.hasMatchingDescendant = false;
            for (auto& child : node.children) {
                markTree(child, componentsOf);
                if (child.matchesSearch || child.hasMatchingDescendant) node.hasMatchingDescendant = true;
            }
        }

    private:
        bool matchCase = false;
        std::string text;            // the words (Match Case: as typed)
        std::string normalizedText;
        std::vector<std::string> types;   // normalized t: values
    };

}
