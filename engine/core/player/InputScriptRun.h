// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputScriptRun_h
#define InputScriptRun_h

// A player's --input-script run (docs/specs/input-recorder-and-input-scripts.md R25, R26): the
// file loads when the engine starts, runs from the frame after "Engine ready:", writes its shots
// from the composited frame, logs [input] lines, and with --quit-after-input ends the game with
// 0, EXIT_INPUT_FAILED or EXIT_INPUT_DIVERGED.

#include "player/LaunchOptions.h"
#include "player/RunControl.h"

namespace jasperengine::player::InputScriptRun {

    // the file's fixed_fps and --after-ready, unless given (before the engine starts)
    void configure(LaunchOptions& options);
    bool isActive();
    // the engine started: the file loads (a failure logs [input] failed and quits with 10) and
    // gives its fixed_fps to `options` unless --fixed-fps did
    void engineStarted(LaunchOptions& options);
    // the first counted frame after ready: the run starts with the next frame
    void ready();
    // a shot waits for the frame drawn now
    bool wantsOffscreenFrame();
    void frameDrawn(const FrameReader& reader);
    void reset();

    // R30: an --input-script the web page fetches (same-origin) before the start scene loads: a
    // URL ("https://...") or a page path ("/tests/menu.input.json", "./menu.input.json"), not a
    // content path ("tests/Input/menu.input.json", "asset://demo/attract.input.json")
    JASPERENGINE_API bool isFetchedUrl(const std::string& path);

}

#endif
