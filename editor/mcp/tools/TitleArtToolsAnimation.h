// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S85's parts of the title_art tools (docs/specs/S85-title-animation.md R50-R55): what the
// handlers of TitleArtTools.cpp and TitleArtToolsRender.cpp call for animations.

#pragma once

#include "TitleArtToolsShared.h"
#include "titleart/TitleArtChecks.h"

#include <filesystem>
#include <memory>
#include <string>

namespace jasperengine::editor {
    class TitleFlipbookBakeJob;
    struct TitleFlipbookResult;
}

namespace jasperengine::editor::mcp::titletools {

    // get_title's outline `animations` (null when the document has none) and `animation` (a name or "*")
    Json titleAnimationsOutline(Project* project, const std::filesystem::path& file, const titleart::TitleArtDoc& doc);
    bool titleAnimationGet(const titleart::TitleArtDoc& doc, const std::string& name, Json& out, std::string& error);
    // edit_title's animation_changes and the cost of each animation a batch touched (null: none)
    Json titleAnimationChanges(Project* project, const std::filesystem::path& file, const titleart::TitleArtDoc& before,
                               const titleart::TitleArtDoc& after);
    // render_title's options.animation
    bool checkAnimationOption(const Json& value, std::string& error);
    void renderTitleAnimation(const CallPtr& call, RenderSpec spec);
    // check_title: the animation, the animation renders and the asset lookups of the anim checks
    // (`keep` holds what the render function uses for the run)
    void setupAnimationChecks(TitleCheckContext& context, Project* project, const std::shared_ptr<const titleart::TitleArtDoc>& doc, const Json& args,
                              std::shared_ptr<void>& keep);
    // export_title: the flipbook job of a document with animations (null when it has none, or
    // with `error` for a bad `animations`)
    std::shared_ptr<TitleFlipbookBakeJob> titleFlipbookJob(Project* project, const std::filesystem::path& file, const titleart::TitleArtDoc& doc,
                                                           const Json& args, std::string& error);
    Json flipbookReportJson(const TitleFlipbookResult& result);
    // list_title_library {kind: "animation_presets"}
    void listAnimationPresets(const CallPtr& call);

}
