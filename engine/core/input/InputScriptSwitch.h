// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Whether the JASPERENGINE_INPUT_SCRIPTS module is in this build (S34 module "inputscripts";
// docs/specs/input-recorder-and-input-scripts.md, Deviations): the input script interpreter, the
// recorder, checkpoints, the player's --input-script and Lua's InputRecording and InputReplay. OFF
// in a player whose project uses none of them (a release Compile build, a lite variant): the
// engine's input entry points test a constant there, and InputScriptsOff.cpp answers the few
// calls the engine makes. Undefined means on, like dialogue's switch: the Xcode project is told
// JASPERENGINE_INPUT_SCRIPTS=0 only for a player that leaves it out.
#if !defined(JASPERENGINE_INPUT_SCRIPTS) || JASPERENGINE_INPUT_SCRIPTS
    #define JASPERENGINE_INPUT_SCRIPTS_ENABLED 1
#else
    #define JASPERENGINE_INPUT_SCRIPTS_ENABLED 0
#endif
