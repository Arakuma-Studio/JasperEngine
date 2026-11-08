// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R19-R23, R26, D4, §6.4)
// - Purpose: the Properties panel's rows and sections, table-driven. A row is a field path in
//   one or more targets (several layers, rims or parts); everything it shows (type, hard range,
//   default, enum values, unit) comes from S80's member schema or the effect / generator
//   registry's ParamDef (lookupField), and only the editor-only data (label overrides, UI range,
//   slider mapping, `when` visibility, section, summary) comes from the TitleRows table. No page
//   hand-writes a numeric range or a default.
// - One code path draws and indexes (D4): every page function takes a RowContext whose mode is
//   Draw or Collect. In Collect each row records itself in the PropertyIndex (label, aliases,
//   section chain, node, targets, field path, kind, enum labels) and returns without an ImGui
//   item; sections report open and `when` runs on the real document, so a row the page would
//   not show is never a search hit, and a hit draws the very same row (fieldRow) bound to the
//   same targets.
// - Edits (R22, D3): a row's gesture stages `set` ops on the state (Set gives every target the
//   value, Shift moves each from its own start value) and commits once on release with the step
//   "Change <Label>", "Change <Label> on 3 Layers"; Reset is "Reset <Label>"; editing an inherited
//   value is "Override <Label>" (the flag and the value in one batch, R23).
// - Thread-safety: main thread.

#pragma once

