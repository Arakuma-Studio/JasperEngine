// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef DIALOGUE_DIALOGUESWITCH_H
#define DIALOGUE_DIALOGUESWITCH_H

// Whether the JASPERENGINE_DIALOGUE module is in this build (docs/specs/dialogue-graphs.md R15). OFF
// leaves engine/core/dialogue out of the build (DialogueSources.cmake) and binds the Lua names as stubs.
// Undefined means on, like noise's switch: the Xcode project is told JASPERENGINE_DIALOGUE=0 only
// when an export leaves it out.

#if !defined(JASPERENGINE_DIALOGUE) || JASPERENGINE_DIALOGUE
    #define JASPERENGINE_DIALOGUE_ENABLED 1
#else
    #define JASPERENGINE_DIALOGUE_ENABLED 0
#endif

#endif // DIALOGUE_DIALOGUESWITCH_H
