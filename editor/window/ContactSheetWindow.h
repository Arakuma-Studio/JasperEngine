// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Game view's Contact Sheet... window (docs/specs/S48-pixel-fx.md R40, R27): capture_contact_sheet's
// targets (the selected GameObject, an .anim clip on it, a .particles asset, a transition kind with its
// options) with Frames, Duration, Backdrops, Scale, Mode and Lints; it shows the sheet and the checks, and
// Save As... writes the PNG as one undo step.

#include "ecs/Entity.h"
#include "pixelart/ScreenTransition.h"
#include "texture/Texture.h"
#include "window/PaletteWindow.h"

#include "pixelart/PixelImage.h"
#include "pixelart/lint/LintTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    namespace pixelart::fx {
        class EffectCapture;
    }

    class ContactSheetWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Contact Sheet";

        explicit ContactSheetWindow(Project* project);
        ~ContactSheetWindow();

        // for a GameObject of a scene (the Game view's selection)
        void open(uint32_t sceneId, Entity entity);
        void show();
        bool isOpen() const { return windowOpen; }

    private:
        enum class TargetKind { GameObject, Clip, Particles, Transition };

        void capture();
        void collect();
        void saveAs();
        void scanTargetFiles();
        void drawTargetRow(bool& canCapture);
        void drawTransitionOptions();
        std::string sheetName() const;

        Project* project = nullptr;
        bool windowOpen = false;
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        TargetKind targetKind = TargetKind::GameObject;
        std::vector<std::string> clipFiles;      // project-relative .anim paths
        std::vector<std::string> particleFiles;  // assets-relative .particles paths
        std::string clipPath, particlesPath;     // the picked ones
        jasperengine::pixelart::TransitionOptions transition;
        jasperengine::pixelart::TransitionKind transitionKind = jasperengine::pixelart::TransitionKind::IRIS;
        int transitionPhase = 0;                 // Play, Cover, Reveal
        int artWidth = 160, artHeight = 90;
        int frames = 8;
        float duration = 1.0f;
        bool loop = false;
        bool sceneMode = false;
        bool lints = true;
        int scale = 0;
        bool backdrops[7] = {true, false, false, false, false, false, false};
        std::shared_ptr<pixelart::fx::EffectCapture> running;
        pixelart::PixelImage sheet;
        pixelart::lint::LintReport report;
        std::string verdict;
        std::string notice;
        bool noticeIsError = false;
        PaletteGpuImage image;
        std::vector<Texture> retired;
    };

}