#include "TitleDesignerState.h"
#include "TitlePropertyIndex.h"

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "widget/DocRow.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    enum class RowMode : uint8_t { Draw, Collect };

    // S83 R41's cost badge of an effect, a generator layer or a layer: its class (C0-C4), the
    // tooltip with the desktop and mobile estimates, amber for a C4 that a Realtime title bakes
    // on mobile ("Baked on mobile")
    struct CostBadge {
        std::string cls;
        std::string tooltip;
        bool amber = false;
    };
    ImVec4 costBadgeColor(const CostBadge& badge);
    // the badge as small coloured text on the current line (SameLine first), its tooltip on hover
    void drawCostBadge(const CostBadge& badge);

    // what the window gives the page functions
    class PropertyHost {
    public:
        virtual ~PropertyHost() = default;
        virtual TitleDesignerState& designerState() = 0;
        // open/closed state of a section per node kind (editor settings, R19)
        virtual bool sectionOpen(const std::string& key, bool defaultOpen) = 0;
        virtual void setSectionOpen(const std::string& key, bool open) = 0;
        // the row a search landing flashes (R26), "" when none
        virtual std::string flashingRow() const { return {}; }
        // the row to scroll into view this frame (a search landing)
        virtual std::string scrollToRow() const { return {}; }
        virtual void rowScrolled() {}
        // UI the window owns
        virtual void openFontPicker(const std::vector<std::string>& /*layers*/) {}
        virtual void openAddPalette(const std::string& /*filter*/) {}
        virtual void exposeField(const FieldRef& /*target*/, const std::string& /*label*/) {}
        virtual void selectNode(const NodeRef& /*node*/) {}
        virtual void drawFontRow(const std::vector<std::string>& /*layers*/, bool /*readOnly*/) {}
        virtual const jasperengine::titleart::AssetResolver* assets() { return nullptr; }
        // the characters selected in the on-canvas text editor (R38): their layer and code point indexes
        virtual bool selectedCharacters(std::string& /*layer*/, std::vector<int>& /*indexes*/) { return false; }
        // a refused edit: the window shows the reason
        virtual void reportError(const std::string& /*message*/) {}
        // S83 R41: the cost of effect `index` of a layer's stack (part "" ) or a part's stack,
        // of a layer's effects together (index -1), of a generator layer (its own row)
        virtual bool effectCost(const std::string& /*layer*/, const std::string& /*part*/, int /*index*/, CostBadge& /*out*/) { return false; }
        // S86's picker tiles: layer `layerId` of `doc` rendered through the window's preview queue
        // at priority Thumbnails under its own slot (0 while it has no picture yet)
        virtual ImTextureID previewTile(const std::string& /*slot*/, const std::shared_ptr<const jasperengine::titleart::TitleArtDoc>& /*doc*/,
                                        const std::string& /*layerId*/, const jasperengine::titleart::TitleRect& /*bounds*/, uint64_t /*content*/,
                                        int /*pixels*/) {
            return 0;
        }
        // the project's files with one of these extensions (".hdr"), project-relative (S86's
        // environment picker lists the project's HDRIs)
        virtual std::vector<std::string> projectFiles(const std::vector<std::string>& /*extensions*/) { return {}; }
        // Preview As (S86's Realtime Preview row drives it)
        virtual std::string previewMode() const { return "realtime"; }
        virtual void choosePreviewMode(const std::string& /*mode*/) {}
        // S83 R39: an effect's or generator's L params play on the canvas (view state, never saved)
        virtual bool livePreviewOn(const std::string& /*layer*/, const std::string& /*effectId*/) const { return false; }
        virtual void setLivePreview(const std::string& /*layer*/, const std::string& /*effectId*/, bool /*on*/) {}
    };

    struct RowContext {
        RowMode mode = RowMode::Draw;
        PropertyHost* host = nullptr;
        TitleDesignerState* state = nullptr;
        PropertyIndex* sink = nullptr;              // Collect
        NodeRef node;                               // the page's node
        std::string nodeLabel;                      // the layer's name ("Document")
        std::string nodeKind;                       // the section-state key ("text", "rim", "face", "fx:outer_glow", "document")
        std::vector<std::string> sectionChain;      // open sections, outermost first (a hit's location)
        std::string stepPrefix;                     // "Rim 2 ", "Bevel ": the undo step's label prefix
        std::string countNoun;                      // "Layers", "Rims": the multi-target step suffix
        bool readOnly = false;
        std::string readOnlyReason;
        bool allOpen = false;                       // Collect: every section open
        std::vector<std::string> forceOpen;         // section titles a search landing opens (R26)
        bool legendShown = false;                   // the source-dot legend is drawn once per page (R23)
        // Collect, one slice of a page (R69's per-frame build): only its top-level section of this
        // index (0 also takes the rows outside sections); -1: the whole page
        int onlySection = -1;
        bool sliceTakesRest = false;                // the last slice: its section and every later one
        int topSections = 0;                        // the top-level sections met so far
        bool collecting() const { return mode == RowMode::Collect; }
    };

    // editor-only row data (TitleRows.cpp's table), by "<kind>.<member>" or "fx:<type>.<param>"
    struct RowSpec {
        const char* label = nullptr;                // overrides the schema / ParamDef label
        double uiMin = 0.0, uiMax = 0.0;            // equal: the hard range
        ui::SliderMap slider = ui::SliderMap::None; // None: Linear when the range is finite, Log when it spans 50x
        bool noSlider = false;
        bool hidden = false;                        // drawn elsewhere (header, Layers, a handle) or internal
        bool link = false;                          // works only on its page: a link in search
        const char* section = nullptr;              // the section it belongs to on its page
        // shown while this holds on the object holding the member (null: always)
        bool (*when)(const Json& object) = nullptr;
        const char* aliases = nullptr;              // extra search words, '|'-separated
    };
    const RowSpec* rowSpec(const std::string& key);
    // the spec key of a member of a kind ("bevel.size"), of an effect param ("fx:outer_glow.size")
    std::string rowSpecKey(const jasperengine::titleart::SchemaKind* kind, const std::string& member);

    // ---- inheritance (R23, TitleInheritance.cpp) ----

    // a field that can take its value from a source (S80's use_global_light members, the
    // withGlobalLight effect params; S86's lighting levels register theirs)
    struct InheritanceRule {
        // the member pattern ("bevel.light_angle") or an effect param ("fx:*.angle" matches every
        // definition whose GlobalLightBinding names it)
        std::string pattern;
        std::string flagMember;                     // the sibling flag (true = inherited): "use_global_light"
        std::string sourceName;                     // "Global Light"
        // the inherited value of the field for `object` (the object holding it) in `doc`
        std::function<Json(const jasperengine::titleart::TitleArtDoc& doc, const Json& object, const std::string& member)> value;
        // S86's lighting levels: the object is a sparse kind and a member it lacks is inherited (no
        // flag member: setting the value sets it here, "Inherit" removes it). `pattern` may end in
        // ".*" (every member of the kind)
        bool sparse = false;
        // the inherited value of a target (the level above it), used instead of `value` when set
        std::function<Json(const jasperengine::titleart::TitleArtDoc& doc, const FieldRef& target, const std::string& member)> valueAt;
    };
    void registerInheritance(InheritanceRule rule);
    // the rule of a field (`object` the object holding `member`, of schema kind `kind`, or an
    // effect's params with its definition); null when it does not inherit
    const InheritanceRule* inheritanceOf(const std::string& kindName, const std::string& member,
                                         const jasperengine::titleart::EffectDef* effect, const jasperengine::titleart::GeneratorDef* generator);

    // ---- sections and rows ----

    // a collapsing section whose closed header states its contents (R19); true when its rows
    // should be drawn (always in Collect). `stateKey` is per node kind: closing Rim > Bevel closes
    // it on every rim
    bool beginSection(RowContext& ctx, const std::string& id, const std::string& title, const std::string& summary, bool defaultOpen);
    void endSection(RowContext& ctx);

    struct RowExtras {
        const char* label = nullptr;                // overrides the table and the schema
        const char* stepLabel = nullptr;            // the undo step's label ("Rim Width")
        bool link = false;
    };

    // one field over `targets` (same member in each; "" layer = the document)
    void fieldRow(RowContext& ctx, const std::vector<FieldRef>& targets, const RowExtras& extras = {});
    // every member of the object at `basePath` (of schema kind `kindName`) in each target, in
    // schema order, skipping hidden ones and nested objects (their callers draw them as sections)
    void objectRows(RowContext& ctx, const std::vector<std::string>& layers, const std::string& basePath, const std::string& kindName,
                    const std::vector<std::string>& skip = {});
    // the params of an effect or generator definition at `basePath` ("effects[2].params")
    void paramRows(RowContext& ctx, const std::vector<std::string>& layers, const std::string& basePath,
                   const std::vector<jasperengine::titleart::ParamDef>& params, const jasperengine::titleart::EffectDef* effect,
                   const jasperengine::titleart::GeneratorDef* generator);

    // the value of a field in a target (null when it does not resolve)
    const Json* fieldValue(const jasperengine::titleart::TitleArtDoc& doc, const FieldRef& target);
    // the step name of an edit: "Change Rim Width", "Change Rim Width on 3 Rims", "Reset Bevel Depth"
    std::string stepName(const char* verb, const std::string& label, size_t targets, const std::string& countNoun);

    // ---- summaries (R19) ----

    // "Smooth 3 % · Ring" for a material sub-object, "Tracking 40 · Vertical" ...; `kindName`
    // the object's schema kind
    std::string objectSummary(const std::string& kindName, const Json& object);
    // a number with its unit, as rows show it ("3 %", "120°", "40 px")
    std::string formatValue(const Json& value, jasperengine::titleart::ParamUnit unit);
    // "solid" -> "Solid", "arc_lower" -> "Arc Lower"
    std::string enumLabel(const std::string& value);
    // the enum labels of a member or param (options' labels)
    std::vector<std::string> enumLabelsOf(const jasperengine::titleart::SchemaMember* member, const jasperengine::titleart::ParamDef* param);
    std::vector<std::string> enumValuesOf(const jasperengine::titleart::SchemaMember* member, const jasperengine::titleart::ParamDef* param);

    // TitleCurve <-> the engine Curve the embedded CurveEditor edits
    Json curveJsonFromKeys(const std::vector<std::pair<std::pair<float, float>, bool>>& points);

    // tests: a row whose default the schema lookup drops is reported (AC7's red control)
    struct RowAudit {
        std::string rowId, label, fieldPath;
        bool numeric = false, hasDefault = false;
        double defaultValue = 0.0;
        double hardMin = 0.0, hardMax = 0.0;
    };
    // where the last Collect pass spent its time (ms): the rows' schema lookups, their values and
    // index entries, and the rows all told (the rest is the pages' own work); R69's bench reads it
    struct CollectTimes {
        double lookup = 0.0, values = 0.0, index = 0.0, rows = 0.0;
        int rowCount = 0;
    };
    const CollectTimes& lastCollectTimes();
    // forgets the rows a Collect pass replays (tests: a full build to compare with)
    void clearRecordedRows();
    // the numeric rows a Collect pass saw, with their schema defaults
    const std::vector<RowAudit>& lastCollectAudit();
    void clearCollectAudit();

    // While alive (a collect pass: the document does not change), the rows find `doc`'s layers by
    // id in a map instead of walking the tree for each field. Main thread; nests.
    class CollectLayerLookup {
    public:
        explicit CollectLayerLookup(const jasperengine::titleart::TitleArtDoc& doc);
        ~CollectLayerLookup();
        CollectLayerLookup(const CollectLayerLookup&) = delete;
        CollectLayerLookup& operator=(const CollectLayerLookup&) = delete;

    private:
        void* previous = nullptr;
    };

}
