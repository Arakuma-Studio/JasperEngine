// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRuntimeChecks.h"

#include "titleart/TitleArtChecks.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    void glyphAtlasCheck(TitleCheckContext& context, std::vector<TitleCheckResult>& out){
        const titleart::TitleGlyphAtlasPlan plan = titleart::planTitleGlyphAtlas(context.doc);
        TitleCheckResult result;
        result.id = "glyph_atlas";
        result.kind = TitleCheckKind::Document;
        result.method = "the text layer's effects and style members by their glyph safety (safe, approximate, unsafe)";
        titleart::Json unsafe = titleart::Json::array();
        titleart::Json approximate = titleart::Json::array();
        titleart::Json ops = titleart::Json::array();
        for (const titleart::TitleGlyphAtlasItem& item : plan.unsafe){
            unsafe.push_back(titleart::Json{{"label", item.label}, {"path", item.path}});
            if (item.fix.is_object() && item.fix.contains("ops")) for (const titleart::Json& op : item.fix["ops"]) ops.push_back(op);
        }
        for (const titleart::TitleGlyphAtlasItem& item : plan.approximate) approximate.push_back(titleart::Json{{"label", item.label}, {"path", item.path}});
        result.measured = titleart::Json{{"text_layer", plan.textLayer}, {"unsafe", unsafe}, {"approximate", approximate}, {"fallback", plan.fallback}};
        result.threshold = titleart::Json{{"unsafe", 0}};
        if (!plan.fallback.empty()){
            result.status = TitleCheckStatus::Warn;
            result.message = "glyph_atlas_unsupported: " + plan.fallback + ": a Glyph Atlas title of this document renders as Realtime";
        }else if (!plan.unsafe.empty()){
            result.status = TitleCheckStatus::Warn;
            std::string list;
            for (const titleart::TitleGlyphAtlasItem& item : plan.unsafe) list += (list.empty() ? "" : ", ") + item.label;
            result.message = "glyph_atlas_effects: " + std::to_string(plan.unsafe.size()) + " effects don't apply per glyph: " + list +
                             " (skipped in Glyph Atlas; hull and box plates draw as contour)";
            if (!ops.empty()) result.fix = titleart::Json{{"ops", ops}};
        }else{
            result.status = TitleCheckStatus::Pass;
            result.message = plan.approximate.empty() ? std::string("every effect applies per glyph exactly")
                                                      : std::to_string(plan.approximate.size()) + " effects apply per glyph approximately (overlaps differ from the merged render)";
        }
        out.push_back(std::move(result));
    }

}

void editor::registerTitleArtRuntimeChecks(){
    static bool done = false;
    if (done) return;
    done = true;
    CheckDef def;
    def.id = "glyph_atlas";
    def.kind = TitleCheckKind::Document;
    def.owner = "S81";
    def.description = "What Glyph Atlas does with the document: effects it skips (with fixes) or approximates, and a fallback to Realtime.";
    def.run = &glyphAtlasCheck;
    TitleArtChecks::registerCheck(std::move(def));
}
