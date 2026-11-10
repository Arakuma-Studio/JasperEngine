// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R51, §6.13)
// - Purpose: the hooks of the Inspector's summary of a selected `.titleart`
//   (InspectorWindow::drawTitleArtAsset): later specs add their sections (S85's animations with
//   their cost per mode, S87's languages) and S82 the window Open in Title Designer opens; the
//   button is hidden until it is set, and a double-click then selects the asset only.
// - Thread-safety: main thread.

#pragma once

#include "ecs/Entity.h"

#include "titleart/TitleArtDoc.h"

#include <filesystem>
#include <functional>
#include <string>

namespace jasperengine::editor {

    class Project;

    namespace InspectorWindowTitleArt {

        // draws its rows below S80's sections, in registration order (`id` replaces an earlier one)
        using SummarySection = std::function<void(Project* project, const std::filesystem::path& document, const titleart::TitleArtDoc& doc)>;
        void addSummarySection(const std::string& id, SummarySection section);
        void removeSummarySection(const std::string& id);
        void drawSummarySections(Project* project, const std::filesystem::path& document, const titleart::TitleArtDoc& doc);

        // Open in Title Designer (S82 sets it); null hides the button
        void setOpenInDesigner(std::function<void(const std::filesystem::path& document)> open);
        bool canOpenInDesigner();
        void openInDesigner(const std::filesystem::path& document);
        // a Title Art Renderer's button: In Context, the title over its scene at the instance's
        // place (S82 R1, R30; S82 sets it)
        void setOpenInContext(std::function<void(const std::filesystem::path& document, uint32_t sceneId, Entity instance)> open);
        void openInContext(const std::filesystem::path& document, uint32_t sceneId, Entity instance);

    }

}
