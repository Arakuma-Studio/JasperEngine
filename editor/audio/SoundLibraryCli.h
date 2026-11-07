// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor {

    // The starter library's command-line tools, windowless: they
    // run before the backend starts (no window, no GL, no audio device) and read no project.
    //
    //   jasperengine-editor audio-analyze [options] <file|essentials:id|library:id>...
    //       the analyze_audio numbers of files and built-in clips (AudioAnalysis: the same items,
    //       with no cache and no sidecar), or --list-types, the SoundTypes.h table; the analyzer
    //       the maintainer pipeline judges with and writes analysis.json with
    //   jasperengine-editor sound-library verify|list [options]
    //       checks the installed starter library against its manifest, or lists it
    //
    // Exit codes, the convention of every audio command: 0
    // success, 1 a verification that failed (sound-library verify), 2 any error or usage error.
    // audio-analyze and sound-library list exit 0 or 2. On Windows they print to the console only as
    // jasperengine-editor-cmd, as audio-check does.
    class SoundLibraryCli {
    public:
        // argv[0] is the subcommand ("audio-analyze", "sound-library"); executableName names the
        // command in the help and the messages
        static int runAudioAnalyze(int argc, char** argv, const char* executableName);
        static int runSoundLibrary(int argc, char** argv, const char* executableName);
    };

}
