// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENTSYSTEMNONE_H
#define UIDOCUMENTSYSTEMNONE_H

// The document system of a build without the HTML UI (JASPERENGINE_HTMLUI 0, a player variant or
// a Compile build that left the module out: docs/specs/S34-player-size.md D3). Such a build is only
// made for a project in which nothing can create a document (no UI Document component in a scene
// or prefab, no script naming the HTML UI's Lua API), so every scene's system answers exactly as a
// document system without documents does: no event is consumed, nothing is drawn, everything is
// loaded. The engine's input routing, its rendering and the script start-up call it unchanged, and
// keep their order of systems and events.

#include "ecs/SubSystem.h"
#include "math/Rect.h"
#include "render/Render.h"
#include "System.h"
#include "htmlui/HtmlUIRenderStats.h"
#include "UIDocumentInput.h"

#include <string>

namespace jasperengine{

    struct CameraComponent;
    struct Transform;

    class JASPERENGINE_API UIDocumentSystem : public SubSystem {
    public:
        explicit UIDocumentSystem(Scene* scene): SubSystem(scene) { declareNoHookTypes(); }

        void load() override {}
        void destroy() override {}
        void update(double) override {}
        void draw() override {}

        void renderOffscreen(const Rect&) {}
        void drawComposite(CameraComponent&, Transform&, PipelineType, bool, int = 1) {}
        bool isAllLoaded() const { return true; }
        const HtmlUIRenderStats& getRenderStats() const { static const HtmlUIRenderStats none; return none; }

        // the project's MSAA and glyph settings have nothing to apply to
        static void setPreferredSampleCount(int) {}
        static void setHtmlUiRenderOptions(int, bool, bool) {}

        bool eventOnPointerDown(int, int, float, float, int, bool, bool = true) { return false; }
        bool eventOnPointerUp(int, int, float, float, int, bool, bool = true) { return false; }
        bool eventOnPointerMove(int, float, float, int, bool, bool = true) { return false; }
        bool eventOnTouchCancel(int) { return false; }
        bool eventOnWheel(float, float, int) { return false; }
        void eventOnMouseLeave() {}
        bool eventOnKeyDown(int, bool, int) { return false; }
        bool eventOnKeyUp(int, int) { return false; }
        bool eventOnTextInput(const std::string&) { return false; }
        bool eventOnImeComposition(const std::string&, int) { return false; }
        void eventOnImeCancel() {}
        bool hasFocusedTextInput() const { return false; }
        bool hasPointerCapture(int, int, bool) const { return false; }
        CursorType getPointerCursor() const { return CursorType::ARROW; }
        bool eventOnGamepadKeyDown(int, GamepadUIKey, bool) { return false; }
        bool eventOnGamepadKeyUp(int, GamepadUIKey) { return false; }
        bool hasGamepadFocus() const { return false; }
        bool dispatchWorldPointer(int, int, float, float, int, PointerPhase, bool, bool = false) { return false; }

        void startDocumentScripts() {}
        void releaseScriptState() {}
    };

}

#endif //UIDOCUMENTSYSTEMNONE_H
