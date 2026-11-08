// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R17, D6; S83 §6.10, R41, R42)
// - Purpose: the entries of the Title Designer's one Add palette, built from the registry and
//   S83's editor data (FxPresets, FxRecipes' recipes and style settings): Layers (S80's types,
//   shapes and adjustments), Generators (S83 §6.10's generator categories, each generator then
//   its presets) and Effects (§6.10's categories in their order, with the recipes and style
//   settings among the effects, and "Generators that follow this layer": every preset of every
//   following generator, which adds a Generator layer beside the target). Each entry carries the
//   op batch Enter commits and a hover previews (the same ops: R44), its undo step name, its
//   cost class (S83 R41) and the one-line note the palette shows.
// - Generators are layer rows, never effect rows (D6): a generator entry's ops add a layer,
//   whichever section lists it. A style setting (Dry Brush, 3D Glint) switches its style member
//   on and names the Properties section to open; it never adds an effect.
// - Search (R26/R27's folding): names, categories, aliases, preset names and Japanese names.
// - Thread-safety: main thread (reads the registry and the embedded presets).

#pragma once

#include "TitleDesignerState.h"

#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    struct AddEntry {
        enum class Section : uint8_t { Layers, Generators, Effects };
        Section section = Section::Layers;
        std::string category;           // the rail ("Glow", "Generators that follow this layer")
        std::string id;                 // unique ("fx:outer_glow", "gen:fire:behind", "recipe:shine")
        std::string label;
        std::string description;        // the footer's line
        std::vector<std::string> terms; // aliases, preset names, the Japanese name
        Json ops = Json::array();       // what Enter adds (and a hover previews)
        std::string stepName;           // "Add Effect Outer Glow to Title", "Add Layer Sunburst"
        PreviewItem::Kind kind = PreviewItem::Kind::Layer;
        std::string cost;               // "C0".."C4" (S83 R41), "" for plain layers
        std::string note;               // "Adds a Generator layer · below", "Style setting", "Recipe"
        int depth = 0;                  // a generator's preset sits under it
        bool addsLayer = false;         // its ops add a layer (the new layer is selected)
        bool startsTextEdit = false;    // Add Text starts editing the new text (R36)
        // a style setting: the Properties section titles to open on the target layer
        std::vector<std::string> openSections;
    };
    const char* addSectionName(AddEntry::Section section);   // "Layers", "Generators", "Effects"

    // what the palette adds to
    struct AddTarget {
        std::vector<std::string> layers;        // the selected layers (ids), primary last
        std::string name;                       // the primary layer's name ("" without one)
        bool hasStyle = false;                  // the primary is a Text, Shape, Vector or Image layer
        bool is3D = false;                      // the primary is Dimension 3D
        std::vector<NodeRef> parts;             // the selected parts (empty: the whole layer)
    };

    // §6.10's category orders
    const std::vector<std::string>& addEffectCategories();      // Glow .. Distort
    const std::vector<std::string>& addGeneratorCategories();   // Particles .. Fill & Pattern
    extern const char* const FOLLOW_CATEGORY;                    // "Generators that follow this layer"

    // the entries for `filter` ("" everything, "layers", "generators", "effects"), in palette
    // order (section, then category, then §6.10's order within it)
    std::vector<AddEntry> buildAddEntries(const std::string& filter, const AddTarget& target);

    // the query (English or Japanese, folded) matches the entry
    bool addEntryMatches(const AddEntry& entry, const std::string& query);

}
