// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One draw and read-back of a private scene of the editor (ParticleEffectPreview's cells, S91;
// EffectCapture's contact sheet frames, S48): the layers drawn into a target of their own through
// Engine::drawView, never read back while the scenes still load or a pipeline is still being made
// (that draw shows nothing). Main thread, in the editor's frame.

#include "Engine.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
}

namespace jasperengine::editor {

    enum class OffscreenDraw { Done, Wait };

    // Draws `layers` (first the base) into `target` and reads its colour back as RGBA8 rows top
    // first. Wait (with what it waits for) while the view, a scene's loading, the first draw's
    // pipelines (`warmed` keeps that it drew once) or `ready` hold it; call again on a later frame.
    // `after` draws over the target once the layers are drawn, before the read-back (a screen
    // transition's overlay); false (with what it waits for) to try again on a later frame.
    OffscreenDraw drawOffscreen(uint32_t viewId, Framebuffer& target, const std::vector<Engine::ViewLayer>& layers, bool& warmed,
                                std::vector<uint8_t>& rgba, std::string& waitingFor,
                                const std::function<bool(std::string& waitingFor)>& ready = nullptr,
                                const std::function<bool(std::string& waitingFor)>& after = nullptr);

}
